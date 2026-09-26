#include "menu.h"
#include "renderer.h"
#include "controls.h"
#include "games/games_list.h"
#include "highscores.h"
#include "sound.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <math.h>
#include <stdio.h>

/*
 * PORT A ESP32: unico cambio estructural respecto al original --
 * sleep_ms() (Pico SDK) -> vTaskDelay(), y absolute_time_t/
 * absolute_time_diff_us() -> milisegundos con esp_timer_get_time()
 * (ver show_scores_screen). Ademas, run_test_controls() y
 * run_clear_scores_confirm() usaban funciones de depuracion de
 * encoder (controls_debug_raw_count/pending) que no existen con
 * joystick -- adaptadas mas abajo, con su propio comentario.
 *
 * TODO LO DEMAS (selector tipo carrete, animaciones, submenu de
 * opciones, attract mode) es la MISMA logica que el menu.c
 * original.
 */

/*
 * Selector tipo "carrete": el juego seleccionado aparece grande y
 * centrado, con el anterior y el siguiente arriba/abajo en letra
 * pequeña. La lista es circular: tras el último elemento vuelve al
 * primero, y viceversa.
 *
 * Pantalla real: 320 (ancho) x 240 (alto), apaisada.
 *
 * Última entrada del carrete: "OPCIONES" (TOTAL_ITEMS = NUM_GAMES + 1).
 */

#define TITLE_SCALE 3
#define PREV_NEXT_SCALE 2
#define SELECTED_SCALE_MAX 4
#define TITLE_Y 8
#define DIVIDER_Y 36

#define CENTER_Y 120
#define ITEM_SPACING 50

#define ANIM_STEPS 5
#define ANIM_STEP_DELAY_MS 12

#define TOTAL_ITEMS (NUM_GAMES + 1)
#define OPTIONS_INDEX NUM_GAMES

#define IDLE_TIMEOUT_MS (30 * 1000)
#define SCORES_DISPLAY_MS (5 * 1000)
#define ATTRACT_DEMO_MODE GAME_MODE_DEMO

static const char *PROJECT_TITLE = "ARCADE COLOR";

static inline uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static int wrap_index(int idx)
{
    idx %= TOTAL_ITEMS;
    if (idx < 0) idx += TOTAL_ITEMS;
    return idx;
}

static const char *game_name_at(int idx)
{
    idx = wrap_index(idx);
    if (idx == OPTIONS_INDEX) return "OPCIONES";
    return games_list[idx].name;
}

static int best_fit_scale(const char *text, int max_scale)
{
    for (int scale = max_scale; scale > 1; scale--) {
        if (st7789_text_width(text, (uint8_t)scale) <= TFT_WIDTH - 10) {
            return scale;
        }
    }
    return 1;
}

static int centered_x(const char *text, int scale)
{
    int text_width = (int)st7789_text_width(text, (uint8_t)scale);
    int x = (TFT_WIDTH - text_width) / 2;
    return (x < 0) ? 0 : x;
}

static void clear_row(int y)
{
    int top_margin = 4;
    int bottom_margin = 6;
    int height = top_margin + FONT_HEIGHT * SELECTED_SCALE_MAX + bottom_margin;
    renderer_fill_rect(0, y - top_margin, TFT_WIDTH, height, COLOR_BLACK);
}

static void draw_row(int y, const char *text, int scale, uint16_t color)
{
    renderer_draw_text(centered_x(text, scale), y, text, color, COLOR_BLACK, scale);
}

static void draw_title(void)
{
    renderer_fill_rect(0, 0, TFT_WIDTH, DIVIDER_Y + 2, COLOR_BLACK);
    renderer_draw_text(centered_x(PROJECT_TITLE, TITLE_SCALE), TITLE_Y, PROJECT_TITLE,
                        COLOR_CYAN, COLOR_BLACK, TITLE_SCALE);
    renderer_fill_rect(10, DIVIDER_Y, TFT_WIDTH - 20, 2, COLOR_CYAN);
}

#define TITLE_WAVE_AMPLITUDE 4
#define TITLE_WAVE_SPEED 0.28f
#define TITLE_WAVE_CHAR_PHASE 0.6f
#define TITLE_COLOR_CYCLE_EVERY 6
#define TITLE_ANIM_THROTTLE 3

static const uint16_t title_palette[] = {
    COLOR_CYAN, COLOR_YELLOW, COLOR_MAGENTA, COLOR_GREEN, COLOR_RED, COLOR_WHITE
};
#define TITLE_PALETTE_LEN (int)(sizeof(title_palette) / sizeof(title_palette[0]))
#define TITLE_BAND_HEIGHT (FONT_HEIGHT * TITLE_SCALE + 2 * TITLE_WAVE_AMPLITUDE + 4)

static float title_wave_phase = 0.0f;
static int title_color_phase = 0;

static void draw_title_wave(void)
{
    int text_w = (int)st7789_text_width(PROJECT_TITLE, TITLE_SCALE);
    int x = (TFT_WIDTH - text_w) / 2;
    if (x < 0) x = 0;

    int band_y = TITLE_Y - TITLE_WAVE_AMPLITUDE - 2;
    renderer_fill_rect(0, band_y, TFT_WIDTH, TITLE_BAND_HEIGHT, COLOR_BLACK);

    for (int i = 0; PROJECT_TITLE[i] != '\0'; i++) {
        float angle = title_wave_phase + i * TITLE_WAVE_CHAR_PHASE;
        int y_offset = (int)(sinf(angle) * TITLE_WAVE_AMPLITUDE);
        uint16_t color = title_palette[(i + title_color_phase) % TITLE_PALETTE_LEN];

        renderer_draw_char(x, TITLE_Y + y_offset, PROJECT_TITLE[i], color, COLOR_BLACK, TITLE_SCALE);
        x += (FONT_WIDTH + 1) * TITLE_SCALE;
    }
}

static void update_title_animation(void)
{
    static int throttle_counter = 0;
    static int color_tick = 0;

    throttle_counter++;
    if (throttle_counter < TITLE_ANIM_THROTTLE) return;
    throttle_counter = 0;

    title_wave_phase += TITLE_WAVE_SPEED;
    if (title_wave_phase > 6.2831853f) title_wave_phase -= 6.2831853f;

    color_tick++;
    if (color_tick >= TITLE_COLOR_CYCLE_EVERY) {
        color_tick = 0;
        title_color_phase = (title_color_phase + 1) % TITLE_PALETTE_LEN;
    }

    draw_title_wave();
    renderer_flush();
}

static void draw_settled(int selected)
{
    clear_row(CENTER_Y - ITEM_SPACING);
    clear_row(CENTER_Y);
    clear_row(CENTER_Y + ITEM_SPACING);

    const char *name = game_name_at(selected);

    draw_row(CENTER_Y - ITEM_SPACING, game_name_at(selected - 1), PREV_NEXT_SCALE, COLOR_WHITE);
    draw_row(CENTER_Y, name, best_fit_scale(name, SELECTED_SCALE_MAX), COLOR_YELLOW);
    draw_row(CENTER_Y + ITEM_SPACING, game_name_at(selected + 1), PREV_NEXT_SCALE, COLOR_WHITE);
}

static void animate_transition(int old_selected, int new_selected, int direction)
{
    const char *slide_center = game_name_at(old_selected);
    const char *slide_edge = game_name_at(new_selected);

    int y_center_start = CENTER_Y;
    int y_center_end = CENTER_Y - direction * ITEM_SPACING;
    int y_edge_start = CENTER_Y + direction * ITEM_SPACING;
    int y_edge_end = CENTER_Y;

    int y_vacating = (direction > 0) ? (CENTER_Y - ITEM_SPACING) : (CENTER_Y + ITEM_SPACING);
    int y_revealing = (direction > 0) ? (CENTER_Y + ITEM_SPACING) : (CENTER_Y - ITEM_SPACING);

    clear_row(y_vacating);
    clear_row(y_revealing);

    int y_center_prev = y_center_start;
    int y_edge_prev = y_edge_start;

    for (int step = 1; step <= ANIM_STEPS; step++) {
        int y_center = y_center_start + (y_center_end - y_center_start) * step / ANIM_STEPS;
        int y_edge = y_edge_start + (y_edge_end - y_edge_start) * step / ANIM_STEPS;

        clear_row(y_center_prev);
        clear_row(y_edge_prev);
        clear_row(y_center);
        clear_row(y_edge);

        draw_row(y_center, slide_center, PREV_NEXT_SCALE, COLOR_WHITE);
        draw_row(y_edge, slide_edge, PREV_NEXT_SCALE, COLOR_WHITE);

        renderer_flush();

        y_center_prev = y_center;
        y_edge_prev = y_edge;

        sound_update(); // la musica sigue avanzando durante la animacion

        vTaskDelay(pdMS_TO_TICKS(ANIM_STEP_DELAY_MS));
    }
}

/*
 * ===========================================================
 * SUBMENÚ DE OPCIONES
 * ===========================================================
 */

#define FIRMWARE_VERSION "v1.0-esp32"
#define FIRMWARE_AUTHOR  "JLP"

typedef enum {
    OPT_TEST_CONTROLS = 0,
    OPT_TEST_SCREEN,
    OPT_TEST_SOUND,
    OPT_CLEAR_SCORES,
    OPT_ABOUT,
    OPT_BACK,
    OPT_COUNT
} options_item_t;

static const char *options_item_names[OPT_COUNT] = {
    "PRUEBA CONTROLES",
    "PRUEBA PANTALLA",
    "PRUEBA SONIDO",
    "BORRAR RECORDS",
    "ACERCA DE",
    "VOLVER"
};

#define OPTIONS_ITEM_Y_START 50
#define OPTIONS_ITEM_SPACING 26
#define OPTIONS_ITEM_SCALE 2

static void draw_options_menu(int selected)
{
    renderer_clear(COLOR_BLACK);
    renderer_draw_text(centered_x("OPCIONES", TITLE_SCALE), TITLE_Y, "OPCIONES",
                        COLOR_CYAN, COLOR_BLACK, TITLE_SCALE);
    renderer_fill_rect(10, DIVIDER_Y, TFT_WIDTH - 20, 2, COLOR_CYAN);

    for (int i = 0; i < OPT_COUNT; i++) {
        const char *text = options_item_names[i];
        uint16_t color = (i == selected) ? COLOR_YELLOW : COLOR_WHITE;
        renderer_draw_text(centered_x(text, OPTIONS_ITEM_SCALE),
                            OPTIONS_ITEM_Y_START + i * OPTIONS_ITEM_SPACING,
                            text, color, COLOR_BLACK, OPTIONS_ITEM_SCALE);
    }

    renderer_flush();
}

/*
 * -----------------------------------------------------------
 * PRUEBA CONTROLES
 *
 * PORT A ESP32: el original mostraba dos encoders (RAW/PEND/
 * DELTA/DETENT via controls_debug_raw_count/pending, que no
 * existen aqui -- no hay encoders). Sustituido por dos cajas de
 * "JOYSTICK 1/2" con el valor crudo del ADC y el normalizado
 * (-1000..1000) de cada eje X/Y, via controls_debug_axis_raw/
 * normalized() (controls.h). El grid de 6 botones y la combinacion
 * de salida (mantener J1-SW + J2-SW) no cambian: los indices 0-5
 * ya coinciden exactamente con nuestros botones reales.
 * -----------------------------------------------------------
 */

#define TEST_CTRL_EXIT_HOLD_MS 1000
#define TEST_CTRL_BTN_COUNT 6
#define TEST_CTRL_IDX_J1_SW 4
#define TEST_CTRL_IDX_J2_SW 5

static const char *test_ctrl_btn_labels[TEST_CTRL_BTN_COUNT] = {
    "J1-A", "J1-B", "J2-A", "J2-B", "J1-SW", "J2-SW"
};

#define TEST_CTRL_BTN_W 46
#define TEST_CTRL_BTN_H 28
#define TEST_CTRL_BTN_Y 185
#define TEST_CTRL_BTN_GAP 6

static int test_ctrl_btn_x(int i)
{
    int total_w = TEST_CTRL_BTN_COUNT * TEST_CTRL_BTN_W + (TEST_CTRL_BTN_COUNT - 1) * TEST_CTRL_BTN_GAP;
    int start_x = (TFT_WIDTH - total_w) / 2;
    return start_x + i * (TEST_CTRL_BTN_W + TEST_CTRL_BTN_GAP);
}

static void test_ctrl_draw_button(int i, bool pressed)
{
    int x = test_ctrl_btn_x(i);
    int y = TEST_CTRL_BTN_Y;
    uint16_t fill = pressed ? COLOR_GREEN : COLOR_BLACK;

    renderer_fill_rect(x, y, TEST_CTRL_BTN_W, TEST_CTRL_BTN_H, fill);
    renderer_fill_rect(x, y, TEST_CTRL_BTN_W, 2, COLOR_WHITE);
    renderer_fill_rect(x, y + TEST_CTRL_BTN_H - 2, TEST_CTRL_BTN_W, 2, COLOR_WHITE);
    renderer_fill_rect(x, y, 2, TEST_CTRL_BTN_H, COLOR_WHITE);
    renderer_fill_rect(x + TEST_CTRL_BTN_W - 2, y, 2, TEST_CTRL_BTN_H, COLOR_WHITE);

    const char *label = test_ctrl_btn_labels[i];
    uint16_t text_color = pressed ? COLOR_BLACK : COLOR_WHITE;
    int text_w = (int)st7789_text_width(label, 1);

    renderer_draw_text(x + (TEST_CTRL_BTN_W - text_w) / 2,
                        y + (TEST_CTRL_BTN_H - FONT_HEIGHT) / 2,
                        label, text_color, fill, 1);
}

#define TEST_CTRL_JOY_Y 56
#define TEST_CTRL_JOY_W 140
#define TEST_CTRL_JOY_H 112
#define TEST_CTRL_JOY_GAP 20

static int test_ctrl_joy_x(int idx)
{
    int total_w = 2 * TEST_CTRL_JOY_W + TEST_CTRL_JOY_GAP;
    int start_x = (TFT_WIDTH - total_w) / 2;
    return start_x + idx * (TEST_CTRL_JOY_W + TEST_CTRL_JOY_GAP);
}

static void test_ctrl_draw_joy_frame(int idx)
{
    int x = test_ctrl_joy_x(idx);
    int y = TEST_CTRL_JOY_Y;

    renderer_fill_rect(x, y, TEST_CTRL_JOY_W, 2, COLOR_CYAN);
    renderer_fill_rect(x, y + TEST_CTRL_JOY_H - 2, TEST_CTRL_JOY_W, 2, COLOR_CYAN);
    renderer_fill_rect(x, y, 2, TEST_CTRL_JOY_H, COLOR_CYAN);
    renderer_fill_rect(x + TEST_CTRL_JOY_W - 2, y, 2, TEST_CTRL_JOY_H, COLOR_CYAN);

    char label[16];
    snprintf(label, sizeof(label), "JOYSTICK %d", (unsigned int) idx + 1);
    renderer_draw_text(x + 6, y + 6, label, COLOR_CYAN, COLOR_BLACK, 1);
}

// axis_x_id/axis_y_id: 0/1 para J1, 2/3 para J2 (ver controls.c)
static void test_ctrl_draw_joy_stats(int idx, int axis_x_id, int axis_y_id)
{
    int x = test_ctrl_joy_x(idx) + 6;
    int y = TEST_CTRL_JOY_Y + 24;
    int line_h = 16;
    char buf[24];

    renderer_fill_rect(x, y, TEST_CTRL_JOY_W - 12, line_h * 4, COLOR_BLACK);

    snprintf(buf, sizeof(buf), "X RAW:  %d", controls_debug_axis_raw(axis_x_id));
    renderer_draw_text(x, y, buf, COLOR_WHITE, COLOR_BLACK, 1);

    snprintf(buf, sizeof(buf), "X NORM: %d", controls_debug_axis_normalized(axis_x_id));
    renderer_draw_text(x, y + line_h, buf, COLOR_WHITE, COLOR_BLACK, 1);

    snprintf(buf, sizeof(buf), "Y RAW:  %d", controls_debug_axis_raw(axis_y_id));
    renderer_draw_text(x, y + line_h * 2, buf, COLOR_YELLOW, COLOR_BLACK, 1);

    snprintf(buf, sizeof(buf), "Y NORM: %d", controls_debug_axis_normalized(axis_y_id));
    renderer_draw_text(x, y + line_h * 3, buf, COLOR_YELLOW, COLOR_BLACK, 1);
}

static void run_test_controls(void)
{
    renderer_clear(COLOR_BLACK);
    renderer_draw_text(centered_x("PRUEBA CONTROLES", 2), TITLE_Y, "PRUEBA CONTROLES",
                        COLOR_CYAN, COLOR_BLACK, 2);
    renderer_fill_rect(10, DIVIDER_Y, TFT_WIDTH - 20, 2, COLOR_CYAN);

    test_ctrl_draw_joy_frame(0);
    test_ctrl_draw_joy_frame(1);

    for (int i = 0; i < TEST_CTRL_BTN_COUNT; i++) {
        test_ctrl_draw_button(i, false);
    }

    static const char *exit_hint = "MANTEN J1-SW + J2-SW PARA SALIR";
    renderer_draw_text(centered_x(exit_hint, 1), TEST_CTRL_BTN_Y + TEST_CTRL_BTN_H + 8,
                        exit_hint, COLOR_YELLOW, COLOR_BLACK, 1);

    renderer_flush();

    bool prev_pressed[TEST_CTRL_BTN_COUNT] = { false };
    int hold_ms = 0;

    while (true) {
        controls_update();
        sound_update();

        // axis_id: 0=J1X 1=J1Y 2=J2X 3=J2Y (ver controls.c)
        test_ctrl_draw_joy_stats(0, 0, 1);
        test_ctrl_draw_joy_stats(1, 2, 3);

        for (int i = 0; i < TEST_CTRL_BTN_COUNT; i++) {
            bool pressed = controls_button_down(i);
            if (pressed != prev_pressed[i]) {
                test_ctrl_draw_button(i, pressed);
                prev_pressed[i] = pressed;
            }
        }

        renderer_flush();

        if (controls_button_down(TEST_CTRL_IDX_J1_SW) && controls_button_down(TEST_CTRL_IDX_J2_SW)) {
            hold_ms += 15;
            if (hold_ms >= TEST_CTRL_EXIT_HOLD_MS) break;
        } else {
            hold_ms = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

/*
 * -----------------------------------------------------------
 * PRUEBA PANTALLA
 * -----------------------------------------------------------
 */

typedef enum {
    TEST_PATTERN_BARS = 0,
    TEST_PATTERN_GRID,
    TEST_PATTERN_GRADIENT,
    TEST_PATTERN_RED,
    TEST_PATTERN_GREEN,
    TEST_PATTERN_BLUE,
    TEST_PATTERN_COUNT
} test_pattern_t;

static const char *test_pattern_names[TEST_PATTERN_COUNT] = {
    "BARRAS DE COLOR", "REJILLA + CRUCETA", "GRADIENTE DE GRISES",
    "ROJO SOLIDO", "VERDE SOLIDO", "AZUL SOLIDO"
};

static uint16_t test_gray565(int level5)
{
    if (level5 < 0) level5 = 0;
    if (level5 > 31) level5 = 31;
    uint16_t r = (uint16_t)level5;
    uint16_t g = (uint16_t)((level5 * 63) / 31);
    uint16_t b = (uint16_t)level5;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void test_pattern_draw_bars(void)
{
    static const uint16_t bar_colors[] = {
        COLOR_WHITE, COLOR_YELLOW, COLOR_CYAN, COLOR_GREEN,
        COLOR_MAGENTA, COLOR_RED, COLOR_BLUE, COLOR_BLACK
    };

    int n = (int)(sizeof(bar_colors) / sizeof(bar_colors[0]));
    int bar_w = TFT_WIDTH / n;
    int bar_h = TFT_HEIGHT - 30;

    for (int i = 0; i < n; i++) {
        int w = (i == n - 1) ? (TFT_WIDTH - i * bar_w) : bar_w;
        renderer_fill_rect(i * bar_w, 0, w, bar_h, bar_colors[i]);
    }

    int steps = 8;
    int step_w = TFT_WIDTH / steps;
    for (int i = 0; i < steps; i++) {
        int level = (i * 31) / (steps - 1);
        int w = (i == steps - 1) ? (TFT_WIDTH - i * step_w) : step_w;
        renderer_fill_rect(i * step_w, bar_h, w, TFT_HEIGHT - bar_h, test_gray565(level));
    }
}

static void test_pattern_draw_grid(void)
{
    renderer_fill_rect(0, 0, TFT_WIDTH, 2, COLOR_WHITE);
    renderer_fill_rect(0, TFT_HEIGHT - 2, TFT_WIDTH, 2, COLOR_WHITE);
    renderer_fill_rect(0, 0, 2, TFT_HEIGHT, COLOR_WHITE);
    renderer_fill_rect(TFT_WIDTH - 2, 0, 2, TFT_HEIGHT, COLOR_WHITE);

    for (int x = 20; x < TFT_WIDTH; x += 20) renderer_fill_rect(x, 0, 1, TFT_HEIGHT, COLOR_CYAN);
    for (int y = 20; y < TFT_HEIGHT; y += 20) renderer_fill_rect(0, y, TFT_WIDTH, 1, COLOR_CYAN);

    int cx = TFT_WIDTH / 2;
    int cy = TFT_HEIGHT / 2;
    renderer_fill_rect(cx - 20, cy - 1, 40, 2, COLOR_YELLOW);
    renderer_fill_rect(cx - 1, cy - 20, 2, 40, COLOR_YELLOW);

    int m = 14;
    renderer_fill_rect(4, 4, m, 2, COLOR_RED);
    renderer_fill_rect(4, 4, 2, m, COLOR_RED);
    renderer_fill_rect(TFT_WIDTH - 4 - m, 4, m, 2, COLOR_RED);
    renderer_fill_rect(TFT_WIDTH - 6, 4, 2, m, COLOR_RED);
    renderer_fill_rect(4, TFT_HEIGHT - 6, m, 2, COLOR_RED);
    renderer_fill_rect(4, TFT_HEIGHT - 4 - m, 2, m, COLOR_RED);
    renderer_fill_rect(TFT_WIDTH - 4 - m, TFT_HEIGHT - 6, m, 2, COLOR_RED);
    renderer_fill_rect(TFT_WIDTH - 6, TFT_HEIGHT - 4 - m, 2, m, COLOR_RED);
}

static void test_pattern_draw_gradient(void)
{
    int steps = 32;
    int step_w = TFT_WIDTH / steps;
    for (int i = 0; i < steps; i++) {
        int level = (i * 31) / (steps - 1);
        int w = (i == steps - 1) ? (TFT_WIDTH - i * step_w) : step_w;
        renderer_fill_rect(i * step_w, 0, w, TFT_HEIGHT, test_gray565(level));
    }
}

static void draw_test_pattern(int idx)
{
    renderer_clear(COLOR_BLACK);

    switch (idx) {
        case TEST_PATTERN_BARS:     test_pattern_draw_bars();     break;
        case TEST_PATTERN_GRID:     test_pattern_draw_grid();     break;
        case TEST_PATTERN_GRADIENT: test_pattern_draw_gradient(); break;
        case TEST_PATTERN_RED:      renderer_clear(COLOR_RED);    break;
        case TEST_PATTERN_GREEN:    renderer_clear(COLOR_GREEN);  break;
        case TEST_PATTERN_BLUE:     renderer_clear(COLOR_BLUE);   break;
    }

    char footer[48];
    snprintf(footer, sizeof(footer), "%d/%d %s -- ARRIBA/ABAJO CAMBIA, SELECCIONA SALE",
            (unsigned int) idx + 1, TEST_PATTERN_COUNT, test_pattern_names[idx]);

    int fy = TFT_HEIGHT - 12;
    renderer_fill_rect(0, fy - 3, TFT_WIDTH, 13, COLOR_BLACK);
    renderer_draw_text(4, fy, footer, COLOR_WHITE, COLOR_BLACK, 1);

    renderer_flush();
}

static void run_test_screen(void)
{
    int pattern = TEST_PATTERN_BARS;
    draw_test_pattern(pattern);

    while (true) {
        controls_update();
        sound_update();

        if (controls_menu_down()) {
            pattern = (pattern + 1) % TEST_PATTERN_COUNT;
            sound_effect_move();
            draw_test_pattern(pattern);
        }
        if (controls_menu_up()) {
            pattern = (pattern - 1 + TEST_PATTERN_COUNT) % TEST_PATTERN_COUNT;
            sound_effect_move();
            draw_test_pattern(pattern);
        }
        if (controls_menu_select()) {
            sound_effect_select();
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

/*
 * -----------------------------------------------------------
 * PRUEBA SONIDO
 * -----------------------------------------------------------
 */

typedef enum {
    SND_MUSIC_TOGGLE = 0, SND_SIREN_TOGGLE,
    SND_FX_SHOOT, SND_FX_EXPLOSION, SND_FX_SELECT, SND_FX_MOVE,
    SND_FX_GAME_OVER, SND_FX_SUCCESS, SND_FX_LOSE_POINT, SND_FX_VICTORY,
    SND_BACK, SND_ITEM_COUNT
} sound_test_item_t;

static const char *sound_test_item_names[SND_ITEM_COUNT] = {
    "CANAL 1+2: MUSICA MENU", "CANAL 2: SIRENA",
    "CANAL 3: EFECTO DISPARO", "CANAL 3: EFECTO EXPLOSION",
    "CANAL 3: EFECTO SELECCION", "CANAL 3: EFECTO MOVIMIENTO",
    "CANAL 3: EFECTO GAME OVER", "CANAL 3: EFECTO EXITO",
    "CANAL 3: EFECTO PIERDE PUNTO", "CANAL 3: EFECTO VICTORIA",
    "VOLVER"
};

static bool sound_test_siren_on = false;

static void draw_sound_test_menu(int selected)
{
    renderer_clear(COLOR_BLACK);
    renderer_draw_text(centered_x("PRUEBA SONIDO", 2), TITLE_Y, "PRUEBA SONIDO",
                        COLOR_CYAN, COLOR_BLACK, 2);
    renderer_fill_rect(10, DIVIDER_Y, TFT_WIDTH - 20, 2, COLOR_CYAN);

    int y = 42;
    int line_h = 14;
    for (int i = 0; i < SND_ITEM_COUNT; i++) {
        uint16_t color = (i == selected) ? COLOR_YELLOW : COLOR_WHITE;
        renderer_draw_text(16, y + i * line_h, sound_test_item_names[i], color, COLOR_BLACK, 1);
    }

    char buf[40];
    int state_y = y + SND_ITEM_COUNT * line_h + 8;
    snprintf(buf, sizeof(buf), "MUSICA: %s", sound_menu_music_is_playing() ? "REPRODUCIENDO" : "PARADA");
    renderer_draw_text(16, state_y, buf, COLOR_CYAN, COLOR_BLACK, 1);
    snprintf(buf, sizeof(buf), "SIRENA: %s", sound_test_siren_on ? "ACTIVA" : "PARADA");
    renderer_draw_text(16, state_y + 14, buf, COLOR_CYAN, COLOR_BLACK, 1);

    renderer_flush();
}

static void run_test_sound(void)
{
    int selected = 0;

    sound_stop_menu_music();
    sound_siren_stop();
    sound_test_siren_on = false;

    draw_sound_test_menu(selected);

    while (true) {
        controls_update();
        sound_update();

        bool redraw = false;

        if (controls_menu_down()) { selected = (selected + 1) % SND_ITEM_COUNT; redraw = true; }
        if (controls_menu_up())   { selected = (selected - 1 + SND_ITEM_COUNT) % SND_ITEM_COUNT; redraw = true; }

        if (controls_menu_select()) {
            switch (selected) {
                case SND_MUSIC_TOGGLE:
                    if (sound_menu_music_is_playing()) sound_stop_menu_music();
                    else sound_start_menu_music();
                    break;
                case SND_SIREN_TOGGLE:
                    if (sound_test_siren_on) { sound_siren_stop(); sound_test_siren_on = false; }
                    else { sound_siren_start(); sound_test_siren_on = true; }
                    break;
                case SND_FX_SHOOT:      sound_effect_shoot();      break;
                case SND_FX_EXPLOSION:  sound_effect_explosion();  break;
                case SND_FX_SELECT:     sound_effect_select();     break;
                case SND_FX_MOVE:       sound_effect_move();       break;
                case SND_FX_GAME_OVER:  sound_effect_game_over();  break;
                case SND_FX_SUCCESS:    sound_effect_success();    break;
                case SND_FX_LOSE_POINT: sound_effect_lose_point(); break;
                case SND_FX_VICTORY:    sound_effect_victory();    break;
                case SND_BACK:
                    sound_stop_menu_music();
                    sound_siren_stop();
                    return;
            }
            redraw = true;
        }

        if (redraw) draw_sound_test_menu(selected);

        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

/*
 * -----------------------------------------------------------
 * CONFIRMACIÓN BORRAR RECORDS
 *
 * PORT A ESP32: el original usaba el signo del delta de encoder
 * para alternar SI/NO. Sustituido por controls_menu_up()/down(),
 * el mismo mecanismo (con su zona muerta/histeresis ya resuelta)
 * que usa el resto del menu.
 * -----------------------------------------------------------
 */
static void run_clear_scores_confirm(void)
{
    bool select_yes = false; // Por defecto empezamos en NO por seguridad

    while (true) {
        controls_update();
        sound_update();

        if (controls_menu_up() || controls_menu_down()) {
            select_yes = !select_yes;
            sound_effect_move();
        }

        renderer_clear(COLOR_BLACK);
        renderer_draw_text(centered_x("BORRAR RECORDS", 2), TITLE_Y, "BORRAR RECORDS",
                            COLOR_RED, COLOR_BLACK, 2);
        renderer_fill_rect(10, DIVIDER_Y, TFT_WIDTH - 20, 2, COLOR_RED);
        renderer_draw_text(centered_x("¿SEGURO?", 2), 80, "¿SEGURO?", COLOR_WHITE, COLOR_BLACK, 2);

        uint16_t color_si = select_yes ? COLOR_YELLOW : COLOR_WHITE;
        uint16_t color_no = !select_yes ? COLOR_YELLOW : COLOR_WHITE;
        renderer_draw_text(90, 140, "SI", color_si, COLOR_BLACK, 3);
        renderer_draw_text(190, 140, "NO", color_no, COLOR_BLACK, 3);

        static const char *hint = "MUEVE PARA ELEGIR, PULSA PARA CONFIRMAR";
        renderer_draw_text(centered_x(hint, 1), 205, hint, COLOR_CYAN, COLOR_BLACK, 1);

        renderer_flush();

        if (controls_menu_select()) {
            sound_effect_select();
            if (select_yes) {
                highscores_reset(); // Borra RAM y actualiza NVS

                renderer_clear(COLOR_BLACK);
                renderer_draw_text(centered_x("¡RECORDS BORRADOS!", 2), 110, "¡RECORDS BORRADOS!",
                                    COLOR_GREEN, COLOR_BLACK, 2);
                renderer_flush();
                vTaskDelay(pdMS_TO_TICKS(1500));
            }
            return;
        }

        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

/*
 * -----------------------------------------------------------
 * ACERCA DE
 * -----------------------------------------------------------
 */
static void run_about_screen(void)
{
    renderer_clear(COLOR_BLACK);
    renderer_draw_text(centered_x(PROJECT_TITLE, TITLE_SCALE), 40, PROJECT_TITLE,
                        COLOR_CYAN, COLOR_BLACK, TITLE_SCALE);

    char version_line[32];
    snprintf(version_line, sizeof(version_line), "VERSION %s", FIRMWARE_VERSION);
    renderer_draw_text(centered_x(version_line, 2), 95, version_line, COLOR_WHITE, COLOR_BLACK, 2);

    char author_line[32];
    snprintf(author_line, sizeof(author_line), "CREADO POR %s", FIRMWARE_AUTHOR);
    renderer_draw_text(centered_x(author_line, 2), 125, author_line, COLOR_YELLOW, COLOR_BLACK, 2);

    static const char *hint = "PULSA CUALQUIER BOTON PARA VOLVER";
    renderer_draw_text(centered_x(hint, 1), 180, hint, COLOR_WHITE, COLOR_BLACK, 1);

    renderer_flush();

    while (true) {
        controls_update();
        if (controls_menu_select()) break;
        sound_update();
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

static void show_options_screen(void)
{
    int selected = 0;
    draw_options_menu(selected);

    while (true) {
        controls_update();
        sound_update();

        bool redraw = false;

        if (controls_menu_down()) { selected = (selected + 1) % OPT_COUNT; redraw = true; }
        if (controls_menu_up())   { selected = (selected - 1 + OPT_COUNT) % OPT_COUNT; redraw = true; }

        if (redraw) {
            sound_effect_move();
            draw_options_menu(selected);
        }

        if (controls_menu_select()) {
            sound_effect_select();

            switch (selected) {
                case OPT_TEST_CONTROLS: run_test_controls(); break;
                case OPT_TEST_SCREEN:   run_test_screen(); break;
                case OPT_TEST_SOUND:    run_test_sound(); break;
                case OPT_CLEAR_SCORES:  run_clear_scores_confirm(); break;
                case OPT_ABOUT:         run_about_screen(); break;
                case OPT_BACK:          return;
            }

            draw_options_menu(selected);
        }

        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

static bool show_scores_screen(int game_index)
{
    renderer_clear(COLOR_BLACK);
    highscores_draw(game_index, game_name_at(game_index), 30);
    renderer_flush();

    uint32_t start = now_ms();

    while (now_ms() - start < SCORES_DISPLAY_MS) {
        controls_update();

        if (controls_menu_up() || controls_menu_down() || controls_menu_select()) {
            return false;
        }

        // La musica no se reproduce durante la pantalla de records.
        vTaskDelay(pdMS_TO_TICKS(15));
    }

    return true;
}

static void run_attract_cycle(int game_index)
{
    if (game_index == OPTIONS_INDEX) return;

    sound_stop_menu_music(); // el attract mode se ejecuta sin musica de menu

    if (show_scores_screen(game_index)) {
        games_list[game_index].run(ATTRACT_DEMO_MODE);
        highscores_flush();
    }
}

void menu_run(void)
{
    int selected = 0;
    int idle_ms = 0;

    sound_init();
    sound_start_menu_music();

    renderer_clear(COLOR_BLACK);
    draw_title();
    draw_settled(selected);
    renderer_flush();

    while (true) {
        sound_update();
        controls_update();

        int direction = 0;
        int new_selected = selected;
        bool had_input = false;

        if (controls_menu_down()) {
            new_selected = wrap_index(selected + 1);
            direction = 1;
            had_input = true;
        }
        if (controls_menu_up()) {
            new_selected = wrap_index(selected - 1);
            direction = -1;
            had_input = true;
        }

        if (direction != 0) {
            animate_transition(selected, new_selected, direction);
            selected = new_selected;
            draw_settled(selected);
            renderer_flush();
        }

        if (controls_menu_select()) {
            had_input = true;

            sound_stop_menu_music(); // debe parar antes de entrar en cualquier pantalla/juego

            if (selected == OPTIONS_INDEX) {
                show_options_screen();
            } else {
                games_list[selected].run(GAME_MODE_1P);
                highscores_flush();
            }

            sound_start_menu_music();

            renderer_clear(COLOR_BLACK);
            draw_title();
            draw_settled(selected);
            renderer_flush();
        }

        if (had_input) {
            idle_ms = 0;
        } else {
            idle_ms += 15;

            if (idle_ms >= IDLE_TIMEOUT_MS) {
                idle_ms = 0;

                run_attract_cycle(selected);
                selected = wrap_index(selected + 1);

                sound_start_menu_music(); // el attract mode puede haber dejado el sonido apagado

                renderer_clear(COLOR_BLACK);
                draw_title();
                draw_settled(selected);
                renderer_flush();
            }
        }

        update_title_animation();

        vTaskDelay(pdMS_TO_TICKS(15));
    }
}
