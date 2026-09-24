#include "highscores.h"
#include "renderer.h"
#include "controls.h"
#include "sound.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"

#include <string.h>
#include <stdio.h>

/* ===========================================================
 * PORT A ESP32 - lo que cambia respecto al original:
 *
 *  - Almacenamiento: flash_range_erase/program de un sector fijo
 *    (Pico) -> un unico blob NVS con toda la tabla de records.
 *    No hace falta la struct FlashData con padding a sector
 *    completo ni el static_assert de tamano: NVS gestiona el
 *    tamano real del blob por su cuenta.
 *  - No hace falta __no_inline_not_in_flash_func ni desactivar
 *    interrupciones: esa necesidad era especifica de escribir en
 *    la flash donde vive el propio codigo en ejecucion (XIP) de
 *    la Pico. NVS en ESP32 no tiene esa restriccion.
 *  - sleep_ms() (Pico SDK) -> vTaskDelay() (FreeRTOS).
 *
 * TODO LO DEMAS (logica de inserccion ordenada en el top-5,
 * dibujado, entrada de iniciales) es la MISMA logica que el
 * highscores.c original: no toca hardware, es C puro.
 * =========================================================== */

#define NVS_NAMESPACE "arcadecolor"
#define NVS_KEY_TABLES "hs_tables"
#define HS_VERSION 1

static ScoreTable g_tables[HS_MAX_GAMES];
static bool g_save_pending = false;

/* ---------------------------------------------------------
 * NVS
 * --------------------------------------------------------- */
void highscores_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    memset(g_tables, 0, sizeof(g_tables));

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(g_tables);
        // Si el blob guardado no mide exactamente lo que esperamos
        // (p.ej. tras cambiar HS_MAX_GAMES/HS_TOP_SCORES entre
        // versiones), lo descartamos y arrancamos con tablas
        // vacias, igual que el original descartaba un magic/version
        // que no coincidiera.
        esp_err_t rd = nvs_get_blob(h, NVS_KEY_TABLES, g_tables, &len);
        if (rd != ESP_OK || len != sizeof(g_tables)) {
            memset(g_tables, 0, sizeof(g_tables));
        }
        nvs_close(h);
    }

    g_save_pending = false;
}

static void save_to_nvs(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;

    nvs_set_blob(h, NVS_KEY_TABLES, g_tables, sizeof(g_tables));
    nvs_commit(h);
    nvs_close(h);

    g_save_pending = false;
}

/* ---------------------------------------------------------
 * Consulta / insercion (identico al original)
 * --------------------------------------------------------- */
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
        save_to_nvs();
    }
}

void highscores_reset(void) {
    memset(g_tables, 0, sizeof(g_tables));
    g_save_pending = true;
    highscores_flush();
}

/* ---------------------------------------------------------
 * Dibujo (identico al original)
 * --------------------------------------------------------- */
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

/* ---------------------------------------------------------
 * Entrada de iniciales (bloqueante) -- identica al original salvo
 * sleep_ms -> vTaskDelay.
 * --------------------------------------------------------- */
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
        vTaskDelay(pdMS_TO_TICKS(15));
    }

    for (int i = 0; i < HS_NAME_LEN; i++) {
        name[i] = hs_letter_at(letter_idx[i]);
    }
    name[HS_NAME_LEN] = '\0';

    highscores_add(game_id, name, score);
}