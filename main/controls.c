#include "controls.h"

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_timer.h"

/* ===========================================================
 * controls.c - Version ESP32: 2 joysticks analogicos + 4
 * botones + 2 pulsadores de joystick, en vez de los 2 encoders
 * en cuadratura + 4 botones de la version Pico.
 *
 * Pinout (igual que el resto de la conversacion):
 *   J1  VRx=32 VRy=33 SW=19   BTN_A=4  BTN_B=14
 *   J2  VRx=34 VRy=35 SW=13   BTN_A=16 BTN_B=17
 * =========================================================== */

#define PIN_J1_SW    19
#define PIN_J1_BTN_A 4
#define PIN_J1_BTN_B 14
#define PIN_J2_SW    13
#define PIN_J2_BTN_A 16
#define PIN_J2_BTN_B 17

typedef enum {
    BTN_J1_A = 0, BTN_J1_B, BTN_J2_A, BTN_J2_B, BTN_J1_SW, BTN_J2_SW, BTN_COUNT
} button_id_t;

static const gpio_num_t s_btn_pin[BTN_COUNT] = {
    [BTN_J1_A] = PIN_J1_BTN_A, [BTN_J1_B] = PIN_J1_BTN_B,
    [BTN_J2_A] = PIN_J2_BTN_A, [BTN_J2_B] = PIN_J2_BTN_B,
    [BTN_J1_SW] = PIN_J1_SW,   [BTN_J2_SW] = PIN_J2_SW,
};
static bool s_btn_level[BTN_COUNT];
static bool s_btn_prev[BTN_COUNT];

/* -----------------------------------------------------------------
 * Ejes: 0=J1_X 1=J1_Y 2=J2_X 3=J2_Y, todos por ADC1.
 * ----------------------------------------------------------------- */
typedef enum { AXIS_J1_X = 0, AXIS_J1_Y, AXIS_J2_X, AXIS_J2_Y, AXIS_COUNT } axis_id_t;

static const adc_channel_t s_axis_ch[AXIS_COUNT] = {
    [AXIS_J1_X] = ADC_CHANNEL_4, // GPIO32
    [AXIS_J1_Y] = ADC_CHANNEL_5, // GPIO33
    [AXIS_J2_X] = ADC_CHANNEL_6, // GPIO34
    [AXIS_J2_Y] = ADC_CHANNEL_7, // GPIO35
};

static adc_oneshot_unit_handle_t s_adc1;
static int s_axis_center[AXIS_COUNT];
static int s_axis_filtered[AXIS_COUNT];

#define AXIS_EMA_SHIFT 3 // filtro paso-bajo, alfa = 1/8

/* -----------------------------------------------------------------
 * Botones: polling simple con deteccion de flanco.
 * ----------------------------------------------------------------- */
static void buttons_poll(void) {
    for (int i = 0; i < BTN_COUNT; i++) {
        s_btn_prev[i] = s_btn_level[i];
        s_btn_level[i] = (gpio_get_level(s_btn_pin[i]) == 0); // pull-up: 0 = pulsado
    }
}
static bool btn_pressed(button_id_t b) {
    return s_btn_level[b] && !s_btn_prev[b];
}

/* -----------------------------------------------------------------
 * Eje Y de J1 -> eventos discretos arriba/abajo para el menu, con
 * zona muerta + histeresis + autorepeticion.
 * ----------------------------------------------------------------- */
#define MENU_AXIS_DEADZONE_RAW   250 // sobre 0-4095
#define MENU_AXIS_RELEASE_RAW    120 // histeresis
#define MENU_AXIS_REPEAT_MS      180

static bool s_axis_y_held_down = false, s_axis_y_held_up = false;
static uint32_t s_axis_y_last_repeat_ms = 0;
static bool s_pending_up = false, s_pending_down = false;

static void axis_poll_all(void) {
    for (int a = 0; a < AXIS_COUNT; a++) {
        int raw = 0;
        adc_oneshot_read(s_adc1, s_axis_ch[a], &raw);
        s_axis_filtered[a] += (raw - s_axis_filtered[a]) >> AXIS_EMA_SHIFT;
    }
}

static void menu_events_from_j1y(void) {
    int delta = s_axis_filtered[AXIS_J1_Y] - s_axis_center[AXIS_J1_Y];
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    if (delta > MENU_AXIS_DEADZONE_RAW) {
        if (!s_axis_y_held_down || (now - s_axis_y_last_repeat_ms) > MENU_AXIS_REPEAT_MS) {
            s_axis_y_held_down = true;
            s_axis_y_last_repeat_ms = now;
            s_pending_down = true;
        }
    } else if (delta < MENU_AXIS_RELEASE_RAW) {
        s_axis_y_held_down = false;
    }

    if (delta < -MENU_AXIS_DEADZONE_RAW) {
        if (!s_axis_y_held_up || (now - s_axis_y_last_repeat_ms) > MENU_AXIS_REPEAT_MS) {
            s_axis_y_held_up = true;
            s_axis_y_last_repeat_ms = now;
            s_pending_up = true;
        }
    } else if (delta > -MENU_AXIS_RELEASE_RAW) {
        s_axis_y_held_up = false;
    }
}

/* -----------------------------------------------------------------
 * API publica
 * ----------------------------------------------------------------- */
void controls_init(void) {
    for (int i = 0; i < BTN_COUNT; i++) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << s_btn_pin[i],
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&cfg);
        s_btn_level[i] = false;
        s_btn_prev[i] = false;
    }

    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ADC_UNIT_1 };
    adc_oneshot_new_unit(&unit_cfg, &s_adc1);

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN_DB_12,
    };
    for (int a = 0; a < AXIS_COUNT; a++) {
        adc_oneshot_config_channel(s_adc1, s_axis_ch[a], &chan_cfg);
    }

    // Calibracion del centro: los joysticks DEBEN estar soltados en
    // este momento del arranque.
    long acc[AXIS_COUNT] = {0};
    for (int n = 0; n < 32; n++) {
        for (int a = 0; a < AXIS_COUNT; a++) {
            int raw;
            adc_oneshot_read(s_adc1, s_axis_ch[a], &raw);
            acc[a] += raw;
        }
    }
    for (int a = 0; a < AXIS_COUNT; a++) {
        s_axis_center[a] = (int)(acc[a] / 32);
        s_axis_filtered[a] = s_axis_center[a];
    }
}

void controls_update(void) {
    buttons_poll();
    axis_poll_all();
    menu_events_from_j1y();
}

bool controls_menu_up(void) {
    bool v = s_pending_up;
    s_pending_up = false;
    return v;
}

bool controls_menu_down(void) {
    bool v = s_pending_down;
    s_pending_down = false;
    return v;
}

bool controls_menu_select(void) {
    // El boton A de J1 o el click del propio joystick sirven para
    // seleccionar -- lo que tengas conectado en cada momento.
    return btn_pressed(BTN_J1_A) || btn_pressed(BTN_J1_SW);
}

// Escala el eje Y (centrado, +-2048 aprox de recorrido real) a un
// "delta" pequeño con signo, pensado para alimentar directamente el
// sistema de aceleracion/inercia (enc_momentum en pong.c) que antes
// recibia cuentas de encoder. /200 da un rango util de
// aproximadamente -10..10 a fondo de recorrido.
#define RAW_DELTA_DIVISOR 200
#define RAW_DELTA_DEADZONE 60 // ignora el ruido cerca del centro

int controls_get_raw_delta(int idx) {
    axis_id_t axis = (idx == 0) ? AXIS_J1_Y : AXIS_J2_Y;
    int delta = s_axis_filtered[axis] - s_axis_center[axis];
    if (delta > -RAW_DELTA_DEADZONE && delta < RAW_DELTA_DEADZONE) return 0;
    return delta / RAW_DELTA_DIVISOR;
}

bool controls_button_down(int idx) {
    if (idx < 0 || idx >= BTN_COUNT) return false;
    return s_btn_level[idx];
}

int controls_debug_axis_raw(int axis_id) {
    if (axis_id < 0 || axis_id >= AXIS_COUNT) return 0;
    return s_axis_filtered[axis_id];
}

int controls_debug_axis_normalized(int axis_id) {
    if (axis_id < 0 || axis_id >= AXIS_COUNT) return 0;
    int delta = s_axis_filtered[axis_id] - s_axis_center[axis_id];
    long norm = ((long)delta * 1000L) / 2000L;
    if (norm > 1000) norm = 1000;
    if (norm < -1000) norm = -1000;
    return (int)norm;
}
