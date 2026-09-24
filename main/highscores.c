#include "highscores.h"
#include "renderer.h"
#include "controls.h"
#include "sound.h"
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Almacenamiento en flash
// ---------------------------------------------------------------------------
// Último sector de flash disponible. PICO_FLASH_SIZE_BYTES lo define el
// SDK según el board real (2MB en "pico", 4MB en "pico2", etc.) -- a
// diferencia de ArcadePi, que asumía 2MB fijo, esto se ajusta solo.
#define HS_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define HS_MAGIC   0x48534332u // "HSC2"
#define HS_VERSION 1

typedef struct {
    uint32_t magic;
    uint32_t version;
    ScoreTable tables[HS_MAX_GAMES];
    uint8_t _pad[FLASH_SECTOR_SIZE - sizeof(uint32_t) * 2 - sizeof(ScoreTable) * HS_MAX_GAMES];
} FlashData;

static_assert(sizeof(FlashData) == FLASH_SECTOR_SIZE, "FlashData debe ocupar exactamente un sector");

static ScoreTable g_tables[HS_MAX_GAMES];
static bool g_save_pending = false;

// ---------------------------------------------------------------------------
// Escritura en flash
// ---------------------------------------------------------------------------
// Debe ejecutarse desde RAM (__no_inline_not_in_flash_func): mientras la
// flash está borrándose/programándose, la CPU no puede buscar en ella
// las siguientes instrucciones (ejecución XIP), así que el propio código
// que hace el borrado/programado no puede vivir en flash.
//
// A diferencia de ArcadePi (que además debía parar el DMA+PIO del vídeo
// compuesto, porque leen flash de forma continua para generar la señal),
// aquí basta con desactivar interrupciones: el renderer solo transmite
// por SPI bajo demanda (renderer_flush()), no hay nada leyendo flash en
// segundo plano durante el guardado.
static void __no_inline_not_in_flash_func(save_to_flash)(void) {
    static FlashData buf;
    buf.magic = HS_MAGIC;
    buf.version = HS_VERSION;
    memcpy(buf.tables, g_tables, sizeof(g_tables));
    memset(buf._pad, 0xFF, sizeof(buf._pad));

    uint32_t irq = save_and_disable_interrupts();
    flash_range_erase(HS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(HS_FLASH_OFFSET, (const uint8_t *)&buf, FLASH_SECTOR_SIZE);
    restore_interrupts(irq);

    g_save_pending = false;
}

void highscores_init(void) {
    const FlashData *stored = (const FlashData *)(XIP_BASE + HS_FLASH_OFFSET);

    if (stored->magic == HS_MAGIC && stored->version == HS_VERSION) {
        memcpy(g_tables, stored->tables, sizeof(g_tables));
    } else {
        memset(g_tables, 0, sizeof(g_tables));
    }
    g_save_pending = false;
}

const ScoreTable *highscores_get(int game_id) {
    if (game_id < 0 || game_id >= HS_MAX_GAMES) return NULL;
    return &g_tables[game_id];
}

bool highscores_is_top(int game_id, uint32_t score) {
    if (game_id < 0 || game_id >= HS_MAX_GAMES) return false;
    const ScoreTable *t = &g_tables[game_id];
    if (t->count < HS_TOP_SCORES) return true;
    return score > t->entries[HS_TOP_SCORES - 1].score;
}

void highscores_add(int game_id, const char *name, uint32_t score) {
    if (game_id < 0 || game_id >= HS_MAX_GAMES) return;
    ScoreTable *t = &g_tables[game_id];

    int insert_at = t->count;
    for (int i = 0; i < t->count; i++) {
        if (score > t->entries[i].score) { insert_at = i; break; }
    }
    if (insert_at >= HS_TOP_SCORES) return; // no entra en el top

    int last = (t->count < HS_TOP_SCORES) ? t->count : HS_TOP_SCORES - 1;
    for (int i = last; i > insert_at; i--) {
        t->entries[i] = t->entries[i - 1];
    }

    strncpy(t->entries[insert_at].name, name, HS_NAME_LEN);
    t->entries[insert_at].name[HS_NAME_LEN] = '\0';
    t->entries[insert_at].score = score;

    if (t->count < HS_TOP_SCORES) t->count++;

    g_save_pending = true;
}

void highscores_flush(void) {
    if (g_save_pending) {
        save_to_flash();
    }
}

void highscores_reset(void) {
    memset(g_tables, 0, sizeof(g_tables));
    g_save_pending = true;
    highscores_flush();
}

// ---------------------------------------------------------------------------
// Dibujo
// ---------------------------------------------------------------------------
static int hs_centered_x(const char *text, int scale) {
    int w = (int)st7789_text_width(text, (uint8_t)scale);
    int x = (TFT_WIDTH - w) / 2;
    return (x < 0) ? 0 : x;
}

void highscores_draw(int game_id, const char *title, int top_y) {
    char buf[32];
    const int title_scale = 2;
    const int row_scale = 2;
    const int row_height = 22;

    renderer_draw_text(hs_centered_x(title, title_scale), top_y, title,
                        COLOR_CYAN, COLOR_BLACK, title_scale);

    const ScoreTable *t = highscores_get(game_id);
    int y = top_y + 26;

    for (int i = 0; i < HS_TOP_SCORES; i++) {
        if (t && i < t->count) {
            snprintf(buf, sizeof(buf), "%d. %s %lu", i + 1,
                     t->entries[i].name, (unsigned long)t->entries[i].score);
        } else {
            snprintf(buf, sizeof(buf), "%d. --- 0", i + 1);
        }
        renderer_draw_text(hs_centered_x(buf, row_scale), y, buf,
                            COLOR_WHITE, COLOR_BLACK, row_scale);
        y += row_height;
    }
}

// ---------------------------------------------------------------------------
// Entrada de iniciales (bloqueante)
// ---------------------------------------------------------------------------
// Adaptado a los controles de ArcadeColor (controls_update/menu_up/
// menu_down/menu_select) en vez de los globales de encoder/botón crudos
// que usaba ArcadePi -- misma idea (gira para cambiar de letra, pulsa
// para confirmar), pero reutilizando lo que ya tiene el proyecto.
#define HS_LETTERS_LEN 27 // 'A'-'Z' + espacio
static char hs_letter_at(int idx) {
    idx = ((idx % HS_LETTERS_LEN) + HS_LETTERS_LEN) % HS_LETTERS_LEN;
    return (idx == HS_LETTERS_LEN - 1) ? ' ' : (char)('A' + idx);
}

void highscores_enter(int game_id, uint32_t score) {
    char name[HS_NAME_LEN + 1] = "AAA";
    int letter_idx[HS_NAME_LEN] = {0, 0, 0};
    int cursor = 0;

    while (cursor < HS_NAME_LEN) {
        renderer_clear(COLOR_BLACK);

        char line1[32];
        snprintf(line1, sizeof(line1), "PUNTUACION: %lu", (unsigned long)score);
        renderer_draw_text(hs_centered_x(line1, 2), 60, line1, COLOR_YELLOW, COLOR_BLACK, 2);
        renderer_draw_text(hs_centered_x("INTRODUCE TUS INICIALES", 1), 100,
                            "INTRODUCE TUS INICIALES", COLOR_WHITE, COLOR_BLACK, 1);

        // Las 3 letras, resaltando la que se está editando
        int total_w = HS_NAME_LEN * (int)st7789_text_width("A", 4) + (HS_NAME_LEN - 1) * 10;
        int x = (TFT_WIDTH - total_w) / 2;
        if (x < 0) x = 0;

        for (int i = 0; i < HS_NAME_LEN; i++) {
            char c[2] = { hs_letter_at(letter_idx[i]), '\0' };
            uint16_t color = (i == cursor) ? COLOR_YELLOW : COLOR_WHITE;
            renderer_draw_char(x, 140, c[0], color, COLOR_BLACK, 4);
            x += (int)st7789_text_width("A", 4) + 10;
        }
        renderer_flush();

        controls_update();
        sound_update(); // sin esto, cualquier sonido con apagado automático
                         // pendiente (p.ej. el tono de "game over") nunca
                         // llega a comprobarse mientras se escriben las
                         // iniciales, y se queda sonando indefinidamente
        if (controls_menu_up())   letter_idx[cursor]++;
        if (controls_menu_down()) letter_idx[cursor]--;
        if (controls_menu_select()) cursor++;
        sleep_ms(15);
    }

    for (int i = 0; i < HS_NAME_LEN; i++) {
        name[i] = hs_letter_at(letter_idx[i]);
    }
    name[HS_NAME_LEN] = '\0';

    highscores_add(game_id, name, score);
}
