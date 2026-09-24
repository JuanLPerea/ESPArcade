#include "controls.h"

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_timer.h"

/* ===========================================================
 * controls.c - Version ESP32: 2 joysticks analogicos + 4
 * botones + 2 pulsadores de joystick, en vez de los 2 encoders
 * en cuadratura + 4 botones de la version Pico.
 *
 * Por ahora solo se usa J1 para el menu (eje Y = arriba/abajo,
 * boton J1_A = seleccionar). J2 y el resto de botones quedan
 * listos para cuando los juegos los necesiten.
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

#define ADC_CH_J1_X ADC_CHANNEL_4 // GPIO32
#define ADC_CH_J1_Y ADC_CHANNEL_5 // GPIO33
#define ADC_CH_J2_X ADC_CHANNEL_6 // GPIO34
#define ADC_CH_J2_Y ADC_CHANNEL_7 // GPIO35

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

static adc_oneshot_unit_handle_t s_adc1;
static int s_axis_j1x_center, s_axis_j1y_center;
static int s_axis_j1x_filtered, s_axis_j1y_filtered;

/* -----------------------------------------------------------------
 * Botones: polling simple con deteccion de flanco. No hace falta
 * debounce por software aparte -- el ruido mecanico de un pulsador
 * normal no sobrevive a los ~15 ms de periodo del bucle del menu.
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
 * zona muerta + histeresis + autorepeticion mientras se mantiene
 * inclinado. Ver explicacion detallada de por que hace falta cada
 * parte de esto en la conversacion sobre sustituir encoders por
 * joysticks.
 * ----------------------------------------------------------------- */
#define AXIS_EMA_SHIFT      3     // filtro paso-bajo, alfa = 1/8
#define AXIS_DEADZONE_RAW   250   // sobre 0-4095, sin dato aun del joystick real
#define AXIS_RELEASE_RAW    120   // histeresis: hay que volver mas cerca del centro para "soltar"
#define AXIS_REPEAT_MS      180

static bool s_axis_y_held_down = false, s_axis_y_held_up = false;
static uint32_t s_axis_y_last_repeat_ms = 0;

static void axis_poll_and_update_menu_events(bool *out_up, bool *out_down) {
    *out_up = false;
    *out_down = false;

    int raw = 0;
    adc_oneshot_read(s_adc1, ADC_CH_J1_Y, &raw);
    s_axis_j1y_filtered += (raw - s_axis_j1y_filtered) >> AXIS_EMA_SHIFT;
    int delta = s_axis_j1y_filtered - s_axis_j1y_center;

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    if (delta > AXIS_DEADZONE_RAW) {
        if (!s_axis_y_held_down || (now - s_axis_y_last_repeat_ms) > AXIS_REPEAT_MS) {
            s_axis_y_held_down = true;
            s_axis_y_last_repeat_ms = now;
            *out_down = true;
        }
    } else if (delta < AXIS_RELEASE_RAW) {
        s_axis_y_held_down = false;
    }

    if (delta < -AXIS_DEADZONE_RAW) {
        if (!s_axis_y_held_up || (now - s_axis_y_last_repeat_ms) > AXIS_REPEAT_MS) {
            s_axis_y_held_up = true;
            s_axis_y_last_repeat_ms = now;
            *out_up = true;
        }
    } else if (delta > -AXIS_RELEASE_RAW) {
        s_axis_y_held_up = false;
    }
}

static bool s_pending_up = false, s_pending_down = false;

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
    adc_oneshot_config_channel(s_adc1, ADC_CH_J1_X, &chan_cfg);
    adc_oneshot_config_channel(s_adc1, ADC_CH_J1_Y, &chan_cfg);
    adc_oneshot_config_channel(s_adc1, ADC_CH_J2_X, &chan_cfg);
    adc_oneshot_config_channel(s_adc1, ADC_CH_J2_Y, &chan_cfg);

    // Calibracion del centro: el joystick DEBE estar soltado en
    // este momento del arranque. Si mas adelante conectas los
    // joysticks y el menu no responde bien, revisa que no esten
    // tocados durante el arranque de la placa.
    long accx = 0, accy = 0;
    for (int n = 0; n < 32; n++) {
        int raw;
        adc_oneshot_read(s_adc1, ADC_CH_J1_X, &raw); accx += raw;
        adc_oneshot_read(s_adc1, ADC_CH_J1_Y, &raw); accy += raw;
    }
    s_axis_j1x_center = (int)(accx / 32);
    s_axis_j1y_center = (int)(accy / 32);
    s_axis_j1x_filtered = s_axis_j1x_center;
    s_axis_j1y_filtered = s_axis_j1y_center;
}

void controls_update(void) {
    buttons_poll();

    bool up = false, down = false;
    axis_poll_and_update_menu_events(&up, &down);
    if (up) s_pending_up = true;
    if (down) s_pending_down = true;
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