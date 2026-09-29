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
#include <stdlib.h>

/*
 * PORT A ESP32 de la versión ampliada del menu.c de la Pico.
 *
 * Cambios respecto a la versión de la Pico (solo hardware/SO):
 *
 *  - sleep_ms() -> menu_sleep_ms() (vTaskDelay con mínimo de 1 tick).
 *  - absolute_time_t / get_absolute_time() / time_us_32() ->
 *    esp_timer_get_time().
 *  - PRUEBA CONTROLES: en vez de dos encoders (controls_debug_raw_count/
 *    pending, que no existen con joystick) muestra dos cajas
 *    "JOYSTICK 1/2" con el valor crudo y normalizado de cada eje.
 *  - BORRAR RECORDS: SI/NO se cambia con controls_menu_up()/down()
 *    en vez de girar el encoder.
 *  - ACERCA DE: la imagen de fondo se vuelca con
 *    renderer_blit_to_buffer() (a través de about_draw_image()).
 *
 * TODO LO DEMÁS (carrete, animaciones, submenú de opciones, prueba de
 * sonido con todas las pistas y efectos, lista de reproducción de la
 * música de menú, attract mode) es la MISMA lógica que el menu.c de
 * la Pico.
 */

/*
 * controls_reset_menu_nav() descarta la navegación de menú pendiente
 * al volver de un juego/prueba (en la Pico evita que un giro de
 * encoder sin consumir se cuele como un cambio de selección). Ponlo a
 * 1 si el controls.c de la ESP32 tiene esa función; a 0 se compila
 * sin ella.
 */
#define MENU_USE_NAV_RESET 0

#if MENU_USE_NAV_RESET
#define menu_nav_reset() controls_reset_menu_nav()
#else
#define menu_nav_reset() ((void)0)
#endif

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* menu_sleep_ms(ms) vale 0 si ms es menor que un tick
 * (10 ms con el tick por defecto de 100 Hz) y entonces no cede la CPU;
 * aquí se garantiza al menos 1 tick. */
static inline void menu_sleep_ms(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks ? ticks : 1);
}

/*
 * Selector tipo "carrete": el juego seleccionado aparece grande y
 * centrado, con el anterior y el siguiente arriba/abajo en letra
 * pequeña. La lista es circular: tras el último elemento vuelve al
 * primero, y viceversa.
 *
 * Pantalla real: 320 (ancho) x 240 (alto), apaisada.
 *
 * Última entrada del carrete: "OPCIONES" (TOTAL_ITEMS = NUM_GAMES + 1),
 * de momento un marcador de posición para futuras funciones de test.
 *
 * DISEÑO SIMPLIFICADO (antes había un bug de borrado con 4 filas
 * animándose a la vez -- dos visibles y dos entrando/saliendo fuera
 * de pantalla): ahora solo existen 3 posiciones fijas en pantalla
 * (prev, centro, next) y, como mucho, 2 filas de texto se mueven a
 * la vez entre esas 3 posiciones. Nunca hay una 4ª fila fantasma.
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

// Lista de reproducción de la música de menú: las pistas de
// sound.c (0=Greensleeves, 1=Neon Circuit, 2=Star Patrol) suenan
// una detrás de otra en bucle, cada una una vuelta completa, con
// este hueco de silencio entre medias -- ver menu_music_playlist_*
// más abajo.
#define MENU_MUSIC_SILENCE_MS (2 * 1000)

static const char *PROJECT_TITLE = "ARCADE COLOR";


static int wrap_index(int idx)
{
    idx %= TOTAL_ITEMS;

    if (idx < 0) {
        idx += TOTAL_ITEMS;
    }

    return idx;
}


static const char *game_name_at(int idx)
{
    idx = wrap_index(idx);

    if (idx == OPTIONS_INDEX) {
        return "OPCIONES";
    }

    return games_list[idx].name;
}


static int best_fit_scale(
    const char *text,
    int max_scale
)
{
    for (
        int scale = max_scale;
        scale > 1;
        scale--
    ) {
        if (
            st7789_text_width(
                text,
                (uint8_t)scale
            ) <= TFT_WIDTH - 10
        ) {
            return scale;
        }
    }

    return 1;
}


static int centered_x(
    const char *text,
    int scale
)
{
    int text_width =
        (int)st7789_text_width(
            text,
            (uint8_t)scale
        );

    int x =
        (TFT_WIDTH - text_width) / 2;

    return (x < 0) ? 0 : x;
}


static void clear_row(int y)
{
    int top_margin = 4;
    int bottom_margin = 6;

    int height =
        top_margin +
        FONT_HEIGHT * SELECTED_SCALE_MAX +
        bottom_margin;

    renderer_fill_rect(
        0,
        y - top_margin,
        TFT_WIDTH,
        height,
        COLOR_BLACK
    );
}


static void draw_row(
    int y,
    const char *text,
    int scale,
    uint16_t color
)
{
    renderer_draw_text(
        centered_x(text, scale),
        y,
        text,
        color,
        COLOR_BLACK,
        scale
    );
}


static void draw_title(void)
{
    renderer_fill_rect(
        0,
        0,
        TFT_WIDTH,
        DIVIDER_Y + 2,
        COLOR_BLACK
    );

    renderer_draw_text(
        centered_x(
            PROJECT_TITLE,
            TITLE_SCALE
        ),
        TITLE_Y,
        PROJECT_TITLE,
        COLOR_CYAN,
        COLOR_BLACK,
        TITLE_SCALE
    );

    renderer_fill_rect(
        10,
        DIVIDER_Y,
        TFT_WIDTH - 20,
        2,
        COLOR_CYAN
    );
}


#define TITLE_WAVE_AMPLITUDE 4
#define TITLE_WAVE_SPEED 0.28f
#define TITLE_WAVE_CHAR_PHASE 0.6f
#define TITLE_COLOR_CYCLE_EVERY 6
#define TITLE_ANIM_THROTTLE 3

static const uint16_t title_palette[] = {
    COLOR_CYAN,
    COLOR_YELLOW,
    COLOR_MAGENTA,
    COLOR_GREEN,
    COLOR_RED,
    COLOR_WHITE
};

#define TITLE_PALETTE_LEN \
    (int)(sizeof(title_palette) / sizeof(title_palette[0]))

#define TITLE_BAND_HEIGHT \
    (FONT_HEIGHT * TITLE_SCALE + \
     2 * TITLE_WAVE_AMPLITUDE + 4)

static float title_wave_phase = 0.0f;
static int title_color_phase = 0;


static void draw_title_wave(void)
{
    int text_w =
        (int)st7789_text_width(
            PROJECT_TITLE,
            TITLE_SCALE
        );

    int x =
        (TFT_WIDTH - text_w) / 2;

    if (x < 0) {
        x = 0;
    }

    int band_y =
        TITLE_Y -
        TITLE_WAVE_AMPLITUDE -
        2;

    renderer_fill_rect(
        0,
        band_y,
        TFT_WIDTH,
        TITLE_BAND_HEIGHT,
        COLOR_BLACK
    );

    for (
        int i = 0;
        PROJECT_TITLE[i] != '\0';
        i++
    ) {
        float angle =
            title_wave_phase +
            i * TITLE_WAVE_CHAR_PHASE;

        int y_offset =
            (int)(
                sinf(angle) *
                TITLE_WAVE_AMPLITUDE
            );

        uint16_t color =
            title_palette[
                (i + title_color_phase) %
                TITLE_PALETTE_LEN
            ];

        renderer_draw_char(
            x,
            TITLE_Y + y_offset,
            PROJECT_TITLE[i],
            color,
            COLOR_BLACK,
            TITLE_SCALE
        );

        x +=
            (FONT_WIDTH + 1) *
            TITLE_SCALE;
    }
}


static void update_title_animation(void)
{
    static int throttle_counter = 0;
    static int color_tick = 0;

    throttle_counter++;

    if (
        throttle_counter <
        TITLE_ANIM_THROTTLE
    ) {
        return;
    }

    throttle_counter = 0;

    title_wave_phase += TITLE_WAVE_SPEED;

    if (
        title_wave_phase >
        6.2831853f
    ) {
        title_wave_phase -= 6.2831853f;
    }

    color_tick++;

    if (
        color_tick >=
        TITLE_COLOR_CYCLE_EVERY
    ) {
        color_tick = 0;

        title_color_phase =
            (title_color_phase + 1) %
            TITLE_PALETTE_LEN;
    }

    draw_title_wave();
    renderer_flush();
}


static void draw_settled(int selected)
{
    clear_row(
        CENTER_Y - ITEM_SPACING
    );

    clear_row(CENTER_Y);

    clear_row(
        CENTER_Y + ITEM_SPACING
    );

    const char *name =
        game_name_at(selected);

    draw_row(
        CENTER_Y - ITEM_SPACING,
        game_name_at(selected - 1),
        PREV_NEXT_SCALE,
        COLOR_WHITE
    );

    draw_row(
        CENTER_Y,
        name,
        best_fit_scale(
            name,
            SELECTED_SCALE_MAX
        ),
        COLOR_YELLOW
    );

    draw_row(
        CENTER_Y + ITEM_SPACING,
        game_name_at(selected + 1),
        PREV_NEXT_SCALE,
        COLOR_WHITE
    );
}


static void animate_transition(
    int old_selected,
    int new_selected,
    int direction
)
{
    const char *slide_center =
        game_name_at(old_selected);

    const char *slide_edge =
        game_name_at(new_selected);

    int y_center_start = CENTER_Y;

    int y_center_end =
        CENTER_Y -
        direction * ITEM_SPACING;

    int y_edge_start =
        CENTER_Y +
        direction * ITEM_SPACING;

    int y_edge_end = CENTER_Y;

    int y_vacating =
        (direction > 0)
        ? (CENTER_Y - ITEM_SPACING)
        : (CENTER_Y + ITEM_SPACING);

    int y_revealing =
        (direction > 0)
        ? (CENTER_Y + ITEM_SPACING)
        : (CENTER_Y - ITEM_SPACING);

    clear_row(y_vacating);
    clear_row(y_revealing);

    int y_center_prev =
        y_center_start;

    int y_edge_prev =
        y_edge_start;

    for (
        int step = 1;
        step <= ANIM_STEPS;
        step++
    ) {
        int y_center =
            y_center_start +
            (y_center_end - y_center_start) *
            step /
            ANIM_STEPS;

        int y_edge =
            y_edge_start +
            (y_edge_end - y_edge_start) *
            step /
            ANIM_STEPS;

        clear_row(y_center_prev);
        clear_row(y_edge_prev);
        clear_row(y_center);
        clear_row(y_edge);

        draw_row(
            y_center,
            slide_center,
            PREV_NEXT_SCALE,
            COLOR_WHITE
        );

        draw_row(
            y_edge,
            slide_edge,
            PREV_NEXT_SCALE,
            COLOR_WHITE
        );

        renderer_flush();

        y_center_prev = y_center;
        y_edge_prev = y_edge;

        /*
         * La música sigue avanzando durante la animación.
         */
        sound_update();

        menu_sleep_ms(ANIM_STEP_DELAY_MS);
    }
}


/*
 * ===========================================================
 * SUBMENÚ DE OPCIONES
 * ===========================================================
 *
 * "OPCIONES" ya no es una pantalla de marcador de posición: es un
 * segundo carrete de menú (más simple, en lista vertical) con 4
 * herramientas de test para el hardware más "VOLVER" para regresar
 * al menú principal.
 * ===========================================================
 */

#define FIRMWARE_VERSION "v1.5 EMT SPECIAL"
#define FIRMWARE_AUTHOR  "JUANLU"

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

    renderer_draw_text(
        centered_x("OPCIONES", TITLE_SCALE),
        TITLE_Y,
        "OPCIONES",
        COLOR_CYAN,
        COLOR_BLACK,
        TITLE_SCALE
    );

    renderer_fill_rect(
        10,
        DIVIDER_Y,
        TFT_WIDTH - 20,
        2,
        COLOR_CYAN
    );

    for (int i = 0; i < OPT_COUNT; i++) {
        const char *text = options_item_names[i];
        uint16_t color = (i == selected) ? COLOR_YELLOW : COLOR_WHITE;

        renderer_draw_text(
            centered_x(text, OPTIONS_ITEM_SCALE),
            OPTIONS_ITEM_Y_START + i * OPTIONS_ITEM_SPACING,
            text,
            color,
            COLOR_BLACK,
            OPTIONS_ITEM_SCALE
        );
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

        menu_sleep_ms(15);
    }
}

/*
 * -----------------------------------------------------------
 * PRUEBA PANTALLA
 *
 * "Carta de ajuste": varios patrones de test para comprobar
 * geometría, colores y píxeles muertos. Arriba/abajo cambia de
 * patrón, seleccionar sale.
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
    "BARRAS DE COLOR",
    "REJILLA + CRUCETA",
    "GRADIENTE DE GRISES",
    "ROJO SOLIDO",
    "VERDE SOLIDO",
    "AZUL SOLIDO"
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

    /* Franja inferior con escalera de grises */
    int steps = 8;
    int step_w = TFT_WIDTH / steps;

    for (int i = 0; i < steps; i++) {
        int level = (i * 31) / (steps - 1);
        int w = (i == steps - 1) ? (TFT_WIDTH - i * step_w) : step_w;

        renderer_fill_rect(
            i * step_w,
            bar_h,
            w,
            TFT_HEIGHT - bar_h,
            test_gray565(level)
        );
    }
}

static void test_pattern_draw_grid(void)
{
    /* Marco exterior */
    renderer_fill_rect(0, 0, TFT_WIDTH, 2, COLOR_WHITE);
    renderer_fill_rect(0, TFT_HEIGHT - 2, TFT_WIDTH, 2, COLOR_WHITE);
    renderer_fill_rect(0, 0, 2, TFT_HEIGHT, COLOR_WHITE);
    renderer_fill_rect(TFT_WIDTH - 2, 0, 2, TFT_HEIGHT, COLOR_WHITE);

    /* Rejilla */
    for (int x = 20; x < TFT_WIDTH; x += 20) {
        renderer_fill_rect(x, 0, 1, TFT_HEIGHT, COLOR_CYAN);
    }

    for (int y = 20; y < TFT_HEIGHT; y += 20) {
        renderer_fill_rect(0, y, TFT_WIDTH, 1, COLOR_CYAN);
    }

    /* Cruceta central */
    int cx = TFT_WIDTH / 2;
    int cy = TFT_HEIGHT / 2;

    renderer_fill_rect(cx - 20, cy - 1, 40, 2, COLOR_YELLOW);
    renderer_fill_rect(cx - 1, cy - 20, 2, 40, COLOR_YELLOW);

    /* Marcas en las 4 esquinas */
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
        case TEST_PATTERN_BARS:
            test_pattern_draw_bars();
            break;

        case TEST_PATTERN_GRID:
            test_pattern_draw_grid();
            break;

        case TEST_PATTERN_GRADIENT:
            test_pattern_draw_gradient();
            break;

        case TEST_PATTERN_RED:
            renderer_clear(COLOR_RED);
            break;

        case TEST_PATTERN_GREEN:
            renderer_clear(COLOR_GREEN);
            break;

        case TEST_PATTERN_BLUE:
            renderer_clear(COLOR_BLUE);
            break;
    }

    /* Dos líneas en vez de una: en una sola línea el texto medía
     * ~380 px con letra de 6 px/carácter y se salía de los 320 px
     * de pantalla; además el buffer de 48 bytes no bastaba y
     * ESP-IDF (-Werror=format-truncation) lo rechaza. */
    char footer[64];

    snprintf(
        footer,
        sizeof(footer),
        "%d/%d %s",
        idx + 1,
        TEST_PATTERN_COUNT,
        test_pattern_names[idx]
    );

    static const char *footer_hint = "ARRIBA/ABAJO CAMBIA, SELECCIONA SALE";

    int fy = TFT_HEIGHT - 22;

    renderer_fill_rect(0, fy - 3, TFT_WIDTH, 25, COLOR_BLACK);

    renderer_draw_text(
        4,
        fy,
        footer,
        COLOR_WHITE,
        COLOR_BLACK,
        1
    );

    renderer_draw_text(
        4,
        fy + 11,
        footer_hint,
        COLOR_YELLOW,
        COLOR_BLACK,
        1
    );

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

        menu_sleep_ms(15);
    }
}


/*
 * -----------------------------------------------------------
 * PRUEBA SONIDO
 *
 * Carrete navegable (mismo estilo que el menú principal: prev/
 * centro/next, con el central resaltado y en tamaño ajustado a lo
 * que quepa) con TODAS las músicas y efectos de sound.h -- música
 * de menú (+ cambio de pista), Tetris, victoria de Scramble,
 * Paratrooper, intro de Pac-Man, sirena, motor, derrape, los 8
 * efectos básicos, los 12 efectos añadidos y los 6 de Pac-Man.
 * Arriba/abajo mueve la selección, seleccionar dispara el efecto (o
 * hace toggle si tiene estado propio: música/sirena/motor/derrape).
 * "VOLVER" sale de vuelta al submenú de opciones.
 * -----------------------------------------------------------
 */

typedef enum {
    SND_MUSIC_MENU = 0,
    SND_MUSIC_MENU_NEXT_TRACK,
    SND_MUSIC_TETRIS,
    SND_MUSIC_SCRAMBLE_VICTORY,
    SND_MUSIC_PARATROOPER,
    SND_MUSIC_PACMAN_INTRO,

    SND_SIREN,
    SND_ENGINE,
    SND_SKID,

    SND_FX_SHOOT,
    SND_FX_EXPLOSION,
    SND_FX_SELECT,
    SND_FX_MOVE,
    SND_FX_GAME_OVER,
    SND_FX_SUCCESS,
    SND_FX_LOSE_POINT,
    SND_FX_VICTORY,

    SND_FX_LASER,
    SND_FX_LASER_BIG,
    SND_FX_POWERUP,
    SND_FX_POWERDOWN,
    SND_FX_COIN,
    SND_FX_JUMP,
    SND_FX_HIT,
    SND_FX_ALARM,
    SND_FX_TELEPORT,
    SND_FX_BOUNCE,
    SND_FX_THRUST,
    SND_FX_EXTRA_LIFE,

    SND_PACMAN_CHOMP,
    SND_PACMAN_POWER,
    SND_PACMAN_FRUIT,
    SND_PACMAN_EAT_GHOST,
    SND_PACMAN_DEATH,
    SND_PACMAN_LEVELUP,

    SND_BACK,
    SND_ITEM_COUNT
} sound_test_item_t;

/*
 * Nombres base (sin estado). Los elementos con estado propio
 * (música / sirena / motor / derrape) lo añaden dinámicamente en
 * sound_test_item_label() -- por eso aquí solo hace falta el nombre
 * "en reposo".
 */
static const char *sound_test_base_names[SND_ITEM_COUNT] = {
    [SND_MUSIC_MENU]             = "MUSICA MENU",
    [SND_MUSIC_MENU_NEXT_TRACK]  = "SIGUIENTE PISTA MENU",
    [SND_MUSIC_TETRIS]           = "MUSICA TETRIS",
    [SND_MUSIC_SCRAMBLE_VICTORY] = "VICTORIA SCRAMBLE",
    [SND_MUSIC_PARATROOPER]      = "MUSICA PARATROOPER",
    [SND_MUSIC_PACMAN_INTRO]     = "INTRO PACMAN",

    [SND_SIREN]  = "SIRENA",
    [SND_ENGINE] = "MOTOR (RAMPA)",
    [SND_SKID]   = "DERRAPE",

    [SND_FX_SHOOT]      = "EFECTO: DISPARO",
    [SND_FX_EXPLOSION]  = "EFECTO: EXPLOSION",
    [SND_FX_SELECT]     = "EFECTO: SELECCION",
    [SND_FX_MOVE]       = "EFECTO: MOVIMIENTO",
    [SND_FX_GAME_OVER]  = "EFECTO: GAME OVER",
    [SND_FX_SUCCESS]    = "EFECTO: EXITO",
    [SND_FX_LOSE_POINT] = "EFECTO: PIERDE PUNTO",
    [SND_FX_VICTORY]    = "EFECTO: VICTORIA",

    [SND_FX_LASER]      = "EFECTO: LASER",
    [SND_FX_LASER_BIG]  = "EFECTO: LASER GRANDE",
    [SND_FX_POWERUP]    = "EFECTO: MEJORA",
    [SND_FX_POWERDOWN]  = "EFECTO: PIERDE PODER",
    [SND_FX_COIN]       = "EFECTO: MONEDA",
    [SND_FX_JUMP]       = "EFECTO: SALTO",
    [SND_FX_HIT]        = "EFECTO: IMPACTO",
    [SND_FX_ALARM]      = "EFECTO: ALARMA",
    [SND_FX_TELEPORT]   = "EFECTO: TELETRANSPORTE",
    [SND_FX_BOUNCE]     = "EFECTO: REBOTE",
    [SND_FX_THRUST]     = "EFECTO: PROPULSOR",
    [SND_FX_EXTRA_LIFE] = "EFECTO: 1UP",

    [SND_PACMAN_CHOMP]     = "PACMAN: CHOMP",
    [SND_PACMAN_POWER]     = "PACMAN: POWER PELLET",
    [SND_PACMAN_FRUIT]     = "PACMAN: FRUTA",
    [SND_PACMAN_EAT_GHOST] = "PACMAN: FANTASMA COMIDO",
    [SND_PACMAN_DEATH]     = "PACMAN: MUERTE",
    [SND_PACMAN_LEVELUP]   = "PACMAN: NIVEL SUPERADO",

    [SND_BACK] = "VOLVER"
};

/*
 * Estado local de los elementos que no tienen su propio
 * "is_playing()" en sound.h (sirena, motor, derrape) y del
 * alternado tic/tac del chomp de Pac-Man.
 */
static bool  sound_test_siren_on        = false;
static bool  sound_test_engine_on       = false;
static bool  sound_test_skid_on         = false;
static bool  sound_test_pacman_chomp_alt = false;
static float sound_test_engine_phase    = 0.0f;

#define SOUND_TEST_ENGINE_RAMP_SPEED 0.05f


static int snd_wrap_index(int idx)
{
    idx %= SND_ITEM_COUNT;

    if (idx < 0) {
        idx += SND_ITEM_COUNT;
    }

    return idx;
}


/*
 * Nombre a mostrar para el elemento "idx", con su estado (SONANDO /
 * PARADA / ACTIVA...) añadido cuando aplica. Se llama tanto para el
 * elemento central como para los de arriba/abajo del carrete, así
 * que el estado se ve incluso antes de tenerlo seleccionado.
 */
static void sound_test_item_label(
    int idx,
    char *buf,
    size_t buf_size
)
{
    idx = snd_wrap_index(idx);

    switch (idx) {
        case SND_MUSIC_MENU:
            snprintf(
                buf, buf_size, "MUSICA MENU: %s",
                sound_menu_music_is_playing() ? "SONANDO" : "PARADA"
            );
            break;

        case SND_MUSIC_MENU_NEXT_TRACK: {
            static const char *track_names[SOUND_MENU_TRACK_COUNT] = {
                "GREENSLEEVES", "NEON CIRCUIT", "STAR PATROL"
            };

            uint8_t track = sound_get_menu_track();

            snprintf(
                buf, buf_size, "PISTA MENU: %s",
                (track < SOUND_MENU_TRACK_COUNT) ? track_names[track] : "?"
            );
            break;
        }

        case SND_MUSIC_TETRIS:
            snprintf(
                buf, buf_size, "MUSICA TETRIS: %s",
                sound_tetris_music_is_playing() ? "SONANDO" : "PARADA"
            );
            break;

        case SND_MUSIC_SCRAMBLE_VICTORY:
            snprintf(
                buf, buf_size, "VICTORIA SCRAMBLE: %s",
                sound_scramble_victory_is_playing() ? "SONANDO" : "PARADA"
            );
            break;

        case SND_MUSIC_PARATROOPER:
            snprintf(
                buf, buf_size, "PARATROOPER: %s",
                sound_paratrooper_music_is_playing() ? "SONANDO" : "PARADA"
            );
            break;

        case SND_SIREN:
            snprintf(
                buf, buf_size, "SIRENA: %s",
                sound_test_siren_on ? "ACTIVA" : "PARADA"
            );
            break;

        case SND_ENGINE:
            snprintf(
                buf, buf_size, "MOTOR: %s",
                sound_test_engine_on ? "SONANDO" : "PARADO"
            );
            break;

        case SND_SKID:
            snprintf(
                buf, buf_size, "DERRAPE: %s",
                sound_test_skid_on ? "SONANDO" : "PARADO"
            );
            break;

        default:
            snprintf(buf, buf_size, "%s", sound_test_base_names[idx]);
            break;
    }
}


/*
 * Dispara/activa el elemento "idx". Los elementos con estado propio
 * (música, sirena, motor, derrape) hacen toggle; el resto suena una
 * vez. SND_BACK se gestiona en run_test_sound(), no aquí.
 */
static void sound_test_activate(int idx)
{
    idx = snd_wrap_index(idx);

    switch (idx) {
        case SND_MUSIC_MENU:
            if (sound_menu_music_is_playing()) {
                sound_stop_menu_music();
            } else {
                sound_start_menu_music();
            }
            break;

        case SND_MUSIC_MENU_NEXT_TRACK:
            sound_next_menu_track();
            break;

        case SND_MUSIC_TETRIS:
            if (sound_tetris_music_is_playing()) {
                sound_stop_tetris_music();
            } else {
                sound_start_tetris_music();
            }
            break;

        case SND_MUSIC_SCRAMBLE_VICTORY:
            if (sound_scramble_victory_is_playing()) {
                sound_stop_scramble_victory();
            } else {
                sound_start_scramble_victory();
            }
            break;

        case SND_MUSIC_PARATROOPER:
            if (sound_paratrooper_music_is_playing()) {
                sound_stop_paratrooper_music();
            } else {
                sound_start_paratrooper_music();
            }
            break;

        case SND_MUSIC_PACMAN_INTRO:
            /* No tiene is_playing(): cada pulsación la relanza. */
            sound_start_pacman_intro();
            break;

        case SND_SIREN:
            if (sound_test_siren_on) {
                sound_siren_stop();
                sound_test_siren_on = false;
            } else {
                sound_siren_start();
                sound_test_siren_on = true;
            }
            break;

        case SND_ENGINE:
            sound_test_engine_on = !sound_test_engine_on;

            if (sound_test_engine_on) {
                sound_test_engine_phase = 0.0f;
            } else {
                sound_engine_stop();
            }
            break;

        case SND_SKID:
            sound_test_skid_on = !sound_test_skid_on;

            if (!sound_test_skid_on) {
                sound_skid_stop();
            }
            break;

        case SND_FX_SHOOT:      sound_effect_shoot();      break;
        case SND_FX_EXPLOSION:  sound_effect_explosion();  break;
        case SND_FX_SELECT:     sound_effect_select();     break;
        case SND_FX_MOVE:       sound_effect_move();       break;
        case SND_FX_GAME_OVER:  sound_effect_game_over();  break;
        case SND_FX_SUCCESS:    sound_effect_success();    break;
        case SND_FX_LOSE_POINT: sound_effect_lose_point(); break;
        case SND_FX_VICTORY:    sound_effect_victory();    break;

        case SND_FX_LASER:      sound_effect_laser();      break;
        case SND_FX_LASER_BIG:  sound_effect_laser_big();  break;
        case SND_FX_POWERUP:    sound_effect_powerup();    break;
        case SND_FX_POWERDOWN:  sound_effect_powerdown();  break;
        case SND_FX_COIN:       sound_effect_coin();       break;
        case SND_FX_JUMP:       sound_effect_jump();       break;
        case SND_FX_HIT:        sound_effect_hit();        break;
        case SND_FX_ALARM:      sound_effect_alarm();      break;
        case SND_FX_TELEPORT:   sound_effect_teleport();   break;
        case SND_FX_BOUNCE:     sound_effect_bounce();     break;
        case SND_FX_THRUST:     sound_effect_thrust();     break;
        case SND_FX_EXTRA_LIFE: sound_effect_extra_life(); break;

        case SND_PACMAN_CHOMP:
            sound_effect_pacman_chomp(sound_test_pacman_chomp_alt);
            sound_test_pacman_chomp_alt = !sound_test_pacman_chomp_alt;
            break;

        case SND_PACMAN_POWER:     sound_effect_pacman_power();     break;
        case SND_PACMAN_FRUIT:     sound_effect_pacman_fruit();     break;
        case SND_PACMAN_EAT_GHOST: sound_effect_pacman_eat_ghost(); break;
        case SND_PACMAN_DEATH:     sound_effect_pacman_death();     break;
        case SND_PACMAN_LEVELUP:   sound_effect_pacman_levelup();   break;
    }
}


// Para todo lo que pueda estar sonando -- al entrar y al salir de
// la pantalla de prueba de sonido, para dejar un estado conocido.
static void sound_test_stop_everything(void)
{
    sound_stop_menu_music();
    sound_stop_tetris_music();
    sound_stop_scramble_victory();
    sound_stop_paratrooper_music();
    sound_stop_pacman_intro();
    sound_siren_stop();
    sound_engine_stop();
    sound_skid_stop();
    sound_effect_stop();

    sound_test_siren_on = false;
    sound_test_engine_on = false;
    sound_test_skid_on = false;
}


static void draw_sound_test_header(void)
{
    renderer_clear(COLOR_BLACK);

    renderer_draw_text(
        centered_x("PRUEBA SONIDO", 2),
        TITLE_Y,
        "PRUEBA SONIDO",
        COLOR_CYAN,
        COLOR_BLACK,
        2
    );

    renderer_fill_rect(10, DIVIDER_Y, TFT_WIDTH - 20, 2, COLOR_CYAN);

    static const char *hint =
        "ARRIBA/ABAJO MUEVE, SELECCIONA ACTIVA/PARA";

    renderer_draw_text(
        centered_x(hint, 1),
        215,
        hint,
        COLOR_CYAN,
        COLOR_BLACK,
        1
    );
}


/*
 * Carrete de 3 posiciones (prev/centro/next), igual que el menú
 * principal (ver draw_settled() más arriba) -- así caben las
 * músicas y todos los efectos de sound.h sin amontonarse: solo se
 * dibujan 3 a la vez y el nombre central se reduce de escala si no
 * cabe (best_fit_scale()), igual que con los nombres largos de
 * juegos.
 */
static void draw_sound_settled(int selected)
{
    clear_row(CENTER_Y - ITEM_SPACING);
    clear_row(CENTER_Y);
    clear_row(CENTER_Y + ITEM_SPACING);

    char prev_buf[40];
    char center_buf[40];
    char next_buf[40];

    sound_test_item_label(selected - 1, prev_buf, sizeof(prev_buf));
    sound_test_item_label(selected, center_buf, sizeof(center_buf));
    sound_test_item_label(selected + 1, next_buf, sizeof(next_buf));

    draw_row(
        CENTER_Y - ITEM_SPACING,
        prev_buf,
        PREV_NEXT_SCALE,
        COLOR_WHITE
    );

    draw_row(
        CENTER_Y,
        center_buf,
        best_fit_scale(center_buf, SELECTED_SCALE_MAX),
        COLOR_YELLOW
    );

    draw_row(
        CENTER_Y + ITEM_SPACING,
        next_buf,
        PREV_NEXT_SCALE,
        COLOR_WHITE
    );
}


/*
 * Misma animación de deslizamiento que animate_transition() del
 * menú principal, adaptada a sound_test_item_label() (que escribe
 * en un buffer en vez de devolver un puntero fijo).
 */
static void snd_animate_transition(
    int old_selected,
    int new_selected,
    int direction
)
{
    char slide_center[40];
    char slide_edge[40];

    sound_test_item_label(old_selected, slide_center, sizeof(slide_center));
    sound_test_item_label(new_selected, slide_edge, sizeof(slide_edge));

    int y_center_start = CENTER_Y;

    int y_center_end =
        CENTER_Y -
        direction * ITEM_SPACING;

    int y_edge_start =
        CENTER_Y +
        direction * ITEM_SPACING;

    int y_edge_end = CENTER_Y;

    int y_vacating =
        (direction > 0)
        ? (CENTER_Y - ITEM_SPACING)
        : (CENTER_Y + ITEM_SPACING);

    int y_revealing =
        (direction > 0)
        ? (CENTER_Y + ITEM_SPACING)
        : (CENTER_Y - ITEM_SPACING);

    clear_row(y_vacating);
    clear_row(y_revealing);

    int y_center_prev = y_center_start;
    int y_edge_prev = y_edge_start;

    for (
        int step = 1;
        step <= ANIM_STEPS;
        step++
    ) {
        int y_center =
            y_center_start +
            (y_center_end - y_center_start) *
            step /
            ANIM_STEPS;

        int y_edge =
            y_edge_start +
            (y_edge_end - y_edge_start) *
            step /
            ANIM_STEPS;

        clear_row(y_center_prev);
        clear_row(y_edge_prev);
        clear_row(y_center);
        clear_row(y_edge);

        draw_row(
            y_center,
            slide_center,
            PREV_NEXT_SCALE,
            COLOR_WHITE
        );

        draw_row(
            y_edge,
            slide_edge,
            PREV_NEXT_SCALE,
            COLOR_WHITE
        );

        renderer_flush();

        y_center_prev = y_center;
        y_edge_prev = y_edge;

        sound_update();

        menu_sleep_ms(ANIM_STEP_DELAY_MS);
    }
}


static void run_test_sound(void)
{
    int selected = 0;

    /*
     * Al entrar, garantizamos un estado conocido: nada sonando
     * hasta que el usuario elija qué probar.
     */
    sound_test_stop_everything();

    draw_sound_test_header();
    draw_sound_settled(selected);
    renderer_flush();

    while (true) {
        controls_update();
        sound_update();

        /*
         * Rampa continua del test de motor -- se llama una vez por
         * vuelta de bucle mientras esté activo, igual que exige
         * sound_engine_set_speed() en sound.h.
         */
        if (sound_test_engine_on) {
            sound_test_engine_phase += SOUND_TEST_ENGINE_RAMP_SPEED;

            if (sound_test_engine_phase > 6.2831853f) {
                sound_test_engine_phase -= 6.2831853f;
            }

            uint8_t speed =
                (uint8_t)(
                    (sinf(sound_test_engine_phase) * 0.5f + 0.5f) *
                    255.0f
                );

            sound_engine_set_speed(speed);
        }

        /*
         * sound_skid_start() no hace nada si ya está sonando, así
         * que llamarlo cada vuelta mientras esté activo es seguro.
         */
        if (sound_test_skid_on) {
            sound_skid_start();
        }

        int direction = 0;
        int new_selected = selected;

        if (controls_menu_down()) {
            new_selected = snd_wrap_index(selected + 1);
            direction = 1;
        }

        if (controls_menu_up()) {
            new_selected = snd_wrap_index(selected - 1);
            direction = -1;
        }

        if (direction != 0) {
            sound_effect_move();

            snd_animate_transition(
                selected,
                new_selected,
                direction
            );

            selected = new_selected;

            draw_sound_settled(selected);
            renderer_flush();
        }

        if (controls_menu_select()) {
            int idx = snd_wrap_index(selected);

            if (idx == SND_BACK) {
                sound_effect_select();
                sound_test_stop_everything();
                return;
            }

            sound_test_activate(idx);

            draw_sound_settled(selected);
            renderer_flush();
        }

        menu_sleep_ms(15);
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
                menu_sleep_ms(1500);
            }
            return;
        }

        menu_sleep_ms(15);
    }
}

/*
 * Vuelca ABOUT_IMAGE_DATA (RGB565 nativo, 320x240, mismo formato que
 * el framebuffer del driver) de una sola vez al framebuffer con
 * renderer_blit_to_buffer(): una copia por fila y UN único
 * rectángulo sucio, que el renderer_flush() posterior transmite.
 * La pantalla debe estar en apaisado (320x240), que es como se
 * queda el menú.
 */
/* Interruptor de diagnóstico: 1 = con imagen, 0 = solo el texto sobre
 * fondo negro (sin tocar about_image.h). */
#define ABOUT_SHOW_IMAGE 1

#if ABOUT_SHOW_IMAGE
#include "about_image.h"

static void about_draw_image(void)
{
    renderer_blit_to_buffer(0, 0, ABOUT_IMG_WIDTH, ABOUT_IMG_HEIGHT, ABOUT_IMAGE_DATA);
}
#else
static void about_draw_image(void)
{
}
#endif

/*
 * -----------------------------------------------------------
 * ACERCA DE
 * -----------------------------------------------------------
 */

static void run_about_screen(void)
{
    renderer_clear(COLOR_BLACK);

    // Volcar directamente la imagen a la pantalla usando el renderer
    about_draw_image();

    renderer_draw_text(
        centered_x(PROJECT_TITLE, TITLE_SCALE),
        5,
        PROJECT_TITLE,
        COLOR_RED,
        COLOR_BLACK,
        TITLE_SCALE
    );

    char version_line[32];
    snprintf(version_line, sizeof(version_line), "VERSION %s", FIRMWARE_VERSION);

    renderer_draw_text(
        centered_x(version_line, 2),
        200,
        version_line,
        COLOR_WHITE,
        COLOR_BLACK,
        2
    );

    char author_line[32];
    snprintf(author_line, sizeof(author_line), "CREADO POR %s", FIRMWARE_AUTHOR);

    renderer_draw_text(
        centered_x(author_line, 2),
        220,
        author_line,
        COLOR_YELLOW,
        COLOR_BLACK,
        2
    );

  /* static const char *hint = "PULSA CUALQUIER BOTON PARA VOLVER";

    renderer_draw_text(
        centered_x(hint, 1),
        180,
        hint,
        COLOR_WHITE,
        COLOR_BLACK,
        1
    );
*/
    renderer_flush();

    while (true) {
        controls_update();

        if (controls_menu_select()) {
            break;
        }

        sound_update();

        menu_sleep_ms(15);
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

        if (controls_menu_down()) {
            selected = (selected + 1) % OPT_COUNT;
            redraw = true;
        }

        if (controls_menu_up()) {
            selected = (selected - 1 + OPT_COUNT) % OPT_COUNT;
            redraw = true;
        }

        if (redraw) {
            sound_effect_move();
            draw_options_menu(selected);
        }

        if (controls_menu_select()) {
            sound_effect_select();

            switch (selected) {
                case OPT_TEST_CONTROLS:
                    run_test_controls();
                    break;

                case OPT_TEST_SCREEN:
                    run_test_screen();
                    break;

                case OPT_TEST_SOUND:
                    run_test_sound();
                    break;

                case OPT_CLEAR_SCORES:
                    run_clear_scores_confirm();
                    break;

                case OPT_ABOUT:
                    run_about_screen();
                    break;

                case OPT_BACK:
                    return;
            }

            /*
             * Algunas de estas pruebas (PRUEBA CONTROLES, BORRAR
             * RECORDS) leen el encoder directamente y no consumen
             * el "pending" de controls_menu_up()/down(), así que
             * puede quedar un giro acumulado -- lo descartamos al
             * volver para que no se cuele como un cambio de
             * selección aquí en OPCIONES.
             */
            menu_nav_reset();

            draw_options_menu(selected);
        }

        menu_sleep_ms(15);
    }
}


static bool show_scores_screen(
    int game_index
)
{
    renderer_clear(COLOR_BLACK);

    highscores_draw(
        game_index,
        game_name_at(game_index),
        30
    );

    renderer_flush();

    uint32_t start = now_ms();

    while (now_ms() - start < SCORES_DISPLAY_MS) {
        controls_update();

        if (
            controls_menu_up() ||
            controls_menu_down() ||
            controls_menu_select()
        ) {
            return false;
        }

        /*
         * La música no se reproduce durante
         * la pantalla de récords.
         */
        menu_sleep_ms(15);
    }

    return true;
}


static void run_attract_cycle(
    int game_index
)
{
    if (game_index == OPTIONS_INDEX) {
        return;
    }

    /*
     * El attract mode se ejecuta sin música de menú.
     */
    sound_stop_menu_music();

    if (show_scores_screen(game_index)) {
        games_list[game_index].run(
            ATTRACT_DEMO_MODE
        );

        /*
         * El demo del attract mode también lee el encoder sin
         * pasar por controls_menu_up()/down() -- descartamos
         * cualquier giro acumulado antes de volver al carrete.
         */
        menu_nav_reset();

        highscores_flush();
    }
}


/*
 * Lista de reproducción de la música de menú.
 *
 * sound_start_menu_music() por sí sola deja una pista en bucle para
 * siempre; esto añade una capa encima que va turnándose entre las
 * SOUND_MENU_TRACK_COUNT pistas (Greensleeves, Neon Circuit, Star
 * Patrol), dejando MENU_MUSIC_SILENCE_MS de silencio entre una y la
 * siguiente. El índice de pista es estático (sobrevive a volver de
 * un juego o del attract mode): la vuelta que toque sigue por donde
 * se quedó en vez de reiniciar siempre en Greensleeves.
 *
 * playlist_ms cuenta manualmente en pasos de 15ms (el propio ciclo
 * del bucle de menu_run(), ver menu_sleep_ms(15) al final) en vez de usar
 * absolute_time_t -- es la misma técnica que ya usa idle_ms más
 * abajo para el timeout del attract mode.
 */
static uint8_t  playlist_track   = 0;
static bool     playlist_silent  = false;
static uint32_t playlist_ms      = 0;

// Arranca sonando la pista "playlist_track" desde el principio.
static void menu_music_playlist_resume(void)
{
    sound_set_menu_track(playlist_track);
    sound_start_menu_music();
    playlist_silent = false;
    playlist_ms = 0;
}

// Avanza el reloj de la lista de reproducción -- llamar una vez por
// vuelta del bucle principal de menu_run(), con los mismos 15ms de
// menu_sleep_ms() que ya usa idle_ms. Solo tiene sentido mientras estamos
// en la pantalla del menú con música sonando (o en su hueco de
// silencio): NO se llama durante un juego, el attract mode o las
// opciones, que ya paran la música por su cuenta.
static void menu_music_playlist_tick(void)
{
    playlist_ms += 15;

    /*
     * Estamos en el silencio entre canciones.
     */
    if (playlist_silent) {
        sound_stop_menu_music();
        if (playlist_ms >= MENU_MUSIC_SILENCE_MS) {
            playlist_track =
                (playlist_track + 1) % SOUND_MENU_TRACK_COUNT;
            menu_music_playlist_resume();
        }

        return;
    }

    /*
     * La pista ha terminado realmente.
     * sound_update() pone menu_music_playing = false
     * al procesar la última nota.
     */
    if (!sound_menu_music_is_playing()) {
        playlist_silent = true;
        playlist_ms = 0;
    }
}


void menu_run(void)
{
    int selected = 0;
    int idle_ms = 0;

    /*
     * Inicializamos e iniciamos la música
     * solamente cuando el menú está preparado.
     *
     * La pista con la que arranca la lista de reproducción es al
     * azar (antes siempre empezaba en playlist_track=0, es decir,
     * Greensleeves) -- menu_run() se llama una sola vez desde main y
     * vive para siempre en su propio bucle, así que sembrar rand()
     * aquí, una vez, es suficiente para toda la sesión del cacharro.
     */
    sound_init();
    srand((unsigned)esp_timer_get_time());
    playlist_track = rand() % SOUND_MENU_TRACK_COUNT;
    menu_music_playlist_resume();

    renderer_clear(COLOR_BLACK);

    draw_title();
    draw_settled(selected);

    renderer_flush();

    while (true) {

        /*
         * Actualización no bloqueante de la música.
         */
        sound_update();
        menu_music_playlist_tick();

        controls_update();

        int direction = 0;
        int new_selected = selected;
        bool had_input = false;

        if (controls_menu_down()) {
            new_selected =
                wrap_index(selected + 1);

            direction = 1;
            had_input = true;
        }

        if (controls_menu_up()) {
            new_selected =
                wrap_index(selected - 1);

            direction = -1;
            had_input = true;
        }

        if (direction != 0) {
            animate_transition(
                selected,
                new_selected,
                direction
            );

            selected = new_selected;

            draw_settled(selected);

            renderer_flush();
        }

        if (controls_menu_select()) {
            had_input = true;

            /*
             * La música debe parar antes de entrar
             * en cualquier pantalla/juego.
             */
            sound_stop_menu_music();

            if (selected == OPTIONS_INDEX) {

                show_options_screen();

            } else {

                games_list[selected].run(
                    GAME_MODE_1P
                );

                highscores_flush();
            }

            /*
             * Tanto los juegos (leen el encoder con
             * controls_get_raw_delta()) como OPCIONES (algunas de
             * sus pruebas leen el encoder directamente) pueden
             * dejar un giro acumulado sin consumir en el "pending"
             * de controls_menu_up()/down(). Sin este reset, ese
             * giro se dispararía como un cambio de selección en
             * cuanto redibujemos el carrete principal -- que es
             * justo el bug que veíamos al volver de una partida.
             */
            menu_nav_reset();

            /*
             * Al regresar al menú, reiniciamos
             * la música y redibujamos todo.
             */
            menu_music_playlist_resume();

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

                selected =
                    wrap_index(selected + 1);

                /*
                 * El attract mode puede haber terminado
                 * dejando el sonido apagado, así que
                 * volvemos a arrancar la música.
                 */
                menu_music_playlist_resume();

                renderer_clear(COLOR_BLACK);

                draw_title();
                draw_settled(selected);

                renderer_flush();
            }
        }

        update_title_animation();

        menu_sleep_ms(15);
    }
}