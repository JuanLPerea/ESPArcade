#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pong.h"
#include "../renderer.h"
#include "../controls.h"
#include "../highscores.h"
#include "../sound.h"

/*
 * Pong -- portado del proyecto ArcadeColor (Pico) a ESP32.
 *
 *  - time_us_32() (Pico SDK) -> esp_timer_get_time() para sembrar
 *    srand() y para medir el dt real de cada frame.
 *  - Controles: controls_get_raw_delta() sigue existiendo con la
 *    misma firma, pero ahora por debajo lee un joystick analogico
 *    (eje Y) en vez de un encoder en cuadratura -- ver controls.c.
 *
 *  - FISICA REESCRITA A TIEMPO REAL (no "por tick"): la version
 *    original (Pico) y la primera version de este port movian bola
 *    y palas una cantidad fija de pixeles POR TICK del bucle
 *    principal, asumiendo que sleep_ms()/vTaskDelay() esperaba
 *    siempre lo mismo entre iteraciones. Eso rompia en ESP32 porque
 *    vTaskDelay(pdMS_TO_TICKS(8)) se redondeaba a 0 con el tick rate
 *    de FreeRTOS por defecto (100 Hz), asi que el juego iba
 *    muchisimo mas rapido y con jitter (variaba segun que otras
 *    tareas de FreeRTOS estuvieran listas en cada momento).
 *
 *    Ahora todas las velocidades (bola, palas, IA) estan en PIXELES
 *    POR SEGUNDO (px/s) y game_pong_run() mide el dt real entre
 *    frames con esp_timer_get_time(), pasandolo a pong_tick(dt) y de
 *    ahi a toda la fisica. El propio bucle se marca con
 *    vTaskDelayUntil() a un periodo objetivo fijo (ver TARGET_FPS)
 *    en vez de vTaskDelay(), para tener un ritmo estable sin ir
 *    acumulando retraso frame a frame.
 */

// ---------------------------------------------------------------------------
// Área de juego, proporcional a la pantalla real (320x240 apaisada)
// ---------------------------------------------------------------------------
#define SCREEN_W 320
#define SCREEN_H 240

#define PLAY_X   4
#define PLAY_Y   3
#define PLAY_W   (SCREEN_W - 2 * PLAY_X)                 // 312
#define PLAY_H   (SCREEN_H - 2 * PLAY_Y)                 // 234
#define CX       (PLAY_X + PLAY_W / 2)
#define CY       (PLAY_Y + PLAY_H / 2)

#define PADDLE_W      6
#define PADDLE_MARGIN 6
#define BALL_SZ       8

#define COLOR_P1 COLOR_CYAN
#define COLOR_P2 COLOR_YELLOW

#define PADDLE_H      (PLAY_H * 72 / 400)   // pala fija de la IA / P2
#define PADDLE_H_LV0  (PLAY_H * 72 / 400)
#define PADDLE_H_LV1  (PLAY_H * 56 / 400)
#define PADDLE_H_LV2  (PLAY_H * 44 / 400)
#define PADDLE_H_LV3  (PLAY_H * 34 / 400)

// ---------------------------------------------------------------------------
// Velocidades en PIXELES POR SEGUNDO (px/s), no "por tick": así el juego se
// mueve igual de rápido pase lo que pase con el framerate real. La velocidad
// de saque depende del nivel del jugador (mismo player_level que ya se usa
// para el tamaño de la pala); cada rebote la va subiendo hasta un máximo.
// ---------------------------------------------------------------------------
#define BALL_BASE_SPEED_LV0  140.0f   // saque lento al empezar / nivel 0
#define BALL_BASE_SPEED_LV1  170.0f
#define BALL_BASE_SPEED_LV2  200.0f
#define BALL_BASE_SPEED_LV3  230.0f
#define BALL_SPEED_INC        18.0f   // incremento por cada rebote en pala
#define BALL_SPEED_MAX        380.0f  // techo de velocidad, nunca se supera
#define BALL_MAX_ANGLE   1.0f
#define BALL_SERVE_ANGLE 0.35f
#define BALL_DEMO_MIN_ANGLE  0.30f
#define BALL_DEMO_MAX_ANGLE  0.75f

#define SCORE_WIN     15
#define SCORE_AI_WIN  15

// Movimiento de pala del jugador humano, también en unidades reales.
#define PAD_ACCEL          900.0f  // px/s^2 por "unidad" de inclinación de joystick
#define PAD_VEL_MAX        600.0f  // px/s, tope de velocidad de la pala
#define PAD_DECAY_PER_SEC  7.0f    // cuanto más alto, más rápido frena la pala al soltar el stick

// Timers por FRAMES: siguen siendo válidos porque el bucle principal ahora
// se marca a un ritmo objetivo estable con vTaskDelayUntil() (ver
// game_pong_run) -- lo único que necesitaba tiempo real era la FÍSICA
// (bola/palas), no estos contadores de frames.
#define PAUSE_TICKS   40
#define BLINK_HALF    14
#define LEVELUP_TICKS (PAUSE_TICKS * 3)
#define DEMO_TIMEOUT_TICKS (PAUSE_TICKS * 30)

// Ritmo objetivo del bucle de juego y paso de simulación fijo que usa la IA
// para predecir la trayectoria de la bola (ai_predict_ball_y).
#define TARGET_FPS     60
#define SIM_DT         (1.0f / TARGET_FPS)
// Si un frame tarda anormalmente (flush SPI lento, otra tarea de FreeRTOS
// que ha acaparado la CPU, etc.) no queremos que la física dé un salto
// gigante: se limita el dt real a esto como máximo.
#define MAX_DT         0.05f

// ---------------------------------------------------------------------------
// IA Adaptativa -- identica al original.
// ---------------------------------------------------------------------------
static const float AI_BASE_SPEED[4] = { 90.0f, 150.0f, 220.0f, 340.0f }; // px/s
#define AI_SPEED_CAP 480.0f                                              // px/s
#define AI_CENTER_SPEED 220.0f  // px/s, recentrado suave cuando la bola está lejos
#define DEMO_AI_SPEED   260.0f  // px/s, velocidad de ambas palas en el modo demo (attract mode)
static const int AI_BASE_VISION[4] = {
    PLAY_X + PLAY_W*4/5,
    PLAY_X + PLAY_W*3/5,
    PLAY_X + PLAY_W*2/5,
    0,
};
static const int AI_BASE_ERROR[4] = { 32, 24, 18, 10 };

typedef struct {
    int   avg_center_y;
    int   zone_bias;
    int   streak;
    int   player_total_pts;
    int   ai_total_pts;
    int   ai_error;
    int   ai_error_dir;
    float ai_speed;   // px/s
    int   target_y;
    bool  target_valid;
} AIProfile;

static AIProfile ai_prof;

typedef enum { S_SELECT, S_SERVE, S_PLAYING, S_PAUSE, S_OVER, S_SCORES } State;
// x/y y bx/by en float: posición sub-píxel y velocidad en px/s (antes eran
// enteros y "px por tick", lo que ataba la física al framerate).
typedef struct { float x, y, bx, by; } Ball;
typedef struct { int x; float y; int score; float vel; } Pad;

static Ball  ball;
static Pad   p1, p2;
static State state;
static float ball_speed;
static bool  two_p;
static bool  demo;
static int   demo_ticks;
static int   serve_side;
static int   pause_cnt;
static int   blink;
static int   player_level;
static int   levelup_timer;
static int   paddle_h;
static bool  g_done;
static int   menu_enc_acc = 0;

static int prev_ball_x = -1, prev_ball_y = -1;
static int prev_p1_y = -1, prev_p2_y = -1;
static int prev_paddle_h = -1;
static int prev_p1_score = -1, prev_p2_score = -1;
static bool field_needs_redraw = true;

static char prev_bottom_msg[32] = "";
static bool prev_levelup_shown = false;

static int clamp(int v, int lo, int hi) { return v<lo?lo:v>hi?hi:v; }
static float clampf(float v, float lo, float hi) { return v<lo?lo:v>hi?hi:v; }

#define MENU_ANIM_Y0   (CY-33)
#define MENU_ANIM_Y1   (CY-12)
#define MENU_ANIM_H    (MENU_ANIM_Y1 - MENU_ANIM_Y0)
#define MENU_PAD_W     3
#define MENU_PAD_H     12
#define MENU_BALL_SZ   4
#define MENU_ANIM_X0   (CX-50)
#define MENU_ANIM_X1   (CX+50-MENU_PAD_W)
#define MENU_BALL_MINX (MENU_ANIM_X0+MENU_PAD_W)
#define MENU_BALL_MAXX (MENU_ANIM_X1-MENU_BALL_SZ)

#define MENU_ANIM_VX_PPS   66.0f  // px/s (antes 1.1 px/tick)
#define MENU_ANIM_VY_PPS   30.0f  // px/s (antes 0.5 px/tick)
#define MENU_PAD_FOLLOW_PPS 90.0f // px/s a la que las palas del menú siguen a la bola

static bool  menu_anim_active = false;
static float menu_ball_x, menu_ball_y, menu_ball_vx, menu_ball_vy;
static float menu_p1_yf, menu_p2_yf; // posición sub-píxel de las palas cosméticas
static int   menu_p1_y, menu_p2_y;   // posición redondeada usada para dibujar
static int   menu_prev_ball_x = -1, menu_prev_ball_y = -1;
static int   menu_prev_p1_y = -1, menu_prev_p2_y = -1;

static void init_menu_anim(void) {
    menu_ball_x = (float)(CX - MENU_BALL_SZ/2);
    menu_ball_y = (float)(MENU_ANIM_Y0 + MENU_ANIM_H/2 - MENU_BALL_SZ/2);
    menu_ball_vx = MENU_ANIM_VX_PPS;
    menu_ball_vy = MENU_ANIM_VY_PPS;
    menu_p1_yf = menu_p2_yf = (float)(MENU_ANIM_Y0 + MENU_ANIM_H/2 - MENU_PAD_H/2);
    menu_p1_y = menu_p2_y = (int)menu_p1_yf;
    menu_prev_ball_x = menu_prev_ball_y = -1;
    menu_prev_p1_y = menu_prev_p2_y = -1;
    menu_anim_active = true;
}

static void menu_anim_tick(float dt) {
    if (!menu_anim_active) return;

    menu_ball_x += menu_ball_vx * dt;
    menu_ball_y += menu_ball_vy * dt;

    if (menu_ball_y <= MENU_ANIM_Y0 || menu_ball_y >= MENU_ANIM_Y1 - MENU_BALL_SZ)
        menu_ball_vy = -menu_ball_vy;
    if (menu_ball_x <= MENU_BALL_MINX) { menu_ball_x = MENU_BALL_MINX; menu_ball_vx =  fabsf(menu_ball_vx); }
    if (menu_ball_x >= MENU_BALL_MAXX) { menu_ball_x = MENU_BALL_MAXX; menu_ball_vx = -fabsf(menu_ball_vx); }

    float target = (float)((int)menu_ball_y - (MENU_PAD_H-MENU_BALL_SZ)/2);
    target = clampf(target, (float)MENU_ANIM_Y0, (float)(MENU_ANIM_Y1-MENU_PAD_H));
    float step = MENU_PAD_FOLLOW_PPS * dt;
    menu_p1_yf = clampf(target, menu_p1_yf - step, menu_p1_yf + step);
    menu_p2_yf = clampf(target, menu_p2_yf - step, menu_p2_yf + step);
    menu_p1_y = (int)(menu_p1_yf + 0.5f);
    menu_p2_y = (int)(menu_p2_yf + 0.5f);

    if (menu_prev_ball_x >= 0)
        renderer_fill_rect(menu_prev_ball_x, menu_prev_ball_y, MENU_BALL_SZ, MENU_BALL_SZ, COLOR_BLACK);
    if (menu_prev_p1_y >= 0)
        renderer_fill_rect(MENU_ANIM_X0, menu_prev_p1_y, MENU_PAD_W, MENU_PAD_H, COLOR_BLACK);
    if (menu_prev_p2_y >= 0)
        renderer_fill_rect(MENU_ANIM_X1, menu_prev_p2_y, MENU_PAD_W, MENU_PAD_H, COLOR_BLACK);

    renderer_fill_rect((int)menu_ball_x, (int)menu_ball_y, MENU_BALL_SZ, MENU_BALL_SZ, COLOR_YELLOW);
    renderer_fill_rect(MENU_ANIM_X0, menu_p1_y, MENU_PAD_W, MENU_PAD_H, COLOR_WHITE);
    renderer_fill_rect(MENU_ANIM_X1, menu_p2_y, MENU_PAD_W, MENU_PAD_H, COLOR_WHITE);

    menu_prev_ball_x = (int)menu_ball_x; menu_prev_ball_y = (int)menu_ball_y;
    menu_prev_p1_y = menu_p1_y; menu_prev_p2_y = menu_p2_y;

    renderer_flush();
}

// dy es un desplazamiento en píxeles YA escalado por dt por quien llama
// (p.ej. velocidad_px_s * dt), no una velocidad -- así esta función no
// necesita saber nada de tiempo, solo aplica el movimiento y recorta al
// terreno de juego.
static void pad_move(Pad *p, float dy) {
    int ph = (p == &p1) ? paddle_h : PADDLE_H;
    p->y = clampf(p->y + dy, (float)PLAY_Y, (float)(PLAY_Y + PLAY_H - ph));
}

// *vel es la velocidad real de la pala en px/s. Con stick inclinado acelera
// (px/s^2 * dt); al soltar, decae exponencialmente -- e^(-k*dt) frena a la
// misma "sensación" de suavidad sea cual sea el framerate real, a diferencia
// del *=6/10 por tick que había antes (que frenaba más o menos según lo
// rápido que fuera el bucle).
static float enc_momentum(int enc_raw, float *vel, float dt) {
    if (enc_raw != 0) {
        *vel += PAD_ACCEL * (float)enc_raw * dt;
        if (*vel >  PAD_VEL_MAX) *vel =  PAD_VEL_MAX;
        if (*vel < -PAD_VEL_MAX) *vel = -PAD_VEL_MAX;
    } else {
        *vel *= expf(-PAD_DECAY_PER_SEC * dt);
    }
    return *vel;
}

static int ai_lv(void) {
    int total = p1.score + p2.score;
    return total >= 20 ? 3 : total >= 12 ? 2 : total >= 5 ? 1 : 0;
}

static int player_paddle_h(void) {
    if (player_level >= 3) return PADDLE_H_LV3;
    if (player_level >= 2) return PADDLE_H_LV2;
    if (player_level >= 1) return PADDLE_H_LV1;
    return PADDLE_H_LV0;
}

// Velocidad de saque: SIEMPRE se reinicia a este valor "lento" al empezar
// un punto (ver ball_launch), en vez de arrastrar la velocidad acelerada
// del rally anterior. Sube un poco con el nivel del jugador (mismo
// player_level que ya reduce el tamaño de su pala), para que el saque
// también se ponga progresivamente más difícil sin dejar de ser el
// momento "de respiro" de cada punto.
static float ball_base_speed_for_level(void) {
    if (player_level >= 3) return BALL_BASE_SPEED_LV3;
    if (player_level >= 2) return BALL_BASE_SPEED_LV2;
    if (player_level >= 1) return BALL_BASE_SPEED_LV1;
    return BALL_BASE_SPEED_LV0;
}

static int ai_predict_ball_y(void) {
    float bx = ball.bx, by = ball.by;
    float x  = ball.x,  y  = ball.y;
    float target_x = (float)p2.x;

    if (bx <= 0.0f) return (int)(y + BALL_SZ / 2);

    // Simulación a paso de tiempo fijo (SIM_DT), no "un paso = un tick real":
    // así la predicción no depende del framerate real del dispositivo.
    for (int step = 0; step < 600; step++) {
        x += bx * SIM_DT;
        y += by * SIM_DT;
        if (y <= PLAY_Y)                    { y = PLAY_Y;               by = -by; }
        if (y + BALL_SZ >= PLAY_Y + PLAY_H) { y = PLAY_Y + PLAY_H - BALL_SZ; by = -by; }
        if (x + BALL_SZ >= target_x) return (int)(y + BALL_SZ / 2);
    }
    return (int)(y + BALL_SZ / 2);
}

static void ai_profile_update(bool player_scored) {
    int lv = ai_lv();

    if (player_scored) {
        ai_prof.streak = ai_prof.streak > 0 ? ai_prof.streak + 1 : 1;
        ai_prof.player_total_pts++;
    } else {
        ai_prof.streak = ai_prof.streak < 0 ? ai_prof.streak - 1 : -1;
        ai_prof.ai_total_pts++;
    }

    int p1_center = (int)p1.y + paddle_h / 2;
    int play_mid  = PLAY_Y + PLAY_H / 2;
    ai_prof.avg_center_y = (ai_prof.avg_center_y * 4 + p1_center) / 5;
    if (p1_center > play_mid + 10)       ai_prof.zone_bias++;
    else if (p1_center < play_mid - 10)  ai_prof.zone_bias--;
    if (ai_prof.zone_bias >  8) ai_prof.zone_bias =  8;
    if (ai_prof.zone_bias < -8) ai_prof.zone_bias = -8;

    int base_err = AI_BASE_ERROR[lv];
    if (ai_prof.streak >= 5)       base_err = base_err * 58 / 100;
    else if (ai_prof.streak >= 4)  base_err = base_err * 70 / 100;
    else if (ai_prof.streak >= 3)  base_err = base_err * 82 / 100;
    else if (ai_prof.streak >= 2)  base_err = base_err * 92 / 100;

    if (lv < 3) {
        if (ai_prof.streak <= -4)       base_err = base_err * 140 / 100;
        else if (ai_prof.streak <= -2)  base_err = base_err * 120 / 100;
    }

    if (base_err < 4)  base_err = 4;
    if (base_err > 80) base_err = 80;
    ai_prof.ai_error = base_err;

    int r = (int)(rand() % 100);
    if (ai_prof.zone_bias > 2)       ai_prof.ai_error_dir = (r < 65) ? -1 : 1;
    else if (ai_prof.zone_bias < -2) ai_prof.ai_error_dir = (r < 65) ?  1 : -1;
    else                             ai_prof.ai_error_dir = (r < 50) ? 1 : -1;

    float spd = AI_BASE_SPEED[lv];
    if (ai_prof.streak >= 3) spd = spd * 1.15f;
    if (ai_prof.streak >= 5) spd = spd * 1.20f;
    if (spd > AI_SPEED_CAP) spd = AI_SPEED_CAP;
    ai_prof.ai_speed = spd;

    ai_prof.target_valid = false;
}

static void ai_profile_init(void) {
    ai_prof.avg_center_y  = CY;
    ai_prof.zone_bias     = 0;
    ai_prof.streak        = 0;
    ai_prof.player_total_pts = 0;
    ai_prof.ai_total_pts     = 0;
    ai_prof.ai_error      = AI_BASE_ERROR[0];
    ai_prof.ai_error_dir  = 1;
    ai_prof.ai_speed      = AI_BASE_SPEED[0];
    ai_prof.target_y      = CY;
    ai_prof.target_valid  = false;
}

static void ai_move_adaptive(Pad *p, float dt) {
    int   lv     = ai_lv();
    int   vis_x  = AI_BASE_VISION[lv];
    float speed  = ai_prof.ai_speed; // px/s

    if (ball.x < vis_x) {
        float default_y = (float)CY;
        if (ai_prof.zone_bias > 2)       default_y = (float)(CY - 30);
        else if (ai_prof.zone_bias < -2) default_y = (float)(CY + 30);
        float diff = default_y - (p->y + PADDLE_H / 2.0f);
        if (fabsf(diff) > 4.0f) {
            float step = AI_CENTER_SPEED * dt;
            pad_move(p, clampf(diff, -step, step));
        }
        return;
    }

    if (!ai_prof.target_valid) {
        int pred_y = ai_predict_ball_y();
        ai_prof.target_y   = pred_y + ai_prof.ai_error * ai_prof.ai_error_dir;
        ai_prof.target_y   = clamp(ai_prof.target_y, PLAY_Y + PADDLE_H/2, PLAY_Y + PLAY_H - PADDLE_H/2);
        ai_prof.target_valid = true;
    }

    float diff = (float)ai_prof.target_y - (p->y + PADDLE_H / 2.0f);
    if (fabsf(diff) <= 1.0f) return;
    float step = speed * dt;
    pad_move(p, clampf(diff, -step, step));
}

// speed en px/s (velocidad tope de esta pala en modo demo).
static void ai_move(Pad *p, int vis_x, float speed, float dt) {
    if (ball.x < vis_x) return;
    float diff = (ball.y + BALL_SZ/2.0f) - (p->y + PADDLE_H/2.0f);
    if (fabsf(diff) <= 1.0f) return;
    float step = speed * dt;
    pad_move(p, clampf(diff, -step, step));
}

static void reset_pads(void) {
    p1.x = PLAY_X + PADDLE_MARGIN;
    p1.y = (float)(CY - paddle_h/2);
    p1.vel = 0.0f;
    p2.x = PLAY_X + PLAY_W - PADDLE_MARGIN - PADDLE_W;
    p2.y = (float)(CY - PADDLE_H/2);
    p2.vel = 0.0f;
}

static void ball_centre(void) {
    ball.x = (float)(CX - BALL_SZ/2);
    ball.y = (float)(CY - BALL_SZ/2);
    ball.bx = ball.by = 0.0f;
}

static void set_ball_velocity(float speed, float angle_rad, int dir) {
    float fbx = cosf(angle_rad) * speed;
    float fby = sinf(angle_rad) * speed;

    // Asegura una componente horizontal mínima visible (que la bola nunca
    // se quede casi vertical eternamente), ya en px/s, no hace falta
    // redondear a entero como antes.
    if (fbx < 1.0f) fbx = 1.0f;
    ball.bx = (float)dir * fbx;
    ball.by = fby;
}

static void ball_launch(void) {
    // Siempre se reinicia a la velocidad de saque (lenta), NUNCA hereda la
    // velocidad acelerada del rally anterior -- así "cuando saca un
    // jugador va más despacio" pase lo que pase con el rally previo.
    ball_speed = ball_base_speed_for_level();

    float angle;

    if (demo) {
        float r = (float)(rand() % 1000) / 1000.0f;
        float magnitude = BALL_DEMO_MIN_ANGLE +
                          r * (BALL_DEMO_MAX_ANGLE - BALL_DEMO_MIN_ANGLE);
        if (rand() & 1)
            angle = magnitude;
        else
            angle = -magnitude;
    } else {
        float r = (float)(rand() % 1000) / 1000.0f;
        angle = (r * 2.0f - 1.0f) * BALL_SERVE_ANGLE;
    }

    int dir = (serve_side == 0) ? 1 : -1;
    set_ball_velocity(ball_speed, angle, dir);
}

static bool hits(Pad *p) {
    int ph = (p == &p1) ? paddle_h : PADDLE_H;
    int hx0 = (p == &p1) ? p->x - 4 : p->x;
    int hx1 = (p == &p1) ? p->x + PADDLE_W : p->x + PADDLE_W + 4;
    return ball.x          < hx1          &&
           ball.x + BALL_SZ > hx0          &&
           ball.y          < p->y + ph     &&
           ball.y + BALL_SZ > p->y;
}

static void bounce(Pad *p, bool left) {
    int ph = (p == &p1) ? paddle_h : PADDLE_H;
    float diff = (ball.y + BALL_SZ/2.0f) - (p->y + ph/2.0f);

    float norm = diff / (float)(ph/2);
    if (norm > 1.0f) norm = 1.0f;
    if (norm < -1.0f) norm = -1.0f;
    float angle = norm * BALL_MAX_ANGLE;

    if (!left && !two_p) {
        float jitter = ((float)(rand() % 100) / 100.0f - 0.5f) * 0.2f;
        angle += jitter;
        if (angle > BALL_MAX_ANGLE)  angle = BALL_MAX_ANGLE;
        if (angle < -BALL_MAX_ANGLE) angle = -BALL_MAX_ANGLE;
    }

    ball_speed += BALL_SPEED_INC;
    if (ball_speed > BALL_SPEED_MAX) ball_speed = BALL_SPEED_MAX;

    set_ball_velocity(ball_speed, angle, left ? 1 : -1);
    ball.x = left ? p->x + PADDLE_W : p->x - BALL_SZ;
    sound_effect_shoot();
}

// ---------------------------------------------------------------------------
// Dibujo
// ---------------------------------------------------------------------------
static int centered_x(const char *text, int scale) {
    int w = (int)st7789_text_width(text, (uint8_t)scale);
    int x = (TFT_WIDTH - w) / 2;
    return (x < 0) ? 0 : x;
}

static void draw_field_static(void) {
    renderer_clear(COLOR_BLACK);
    renderer_fill_rect(PLAY_X, PLAY_Y,          PLAY_W, 2, COLOR_WHITE);
    renderer_fill_rect(PLAY_X, PLAY_Y+PLAY_H-2, PLAY_W, 2, COLOR_WHITE);
    for (int y = PLAY_Y+4; y < PLAY_Y+PLAY_H-10; y += 16)
        renderer_fill_rect(CX-1, y, 2, 8, COLOR_WHITE);

    prev_ball_x = prev_ball_y = -1;
    prev_p1_y = prev_p2_y = -1;
    prev_paddle_h = -1;
    prev_p1_score = prev_p2_score = -1;
    prev_bottom_msg[0] = '\0';
    prev_levelup_shown = false;
    field_needs_redraw = false;
}

static bool draw_score_if_changed(void) {
    char buf[4];
    bool changed = false;
    if (p1.score != prev_p1_score) {
        renderer_fill_rect(CX-90, PLAY_Y+6, 40, 24, COLOR_BLACK);
        snprintf(buf, sizeof(buf), "%d", p1.score);
        renderer_draw_text(CX-90, PLAY_Y+6, buf, COLOR_WHITE, COLOR_BLACK, 3);
        prev_p1_score = p1.score;
        changed = true;
    }
    if (p2.score != prev_p2_score) {
        renderer_fill_rect(CX+50, PLAY_Y+6, 40, 24, COLOR_BLACK);
        snprintf(buf, sizeof(buf), "%d", p2.score);
        renderer_draw_text(CX+50, PLAY_Y+6, buf, COLOR_WHITE, COLOR_BLACK, 3);
        prev_p2_score = p2.score;
        changed = true;
    }
    return changed;
}

static void draw_playing_frame(void) {
    if (field_needs_redraw) draw_field_static();

    // p1/p2/ball ahora guardan posición sub-píxel en float (para que la
    // física con dt variable no pierda precisión); se redondea a entero
    // solo aquí, justo para dibujar, y se compara en enteros para no
    // redibujar cada frame por ruido de sub-píxel sin cambio visible.
    int p1_draw_y   = (int)(p1.y + 0.5f);
    int p2_draw_y   = (int)(p2.y + 0.5f);
    int ball_draw_x = (int)(ball.x + 0.5f);
    int ball_draw_y = (int)(ball.y + 0.5f);

    if (prev_p1_y != p1_draw_y || prev_paddle_h != paddle_h) {
        if (prev_p1_y >= 0)
            renderer_fill_rect(p1.x, prev_p1_y, PADDLE_W,
                                (prev_paddle_h > 0) ? prev_paddle_h : paddle_h, COLOR_BLACK);
        renderer_fill_rect(p1.x, p1_draw_y, PADDLE_W, paddle_h, COLOR_P1);
        prev_p1_y = p1_draw_y;
        prev_paddle_h = paddle_h;
        renderer_flush();
    }
    if (prev_p2_y != p2_draw_y) {
        if (prev_p2_y >= 0)
            renderer_fill_rect(p2.x, prev_p2_y, PADDLE_W, PADDLE_H, COLOR_BLACK);
        renderer_fill_rect(p2.x, p2_draw_y, PADDLE_W, PADDLE_H, COLOR_P2);
        prev_p2_y = p2_draw_y;
        renderer_flush();
    }
    if (prev_ball_x != ball_draw_x || prev_ball_y != ball_draw_y) {
        if (prev_ball_x >= 0)
            renderer_fill_rect(prev_ball_x, prev_ball_y, BALL_SZ, BALL_SZ, COLOR_BLACK);
        renderer_fill_rect(ball_draw_x, ball_draw_y, BALL_SZ, BALL_SZ, COLOR_WHITE);
        prev_ball_x = ball_draw_x;
        prev_ball_y = ball_draw_y;
        renderer_flush();
    }

    bool ui_changed = draw_score_if_changed();

    bool bon = (blink / BLINK_HALF) % 2 == 0;
    bool levelup_active = (levelup_timer > 0);

    if (levelup_active != prev_levelup_shown) {
        renderer_fill_rect(CX-70, CY-30, 140, 44, COLOR_BLACK);
        if (levelup_active) {
            renderer_draw_text(centered_x("LEVEL UP!", 2), CY-24, "LEVEL UP!", COLOR_YELLOW, COLOR_BLACK, 2);
            char lv_msg[16];
            snprintf(lv_msg, sizeof(lv_msg), "NIVEL %d", player_level);
            renderer_draw_text(centered_x(lv_msg, 1), CY-2, lv_msg, COLOR_WHITE, COLOR_BLACK, 1);
        }
        prev_levelup_shown = levelup_active;
        ui_changed = true;
    }

    const char *target_msg = "";
    if (state == S_SERVE && !demo && bon)      target_msg = "PULSA PARA SACAR";
    else if (demo && bon)                       target_msg = "DEMO - PULSA PARA JUGAR";

    if (strcmp(target_msg, prev_bottom_msg) != 0) {
        renderer_fill_rect(0, PLAY_Y+PLAY_H-24, TFT_WIDTH, 18, COLOR_BLACK);
        if (target_msg[0]) {
            renderer_draw_text(centered_x(target_msg, 2), PLAY_Y+PLAY_H-22,
                                target_msg, COLOR_WHITE, COLOR_BLACK, 2);
        }
        strncpy(prev_bottom_msg, target_msg, sizeof(prev_bottom_msg) - 1);
        prev_bottom_msg[sizeof(prev_bottom_msg) - 1] = '\0';
        ui_changed = true;
    }

    if (ui_changed) renderer_flush();
}

static void draw_select_screen(void) {
    renderer_clear(COLOR_BLACK);
    renderer_draw_text(centered_x("PONG", 3), CY-60, "PONG", COLOR_CYAN, COLOR_BLACK, 3);
    renderer_draw_text(centered_x(two_p ? "- 2 JUGADORES -" : "  2 JUGADORES  ", 2),
                        CY-10, two_p ? "- 2 JUGADORES -" : "  2 JUGADORES  ", COLOR_WHITE, COLOR_BLACK, 2);
    renderer_draw_text(centered_x(!two_p ? "- 1 JUGADOR   -" : "  1 JUGADOR    ", 2),
                        CY+16, !two_p ? "- 1 JUGADOR   -" : "  1 JUGADOR    ", COLOR_WHITE, COLOR_BLACK, 2);
    renderer_draw_text(centered_x("MUEVE PARA CAMBIAR - PULSA PARA JUGAR", 1),
                        CY+60, "MUEVE PARA CAMBIAR - PULSA PARA JUGAR", COLOR_WHITE, COLOR_BLACK, 1);
    renderer_flush();
    init_menu_anim();
}

static void draw_over_screen(void) {
    renderer_clear(COLOR_BLACK);
    const char *msg;
    if (two_p)
        msg = p1.score >= SCORE_WIN ? "JUGADOR 1 GANA" : "JUGADOR 2 GANA";
    else
        msg = "FIN";
    renderer_draw_text(centered_x(msg, 2), CY-30, msg, COLOR_YELLOW, COLOR_BLACK, 2);

    char sc[20];
    if (two_p) snprintf(sc, sizeof(sc), "%d - %d", p1.score, p2.score);
    else       snprintf(sc, sizeof(sc), "TUS PUNTOS: %d", p1.score);
    renderer_draw_text(centered_x(sc, 2), CY+4, sc, COLOR_WHITE, COLOR_BLACK, 2);

    if (!demo)
        renderer_draw_text(centered_x("PULSA PARA CONTINUAR", 1), CY+40,
                            "PULSA PARA CONTINUAR", COLOR_WHITE, COLOR_BLACK, 1);
    renderer_flush();
}

static void draw_scores_screen(void) {
    renderer_clear(COLOR_BLACK);
    highscores_draw(PONG_GAME_ID, "PONG", 20);
    renderer_flush();
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------
static void pong_tick(float dt) {
    blink++;

    if (demo) {
        bool any = controls_menu_select()
                || controls_get_raw_delta(0) != 0
                || controls_get_raw_delta(1) != 0;
        if (any || ++demo_ticks >= DEMO_TIMEOUT_TICKS) {
            g_done = true;
            return;
        }
    }

    switch (state) {

    case S_SELECT:

        menu_anim_tick(dt);

        int d = controls_get_raw_delta(0);
         if (d) {
            two_p = !two_p;
            menu_enc_acc += d;
            if (menu_enc_acc >= 2)  { two_p = !two_p; menu_enc_acc = 0; draw_select_screen(); }
            if (menu_enc_acc <= -2) { two_p = !two_p; menu_enc_acc = 0; draw_select_screen(); }
         }

        if (controls_menu_select()) {
            p1.score = p2.score = 0;
            player_level = 0;
            paddle_h = PADDLE_H_LV0;
            levelup_timer = 0;
            serve_side = 0;
            ai_profile_init();
            reset_pads(); ball_centre();
            field_needs_redraw = true;
            sound_stop_menu_music();
            state = S_SERVE;
        }
        break;

    case S_SERVE:
        pad_move(&p1, enc_momentum(controls_get_raw_delta(0), &p1.vel, dt) * dt);
        if (two_p)
            pad_move(&p2, enc_momentum(controls_get_raw_delta(1), &p2.vel, dt) * dt);
        if (demo || controls_menu_select() || (!two_p && serve_side==1)) {
            ball_launch(); state = S_PLAYING;
        }
        draw_playing_frame();
        break;

    case S_PLAYING:
        if (demo) {
            ai_move(&p1, 0, DEMO_AI_SPEED, dt);
            ai_move(&p2, 0, DEMO_AI_SPEED, dt);
        } else {
            pad_move(&p1, enc_momentum(controls_get_raw_delta(0), &p1.vel, dt) * dt);
            if (two_p)
                pad_move(&p2, enc_momentum(controls_get_raw_delta(1), &p2.vel, dt) * dt);
            else
                ai_move_adaptive(&p2, dt);
        }

        ball.x += ball.bx * dt;
        ball.y += ball.by * dt;

        if (ball.y <= PLAY_Y)                { ball.y = PLAY_Y;              ball.by = -ball.by; ai_prof.target_valid = false; sound_effect_move(); }
        if (ball.y+BALL_SZ >= PLAY_Y+PLAY_H) { ball.y = PLAY_Y+PLAY_H-BALL_SZ; ball.by = -ball.by; ai_prof.target_valid = false; sound_effect_move(); }

        if (ball.bx < 0 && hits(&p1)) { bounce(&p1, true);  ai_prof.target_valid = false; }
        if (ball.bx > 0 && hits(&p2)) { bounce(&p2, false); ai_prof.target_valid = false; }

        if (ball.x + BALL_SZ < PLAY_X) {
            p2.score++;
            sound_effect_explosion();
            if (!two_p && !demo) ai_profile_update(false);
            serve_side = 1; pause_cnt = 0; state = S_PAUSE;
        } else if (ball.x > PLAY_X + PLAY_W) {
            p1.score++;
            sound_effect_explosion();
            if (!two_p && !demo) ai_profile_update(true);
            serve_side = 0; pause_cnt = 0; state = S_PAUSE;
        }

        draw_playing_frame();
        break;

    case S_PAUSE:
        if (levelup_timer > 0) levelup_timer--;
        draw_playing_frame();

        if (++pause_cnt >= PAUSE_TICKS) {
            if (!two_p) {
                int new_level = p1.score / 15;
                if (new_level > player_level) {
                    player_level = new_level;
                    paddle_h = player_paddle_h();
                    levelup_timer = LEVELUP_TICKS;
                    sound_effect_success();
                }
            }
            if (( two_p && (p1.score >= SCORE_WIN  || p2.score >= SCORE_WIN)) ||
                (!two_p && p2.score >= SCORE_AI_WIN)) {
                sound_stop_menu_music();
                if (two_p) {
                    sound_effect_success();
                } else {
                    sound_effect_game_over();
                }
                if (!demo && !two_p && highscores_is_top(PONG_GAME_ID, p1.score)) {
                    highscores_enter(PONG_GAME_ID, (uint32_t)p1.score);
                }
                pause_cnt = 0; state = S_OVER;
                draw_over_screen();
            } else {
                reset_pads(); ball_centre();
                state = S_SERVE;
                field_needs_redraw = true;
            }
        }
        break;

    case S_OVER:
        if (demo) {
            if (++pause_cnt >= PAUSE_TICKS*3) g_done = true;
        } else {
            pause_cnt++;
            if (pause_cnt >= PAUSE_TICKS * 3) {
                if (controls_menu_select()) {
                    pause_cnt = 0; state = S_SCORES;
                    draw_scores_screen();
                } else if (pause_cnt >= PAUSE_TICKS * 10) {
                    pause_cnt = 0; state = S_SCORES;
                    draw_scores_screen();
                }
            }
        }
        break;

    case S_SCORES:
        pause_cnt++;
        if (controls_menu_select()) {
            g_done = true;
        }
        if (pause_cnt >= PAUSE_TICKS * 6) g_done = true;
        break;
    }
}

// ---------------------------------------------------------------------------
// API pública
// ---------------------------------------------------------------------------
void game_pong_run(game_mode_t mode) {
    srand((unsigned int)esp_timer_get_time());

    p1.score = p2.score = 0;
    player_level = 0; paddle_h = PADDLE_H_LV0; levelup_timer = 0;
    two_p = (mode == GAME_MODE_2P);
    demo = (mode == GAME_MODE_DEMO);
    demo_ticks = 0;
    serve_side = 0;
    blink = 0;
    g_done = false;
    field_needs_redraw = true;

    ai_profile_init();
    reset_pads();
    ball_centre();

    if (demo) {
        ball_launch();
        state = S_PLAYING;
    } else {
        state = S_SELECT;
        draw_select_screen();
        sound_start_menu_music();
    }

    // Periodo objetivo del bucle. OJO: vTaskDelay(pdMS_TO_TICKS(8)) es lo
    // que causaba el bug original -- con el tick rate de FreeRTOS por
    // defecto en ESP-IDF (100 Hz = 10 ms/tick), pdMS_TO_TICKS(8) se
    // redondea a 0 (división entera: 8*100/1000 = 0), así que
    // vTaskDelay(0) apenas esperaba nada real y el bucle corría a la
    // velocidad máxima que le dejaba el scheduler, de forma irregular
    // según qué otras tareas de FreeRTOS estuvieran listas en cada
    // momento -- de ahí "muy rápido y no uniforme".
    //
    // Con 1000/TARGET_FPS = ~16 ms el redondeo ya no da 0 aunque el tick
    // rate siga en 100 Hz (aunque para un ritmo más fino conviene subir
    // CONFIG_FREERTOS_HZ a 1000 en menuconfig). Y como además la física
    // de arriba usa el dt REAL medido con esp_timer_get_time() (no un
    // valor fijo asumido), el juego se mueve a la velocidad correcta
    // incluso si algún frame se retrasa por el propio jitter del
    // scheduler o por un flush SPI lento.
    const TickType_t period_ticks = pdMS_TO_TICKS(1000 / TARGET_FPS);
    TickType_t last_wake = xTaskGetTickCount();
    int64_t last_time_us = esp_timer_get_time();

    while (!g_done) {
        controls_update();

        int64_t now_us = esp_timer_get_time();
        float dt = (float)(now_us - last_time_us) / 1000000.0f;
        last_time_us = now_us;
        if (dt > MAX_DT) dt = MAX_DT;             // evita saltos tras una pausa larga
        if (dt <= 0.0f) dt = 1.0f / TARGET_FPS;    // por si el timer no ha avanzado

        pong_tick(dt);
        sound_update();

        vTaskDelayUntil(&last_wake, period_ticks ? period_ticks : 1);
    }

    highscores_flush();
}