/**
 * breakout.c -- portado de ArcadePi (https://github.com/JuanLPerea/ArcadePi),
 * mismo concepto (10 layouts de nivel, 5 power-ups, warp, imán,
 * disparo), adaptado a ArcadeColor / ESP32.
 *
 * PORT A ESP32 -- FISICA REESCRITA A TIEMPO REAL (mismo patron que
 * pong.c): la version original (Pico) movia la bola con un
 * acumulador de subpixel fijo (/64) y la pala con un entero de
 * velocidad, ambos avanzando una cantidad fija POR TICK del bucle
 * (sleep_ms(8)). En ESP32 eso se rompia igual que en Pong: con el
 * tick rate de FreeRTOS a 100Hz por defecto, pdMS_TO_TICKS(8) se
 * redondeaba a 0 y el juego iba a trompicones.
 *
 * Ahora la velocidad de la bola y de la pala estan en PIXELES POR
 * SEGUNDO (px/s), como floats, y game_breakout_run() mide el dt real
 * entre frames con esp_timer_get_time() (igual que pong.c). El resto
 * de temporizadores (pause_cnt, blink, cooldowns, power-ups,
 * balas...) se quedan como contadores POR FRAME, exactamente igual
 * que en pong.c -- son validos porque el bucle principal ahora se
 * marca a un ritmo objetivo estable con vTaskDelayUntil(), no hace
 * falta convertir TODO a tiempo real, solo lo que de verdad se nota
 * (bola y pala).
 *
 * Resto de adaptaciones (iguales que pong.c/space_invaders.c):
 *  - Resolucion: campo a pantalla casi completa (320x240 apaisada).
 *  - Texturas simplificadas: color solido por tipo de ladrillo.
 *  - Controles: controls_get_raw_delta_x() (eje X del joystick,
 *    movimiento horizontal) para la pala,
 *    controls_menu_select() para lanzar/disparar/soltar iman.
 *  - Render incremental: igual que el original.
 *  - Sonido: mapeado a sound_effect_move/shoot/explosion/select/
 *    lose_point, igual que en pong.c.
 *
 * MODO DE CONTROL DE LA PALA (seleccionable en la pantalla de titulo,
 * arriba/abajo del stick para alternar, igual que el cursor de
 * tetris.c/asteroids.c):
 *  - INERCIA (por defecto, comportamiento original): enc_momentum()
 *    acumula una velocidad con aceleracion/decaimiento a partir de
 *    controls_get_raw_delta_x() -- hay que soltar el stick para que
 *    la pala frene, como con un encoder "virtual".
 *  - DIRECTO (nuevo): pad_update_direct() mapea la posicion absoluta
 *    del eje X de J1 (controls_debug_axis_normalized(), -1000..1000)
 *    directamente a una posicion de la pala en el campo: stick
 *    centrado = pala centrada, stick a fondo = pala en el extremo.
 *    Sin inercia ni velocidad acumulada.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "breakout.h"
#include "../renderer.h"
#include "../controls.h"
#include "../highscores.h"
#include "../sound.h"

// ---------------------------------------------------------------------------
// Área de juego
// ---------------------------------------------------------------------------
#define SCREEN_W 320
#define SCREEN_H 240

#define PLAY_X   4
#define PLAY_Y   3
#define PLAY_W   (SCREEN_W - 2 * PLAY_X)   // 312
#define PLAY_H   (SCREEN_H - 2 * PLAY_Y)   // 234
#define CX       (PLAY_X + PLAY_W / 2)
#define CY       (PLAY_Y + PLAY_H / 2)

#define TICKS_S  60   // referencia nominal para pausas simples (contadores por frame)

// Ritmo objetivo del bucle y paso de simulacion fijo (igual que pong.c).
#define TARGET_FPS 60
#define SIM_DT     (1.0f / TARGET_FPS)
#define MAX_DT     0.05f

// ---------------------------------------------------------------------------
// Pala -- ahora en px/s (antes enteros "por tick")
// ---------------------------------------------------------------------------
#define PAD_H         6
#define PAD_Y        (PLAY_Y + PLAY_H - 16)
#define PAD_W_BASE   44
#define PAD_W_WIDE   70   // power-up WIDE
#define PAD_W_MIN    24
#define PAD_W_SHRINK  3

// Mas suave y lenta que la pala de pong.c a proposito: Breakout
// necesita ajustes finos de posicion (golpear la bola con un punto
// concreto de la pala), asi que responde con menos brusquedad al
// stick que un Pong tipico.
#define PAD_ACCEL          100.0f  // px/s^2 por "unidad" de inclinacion de joystick
#define PAD_VEL_MAX        8.0f  // px/s, tope de velocidad de la pala
#define PAD_DECAY_PER_SEC  20.0f    // decaimiento exponencial al soltar el stick
#define PAD_AI_SPEED_PPS   100.0f  // demo: velocidad de seguimiento de la IA, en px/s

static float pad_xf;   // posicion real (sub-pixel) de la pala
static float pad_vel;  // velocidad real de la pala, px/s
static int   pad_x, pad_w; // posicion/anchura ENTERA usada para dibujar y colisiones

// clampf/clamp se definen mas abajo (junto al resto de helpers), pero
// se necesitan aqui arriba para pad_update()/pad_update_direct().
static float clampf(float v, float lo, float hi);

// ---------------------------------------------------------------------------
// Modo de control de la pala -- seleccionable desde la pantalla de titulo.
//
//  BRK_CTRL_INERTIA (por defecto, comportamiento original): el stick
//  empuja una velocidad con aceleracion/decaimiento exponencial
//  (enc_momentum), igual que un encoder "virtual" -- hay que soltar
//  el stick para que la pala frene.
//
//  BRK_CTRL_DIRECT: la pala responde directamente a la posicion del
//  stick. Stick centrado = pala centrada; stick a fondo a un lado =
//  pala en el extremo correspondiente del campo. No hay inercia ni
//  velocidad acumulada, es un mapeo de posicion 1:1 por frame.
// ---------------------------------------------------------------------------
typedef enum { BRK_CTRL_INERTIA = 0, BRK_CTRL_DIRECT = 1, BRK_CTRL_COUNT } brk_ctrl_mode_t;
static brk_ctrl_mode_t ctrl_mode = BRK_CTRL_INERTIA;

static const char * const ctrl_mode_names[BRK_CTRL_COUNT] = { "INERCIA", "DIRECTO" };

// Zona muerta central para el modo directo, sobre el rango normalizado
// -1000..1000 de controls_debug_axis_normalized(). Sin esto, el ruido
// residual cerca del centro (tras el filtro EMA de controls.c) haria
// temblar la pala en reposo.
#define PAD_DIRECT_DEADZONE 40

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

// Mapea la posicion del eje X de J1 (normalizada, -1000..1000, 0=centro)
// directamente a una posicion de la pala: 0 -> pala centrada en el
// campo, +-1000 -> pala pegada al extremo correspondiente. pad_vel se
// mantiene a 0 porque este modo no usa inercia (si se cambia de modo
// a mitad de partida, el modo INERCIA debe arrancar sin velocidad
// heredada).
static void pad_update_direct(void) {
    int norm = controls_debug_axis_normalized(0); // eje X de J1

    if (norm > -PAD_DIRECT_DEADZONE && norm < PAD_DIRECT_DEADZONE) {
        norm = 0;
    } else if (norm > 0) {
        norm -= PAD_DIRECT_DEADZONE;
    } else {
        norm += PAD_DIRECT_DEADZONE;
    }
    // Reescala para que, tras quitar la zona muerta, se pueda seguir
    // alcanzando +-1000 (el extremo del campo) sin necesitar mas
    // inclinacion de la que ya haria falta sin zona muerta.
    float scale = 1000.0f / (float)(1000 - PAD_DIRECT_DEADZONE);
    float t = clampf((float)norm * scale / 1000.0f, -1.0f, 1.0f); // -1..1

    float half_range = (float)(PLAY_W - pad_w) / 2.0f;
    float center = (float)(CX) - (float)pad_w / 2.0f;

    pad_xf  = clampf(center + t * half_range, (float)PLAY_X, (float)(PLAY_X + PLAY_W - pad_w));
    pad_vel = 0.0f;
    pad_x   = (int)(pad_xf + 0.5f);
}

// Punto unico de entrada para mover la pala segun el modo activo --
// llamado desde BRK_SERVE y BRK_PLAYING (solo cuando !demo; la IA de
// la demo sigue usando demo_ai() en los dos modos).
static void pad_update(float dt) {
    if (ctrl_mode == BRK_CTRL_DIRECT) {
        pad_update_direct();
    } else {
        int d = controls_get_raw_delta_x(0);
        pad_xf = clampf(pad_xf + enc_momentum(d, &pad_vel, dt), (float)PLAY_X, (float)(PLAY_X + PLAY_W - pad_w));
        pad_x = (int)(pad_xf + 0.5f);
    }
}

#define WARP_GAP_H   40
#define WARP_GAP_Y   (PAD_Y - (WARP_GAP_H - PAD_H)/2)

// ---------------------------------------------------------------------------
// Bola -- ahora en px/s (antes subpixel fijo /64 por tick)
// ---------------------------------------------------------------------------
#define BALL_SZ         6
#define BALL_SPD0_PPS      190.0f
#define BALL_SPD_INC_PPS    16.0f
#define BALL_SPD_MAX_PPS   540.0f
#define BRICKS_PER_ACCEL 3

// ---------------------------------------------------------------------------
// Ladrillos
// ---------------------------------------------------------------------------
#define BRICK_COLS   10
#define BRICK_ROWS    7
#define BRICK_W      30
#define BRICK_H      10
#define BRICK_GAP     1
#define BRICK_X0     (PLAY_X + 6)
#define BRICK_Y0     (PLAY_Y + 20)

// ---------------------------------------------------------------------------
// Power-ups
// ---------------------------------------------------------------------------
#define PU_NONE    0
#define PU_SHOOT   1
#define PU_WARP    2
#define PU_WIDE    3
#define PU_MAGNET  4
#define PU_LIFE    5

#define PU_COUNT      5
#define MAX_POWERUPS  4
#define PU_W         22
#define PU_H         14
#define PU_SPEED      1   // px/frame (contador por frame, igual que pause_cnt/blink)

#define PU_DURATION    (TICKS_S * 15)
#define SHOOT_COOLDOWN 20
#define MAX_BULLETS     3

typedef struct { int x, y; bool active; uint8_t type; } PowerUp;
typedef struct { int x, y; bool active; } Bullet;

static const char * const pu_labels[6] = { "?", "F", "W", "L", "M", "U" };

// Color de cada tipo de power-up. Se eligio para que sea el MISMO que el de su
// efecto en pantalla, asi la capsula "avisa" de lo que va a pasar:
//   F  DISPARO       amarillo   (las balas son amarillas)
//   W  WARP          verde      (el hueco que se abre en el lateral)
//   L  PALA LARGA    cian       (la pala ancha se pinta cian)
//   M  IMAN          magenta    (la bola con iman se pinta magenta)
//   U  VIDA EXTRA    rojo
static uint16_t pu_color(uint8_t type) {
    switch (type) {
        case PU_SHOOT:  return COLOR_YELLOW;
        case PU_WARP:   return COLOR_GREEN;
        case PU_WIDE:   return COLOR_CYAN;
        case PU_MAGNET: return COLOR_MAGENTA;
        case PU_LIFE:   return COLOR_RED;
        default:        return COLOR_BLUE;
    }
}
// Letra negra sobre los colores claros, blanca sobre los oscuros.
static uint16_t pu_text_color(uint8_t type) {
    return (type == PU_MAGNET || type == PU_LIFE) ? COLOR_WHITE : COLOR_BLACK;
}

// ---------------------------------------------------------------------------
// Tipos de ladrillo -- color solido por tipo
// ---------------------------------------------------------------------------
#define BRICK_INDESTRUCTIBLE 9

static const int BRICK_PTS_BY_TYPE[10] = { 0, 4, 3, 3, 2, 2, 2, 1, 1, 0 };
static const uint16_t BRICK_COLOR_BY_TYPE[10] = {
    COLOR_BLACK, COLOR_RED, COLOR_YELLOW, COLOR_YELLOW, COLOR_GREEN,
    COLOR_GREEN, COLOR_CYAN, COLOR_CYAN, COLOR_MAGENTA, COLOR_WHITE,
};

#define PU_SPAWN_RATE 5

// ---------------------------------------------------------------------------
// Layouts de nivel -- identicos al original, no dependen de resolucion
// ---------------------------------------------------------------------------
#define NL 10

static const uint8_t layout_classic[BRICK_ROWS][BRICK_COLS] = {
    {1,1,1,1,1,1,1,1,1,1},
    {2,2,2,2,2,2,2,2,2,2},
    {3,3,3,3,3,3,3,3,3,3},
    {4,4,4,4,4,4,4,4,4,4},
    {5,5,5,5,5,5,5,5,5,5},
    {6,6,6,6,6,6,6,6,6,6},
    {7,7,7,7,7,7,7,7,7,7},
};
static const uint8_t layout_bars[BRICK_ROWS][BRICK_COLS] = {
    {2,0,2,0,2,0,2,0,2,0},
    {2,0,2,0,2,0,2,0,2,0},
    {3,0,3,0,3,0,3,0,3,0},
    {3,0,3,0,3,0,3,0,3,0},
    {1,0,1,0,1,0,1,0,1,0},
    {1,0,1,0,1,0,1,0,1,0},
    {9,0,9,0,9,0,9,0,9,0},
};
static const uint8_t layout_diamond[BRICK_ROWS][BRICK_COLS] = {
    {0,0,0,0,4,4,0,0,0,0},
    {0,0,0,4,4,4,4,0,0,0},
    {0,0,4,4,4,4,4,4,0,0},
    {0,4,4,4,4,4,4,4,4,0},
    {0,0,4,4,4,4,4,4,0,0},
    {0,0,0,4,4,4,4,0,0,0},
    {0,0,0,0,4,4,0,0,0,0},
};
static const uint8_t layout_alien[BRICK_ROWS][BRICK_COLS] = {
    {0,0,1,0,0,0,0,1,0,0},
    {0,0,0,1,0,0,1,0,0,0},
    {0,0,1,1,1,1,1,1,0,0},
    {0,1,1,0,1,1,0,1,1,0},
    {1,1,1,1,1,1,1,1,1,1},
    {1,0,1,1,1,1,1,1,0,1},
    {0,0,0,1,1,1,1,0,0,0},
};
static const uint8_t layout_smile[BRICK_ROWS][BRICK_COLS] = {
    {0,1,1,1,1,1,1,1,1,0},
    {1,1,0,0,0,0,0,0,1,1},
    {1,0,2,0,0,0,0,2,0,1},
    {1,0,0,0,0,0,0,0,0,1},
    {1,0,3,0,0,0,0,3,0,1},
    {1,1,0,3,3,3,3,0,1,1},
    {0,1,1,1,1,1,1,1,1,0},
};
static const uint8_t layout_heart[BRICK_ROWS][BRICK_COLS] = {
    {0,1,1,0,0,0,0,1,1,0},
    {1,1,1,1,0,0,1,1,1,1},
    {1,1,1,1,1,1,1,1,1,1},
    {1,1,1,1,1,1,1,1,1,1},
    {0,1,1,1,1,1,1,1,1,0},
    {0,0,1,1,1,1,1,1,0,0},
    {0,0,0,0,1,1,0,0,0,0},
};
static const uint8_t layout_xshape[BRICK_ROWS][BRICK_COLS] = {
    {3,3,0,0,0,0,0,0,3,3},
    {0,3,3,0,0,0,0,3,3,0},
    {0,0,3,3,0,0,3,3,0,0},
    {0,0,0,3,3,3,3,0,0,0},
    {0,0,3,3,0,0,3,3,0,0},
    {0,3,3,0,0,0,0,3,3,0},
    {3,3,0,0,0,0,0,0,3,3},
};
static const uint8_t layout_triangle[BRICK_ROWS][BRICK_COLS] = {
    {0,0,0,0,6,6,0,0,0,0},
    {0,0,0,5,5,5,5,0,0,0},
    {0,0,4,4,4,4,4,4,0,0},
    {0,3,3,3,3,3,3,3,3,0},
    {2,2,2,2,2,2,2,2,2,2},
    {1,1,1,1,1,1,1,1,1,1},
    {9,9,9,9,9,9,9,9,9,9},
};
static const uint8_t layout_zigzag[BRICK_ROWS][BRICK_COLS] = {
    {5,5,0,0,0,0,0,0,0,0},
    {0,5,5,0,0,0,0,0,0,0},
    {0,0,5,5,0,0,0,0,0,0},
    {0,0,0,5,5,0,0,0,0,0},
    {0,0,0,0,5,5,0,0,0,0},
    {0,0,0,0,0,5,5,0,0,0},
    {0,0,0,0,0,0,5,5,5,5},
};
static const uint8_t layout_chess[BRICK_ROWS][BRICK_COLS] = {
    {2,0,2,0,2,0,2,0,2,0},
    {0,5,0,5,0,5,0,5,0,5},
    {2,0,2,0,2,0,2,0,2,0},
    {0,5,0,5,0,5,0,5,0,5},
    {2,0,2,0,2,0,2,0,2,0},
    {0,5,0,5,0,5,0,5,0,5},
    {2,0,2,0,2,0,2,0,2,0},
};

static const uint8_t * const all_layouts[NL] = {
    &layout_classic [0][0], &layout_bars    [0][0], &layout_diamond [0][0],
    &layout_alien   [0][0], &layout_smile   [0][0], &layout_heart   [0][0],
    &layout_xshape  [0][0], &layout_triangle[0][0], &layout_zigzag  [0][0],
    &layout_chess   [0][0],
};
static const char *layout_names[NL] = {
  "BARRAS", "DIAMANTE", "ALIEN", "SONRISA",
    "CORAZON", "ASPA", "TRIANGULO", "ZIGZAG", "AJEDREZ", "CLASICO"
};

// ---------------------------------------------------------------------------
// Estado
// ---------------------------------------------------------------------------
typedef enum {
    BRK_TITLE, BRK_SERVE, BRK_PLAYING, BRK_DEAD,
    BRK_LEVELUP, BRK_OVER, BRK_SCORES,
} BrkState;

static uint8_t bricks[BRICK_ROWS][BRICK_COLS];
static int     bricks_left;

// Bola: posicion y velocidad en float (px, px/s). ball_x/ball_y (int)
// se derivan cada frame para dibujo/colisiones.
static float ball_xf, ball_yf, ball_bxf, ball_byf;
static int   ball_x, ball_y;
static bool ball_held, ball_magnet;

static int      lives, level, score, bricks_broken;
static float    ball_spd; // px/s
static BrkState brk_state;
static int      pause_cnt, blink;
static bool     demo;
static int      demo_ticks;
static int      snd_cooldown;
static bool     g_done;

static PowerUp powerups[MAX_POWERUPS];
static uint8_t active_pu;
static int     active_pu_timer;
static Bullet  bullets[MAX_BULLETS];
static int     shoot_cooldown;
static bool    warp_spawned;
static int     warp_side;

static uint32_t rng_state_v = 12345;
static uint32_t rng_next(void) {
    rng_state_v ^= rng_state_v << 13;
    rng_state_v ^= rng_state_v >> 17;
    rng_state_v ^= rng_state_v << 5;
    return rng_state_v;
}

static int clamp(int v, int lo, int hi) { return v<lo?lo:v>hi?hi:v; }
static float clampf(float v, float lo, float hi) { return v<lo?lo:v>hi?hi:v; }
static int iabs_brk(int v) { return v<0?-v:v; }

static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static int  prev_ball_x=-1, prev_ball_y=-1;
static int  prev_pad_x=-1, prev_pad_w=-1;
static int  prev_bullet_x[MAX_BULLETS], prev_bullet_y[MAX_BULLETS];
static bool prev_bullet_active[MAX_BULLETS];
static int  prev_pu_x[MAX_POWERUPS], prev_pu_y[MAX_POWERUPS];
static bool prev_pu_active[MAX_POWERUPS];

static int last_ind_r = -1;
static int last_ind_c = -1;
static int ind_cooldown = 0;

// ---------------------------------------------------------------------------
// Helpers de estado
// ---------------------------------------------------------------------------
static void brick_rect(int r, int c, int *bx, int *by, int *bw, int *bh) {
    *bx = BRICK_X0 + c * BRICK_W;
    *by = BRICK_Y0 + r * BRICK_H;
    *bw = BRICK_W - BRICK_GAP;
    *bh = BRICK_H - BRICK_GAP;
}

static void init_bricks(void) {
    int layout_idx = (level - 1) % NL;
    const uint8_t *lay = all_layouts[layout_idx];
    bricks_left = 0;
    for (int r=0;r<BRICK_ROWS;r++)
        for (int c=0;c<BRICK_COLS;c++) {
            bricks[r][c] = lay[r * BRICK_COLS + c];
            if (bricks[r][c] != 0 && bricks[r][c] != BRICK_INDESTRUCTIBLE) bricks_left++;
        }
}

static void powerups_clear(void) { for (int i=0;i<MAX_POWERUPS;i++) powerups[i].active = false; }

static void bullets_clear(void) {
    for (int i=0;i<MAX_BULLETS;i++) bullets[i].active = false;
    shoot_cooldown = 0;
}

static float ball_speed_for_level(void) {
    float spd = BALL_SPD0_PPS + (level - 1) * BALL_SPD_INC_PPS * 2.0f;
    if (spd > BALL_SPD_MAX_PPS) spd = BALL_SPD_MAX_PPS;
    return spd;
}

static void deactivate_powerup(void) {
    if (active_pu == PU_WIDE) pad_w = clamp(PAD_W_BASE - (level-1)*PAD_W_SHRINK, PAD_W_MIN, PAD_W_BASE);
    if (active_pu == PU_MAGNET) {
        ball_magnet = false;
        if (ball_held) {
            ball_held = false;
            float sign = (rng_next() & 1) ? 1.0f : -1.0f;
            ball_bxf = ball_spd * 0.5f * sign;
            ball_byf = -ball_spd;
        }
    }
    if (active_pu == PU_WARP) {
        int wx = warp_side ? PLAY_X+PLAY_W-3 : PLAY_X;
        renderer_fill_rect(wx, WARP_GAP_Y, 3, WARP_GAP_H, COLOR_BLACK);
        renderer_flush();
    }
    active_pu = PU_NONE;
    active_pu_timer = 0;
}

static void activate_powerup(uint8_t type) {
    deactivate_powerup();
    active_pu = type;
    active_pu_timer = PU_DURATION;
    switch (type) {
        case PU_WIDE:   pad_w = PAD_W_WIDE; break;
        case PU_MAGNET: ball_magnet = false; break;
        case PU_WARP: {
            warp_side = rng_next() & 1;
            int wx = warp_side ? PLAY_X+PLAY_W-3 : PLAY_X;
            renderer_fill_rect(wx, WARP_GAP_Y, 3, WARP_GAP_H, pu_color(PU_WARP));
            renderer_flush();
            break;
        }
        case PU_LIFE:
            lives++;
            active_pu = PU_NONE;
            active_pu_timer = 0;
            break;
        default: break;
    }
}

static void powerup_spawn(int cx, int cy) {
    if ((rng_next() % PU_SPAWN_RATE) != 0) return;
    for (int i=0;i<MAX_POWERUPS;i++) {
        if (powerups[i].active) continue;
        powerups[i].active = true;
        powerups[i].x = cx - PU_W/2;
        powerups[i].y = cy;
        uint8_t type;
        do {
            type = (uint8_t)(1 + (rng_next() % PU_COUNT));
        } while (type == PU_WARP && warp_spawned);
        if (type == PU_WARP) warp_spawned = true;
        powerups[i].type = type;
        return;
    }
}

static void serve_reset(void) {
    ball_spd = ball_speed_for_level();

    pad_x = (int)(pad_xf + 0.5f);
    ball_xf = (float)(pad_x + pad_w/2 - BALL_SZ/2);
    ball_yf = (float)(PAD_Y - BALL_SZ - 2);
    float sign = (rng_next() & 1) ? 1.0f : -1.0f;
    ball_bxf = ball_spd * sign;
    ball_byf = -ball_spd;
    ball_held = true;
    ball_magnet = false;
    snd_cooldown = 0;
    bullets_clear();

    // OJO: aqui NO se ponen prev_ball_x / prev_pad_x a -1. Ese valor
    // significa "no hay nada dibujado, no borres", y no es cierto: al
    // perder una bola el campo NO se redibuja, asi que la pala y la bola
    // anteriores siguen en pantalla. Con -1, draw_paddle_if_moved() y
    // draw_ball_if_moved() no las borraban y quedaban como restos en cuanto
    // la pala se movia en BRK_SERVE (en modo DIRECTO salta al instante a
    // donde este el stick). Cuando si hay que empezar de cero
    // (level_start -> field_needs_redraw) ya lo hace draw_field_static().
}

static bool field_needs_redraw = true;

static void level_start(void) {
    init_bricks();
    pad_w  = clamp(PAD_W_BASE - (level-1)*PAD_W_SHRINK, PAD_W_MIN, PAD_W_BASE);
    pad_xf = (float)(CX - pad_w/2);
    pad_vel = 0.0f;
    ball_spd = ball_speed_for_level();
    bricks_broken = 0;
    powerups_clear();
    bullets_clear();
    deactivate_powerup();
    warp_spawned = (rng_next() % 4 != 0);
    serve_reset();
    brk_state = BRK_SERVE;
    pause_cnt = 0;
    field_needs_redraw = true;
}

static void game_start(void) {
    lives = 3; level = 1; score = 0;
    rng_state_v = (uint32_t)esp_timer_get_time();
    level_start();
}

// ---------------------------------------------------------------------------
// Render incremental
// ---------------------------------------------------------------------------
static int centered_x(const char *text, int scale) {
    int w = (int)st7789_text_width(text, (uint8_t)scale);
    int x = (TFT_WIDTH - w) / 2;
    return (x < 0) ? 0 : x;
}

static void draw_bricks_full(void) {
    for (int r=0;r<BRICK_ROWS;r++)
        for (int c=0;c<BRICK_COLS;c++) {
            if (bricks[r][c] == 0) continue;
            int bx,by,bw,bh;
            brick_rect(r,c,&bx,&by,&bw,&bh);
            renderer_fill_rect(bx,by,bw,bh, BRICK_COLOR_BY_TYPE[bricks[r][c]]);
        }
}

// Tras borrar con negro un rectangulo, repinta lo ESTATICO que hubiera
// debajo y que el borrado se ha llevado por delante: los bordes superior e
// inferior del campo (la bola, las balas y los powerups los cruzan) y los
// ladrillos que sigan vivos (los powerups caen atravesando la zona de
// ladrillos). Sin esto quedaban huecos en la linea blanca de abajo y
// ladrillos "borrados" en pantalla que seguian existiendo.
static void restore_static_in_rect(int x, int y, int w, int h) {
    int top = PLAY_Y, bot = PLAY_Y + PLAY_H - 1;
    int x0 = (x < PLAY_X) ? PLAY_X : x;
    int x1 = (x + w > PLAY_X + PLAY_W) ? PLAY_X + PLAY_W : x + w;
    if (x1 > x0) {
        if (y <= top && y + h > top) renderer_fill_rect(x0, top, x1 - x0, 1, COLOR_WHITE);
        if (y <= bot && y + h > bot) renderer_fill_rect(x0, bot, x1 - x0, 1, COLOR_WHITE);
    }
    if (active_pu == PU_WARP) {   // marca del hueco lateral del warp (la bola/pala la borran al cruzarla)
        int wx = warp_side ? PLAY_X+PLAY_W-3 : PLAY_X;
        if (wx < x + w && wx + 3 > x && WARP_GAP_Y < y + h && WARP_GAP_Y + WARP_GAP_H > y)
            renderer_fill_rect(wx, WARP_GAP_Y, 3, WARP_GAP_H, pu_color(PU_WARP));
    }
    if (y + h <= BRICK_Y0 || y >= BRICK_Y0 + BRICK_ROWS * BRICK_H) return;   // fuera de la zona de ladrillos
    for (int r = 0; r < BRICK_ROWS; r++)
        for (int c = 0; c < BRICK_COLS; c++) {
            if (bricks[r][c] == 0) continue;
            int bx, by, bw, bh;
            brick_rect(r, c, &bx, &by, &bw, &bh);
            if (bx >= x + w || bx + bw <= x || by >= y + h || by + bh <= y) continue;
            renderer_fill_rect(bx, by, bw, bh, BRICK_COLOR_BY_TYPE[bricks[r][c]]);
        }
}

static void draw_ball_if_moved(void) {
    if (ball_x == prev_ball_x && ball_y == prev_ball_y) return;
    if (prev_ball_x >= 0) {
        renderer_fill_rect(prev_ball_x, prev_ball_y, BALL_SZ, BALL_SZ, COLOR_BLACK);
        restore_static_in_rect(prev_ball_x, prev_ball_y, BALL_SZ, BALL_SZ);
    }
    renderer_fill_rect(ball_x, ball_y, BALL_SZ, BALL_SZ, ball_magnet ? COLOR_MAGENTA : COLOR_WHITE);
    prev_ball_x = ball_x; prev_ball_y = ball_y;
    renderer_flush();
}

static void draw_paddle_if_moved(void) {
    if (pad_x == prev_pad_x && pad_w == prev_pad_w) return;
    if (prev_pad_x >= 0) {
        renderer_fill_rect(prev_pad_x, PAD_Y, prev_pad_w, PAD_H, COLOR_BLACK);
        restore_static_in_rect(prev_pad_x, PAD_Y, prev_pad_w, PAD_H);
    }
    uint16_t color = (active_pu == PU_WIDE) ? pu_color(PU_WIDE) : COLOR_WHITE;
    renderer_fill_rect(pad_x, PAD_Y, pad_w, PAD_H, color);
    prev_pad_x = pad_x; prev_pad_w = pad_w;
    renderer_flush();
}

static void draw_field_static(void) {
    renderer_clear(COLOR_BLACK);
    renderer_fill_rect(PLAY_X, PLAY_Y,          PLAY_W, 1, COLOR_WHITE);
    renderer_fill_rect(PLAY_X, PLAY_Y+PLAY_H-1, PLAY_W, 1, COLOR_WHITE);
    draw_bricks_full();

    prev_ball_x = prev_ball_y = -1;
    prev_pad_x = prev_pad_w = -1;
    for (int i = 0; i < MAX_BULLETS; i++) prev_bullet_active[i] = false;
    for (int i = 0; i < MAX_POWERUPS; i++) prev_pu_active[i] = false;

    field_needs_redraw = false;
    renderer_flush();
}

static void draw_bullets_if_moved(void) {
    bool any=false;
    for (int i=0;i<MAX_BULLETS;i++) {
        bool show = bullets[i].active;
        if (!show && !prev_bullet_active[i]) continue;
        if (prev_bullet_active[i]) {
            renderer_fill_rect(prev_bullet_x[i], prev_bullet_y[i], 2, 6, COLOR_BLACK);
            restore_static_in_rect(prev_bullet_x[i], prev_bullet_y[i], 2, 6);
        }
        if (show) renderer_fill_rect(bullets[i].x, bullets[i].y, 2, 6, COLOR_YELLOW);
        prev_bullet_x[i]=bullets[i].x; prev_bullet_y[i]=bullets[i].y; prev_bullet_active[i]=show;
        any=true;
    }
    if (any) renderer_flush();
}

// Capsula de power-up: borde blanco + anillo negro + relleno del color del tipo +
// letra. El borde y el anillo la separan de los ladrillos por los que cae, que
// usan esa misma paleta (rojo, amarillo, verde, cian, magenta, blanco).
static void draw_powerup_capsule(int x, int y, uint8_t type) {
    uint16_t c = pu_color(type);
    renderer_fill_rect(x,   y,   PU_W,   PU_H,   COLOR_WHITE);
    renderer_fill_rect(x+1, y+1, PU_W-2, PU_H-2, COLOR_BLACK);
    renderer_fill_rect(x+2, y+2, PU_W-4, PU_H-4, c);
    renderer_draw_text(x+PU_W/2-3, y+PU_H/2-4, pu_labels[type], pu_text_color(type), c, 1);
}

static void draw_powerups_if_moved(void) {
    bool any=false;
    for (int i=0;i<MAX_POWERUPS;i++) {
        bool show = powerups[i].active;
        if (!show && !prev_pu_active[i]) continue;
        if (prev_pu_active[i]) {
            renderer_fill_rect(prev_pu_x[i], prev_pu_y[i], PU_W, PU_H, COLOR_BLACK);
            restore_static_in_rect(prev_pu_x[i], prev_pu_y[i], PU_W, PU_H);
        }
        if (show) draw_powerup_capsule(powerups[i].x, powerups[i].y, powerups[i].type);
        prev_pu_x[i]=powerups[i].x; prev_pu_y[i]=powerups[i].y; prev_pu_active[i]=show;
        any=true;
    }
    if (any) renderer_flush();
}

// ---------------------------------------------------------------------------
// HUD -- refresco forzado periodico via now_ms() en vez de
// absolute_time_t/time_reached (Pico SDK)
// ---------------------------------------------------------------------------
static int prev_score_hud=-1, prev_lives_hud=-1, prev_level_hud=-1;
static uint32_t next_hud_refresh_ms = 0;
static char prev_bottom_msg[40] = "";

static void draw_hud_if_changed(void) {
    char buf[16];
    bool changed=false;
    bool force = now_ms() >= next_hud_refresh_ms;
    if (force) next_hud_refresh_ms = now_ms() + 700;

    if (score != prev_score_hud || force) {
        renderer_fill_rect(PLAY_X+2, PLAY_Y+3, 90, 14, COLOR_BLACK);
        snprintf(buf, sizeof(buf), "%d", score);
        renderer_draw_text(PLAY_X+2, PLAY_Y+3, buf, COLOR_WHITE, COLOR_BLACK, 2);
        prev_score_hud = score; changed = true;
    }
    if (lives != prev_lives_hud || force) {
        renderer_fill_rect(PLAY_X+PLAY_W-46, PLAY_Y+3, 44, 14, COLOR_BLACK);
        snprintf(buf, sizeof(buf), "V:%d", lives);
        renderer_draw_text(PLAY_X+PLAY_W-46, PLAY_Y+3, buf, COLOR_WHITE, COLOR_BLACK, 2);
        prev_lives_hud = lives; changed = true;
    }
    if (level != prev_level_hud || force) {
        renderer_fill_rect(CX-30, PLAY_Y+3, 60, 14, COLOR_BLACK);
        snprintf(buf, sizeof(buf), "NIV %d", level);
        renderer_draw_text(centered_x(buf,2), PLAY_Y+3, buf, COLOR_WHITE, COLOR_BLACK, 2);
        prev_level_hud = level; changed = true;
    }
    if (changed) renderer_flush();
}

static void update_bottom_message(const char *target, int scale) {
    if (strcmp(target, prev_bottom_msg)==0) return;
    renderer_fill_rect(PLAY_X, CY - 10, PLAY_W, 20, COLOR_BLACK);
    if (target[0]) renderer_draw_text(centered_x(target,scale), CY - 8, target, COLOR_WHITE, COLOR_BLACK, scale);
    strncpy(prev_bottom_msg, target, sizeof(prev_bottom_msg)-1);
    prev_bottom_msg[sizeof(prev_bottom_msg)-1]='\0';
    renderer_flush();
}

static void draw_playing_frame(void) {
    if (field_needs_redraw) draw_field_static();

    draw_ball_if_moved();
    draw_paddle_if_moved();
    draw_bullets_if_moved();
    draw_powerups_if_moved();
    draw_hud_if_changed();

    bool bon = (blink/20)%2==0;
    const char *bottom = "";
    if (brk_state==BRK_SERVE && !demo) bottom = "PULSA PARA LANZAR";
    else if (demo && bon)               bottom = "DEMO - PULSA PARA JUGAR";
    update_bottom_message(bottom, 2);
}

// ---------------------------------------------------------------------------
// Pantallas estáticas
// ---------------------------------------------------------------------------
// Franja de seleccion del modo de control, en la pantalla de titulo.
// Separada de draw_title_screen() para poder redibujar SOLO esta
// linea cuando el jugador cambia de modo con arriba/abajo, en vez de
// repintar toda la pantalla (evita parpadeo del resto del titulo).
#define TITLE_CTRL_Y (CY + 78)

static void draw_title_ctrl_mode(void) {
    char line[32];
    snprintf(line, sizeof(line), ">  CONTROL: %s  <", ctrl_mode_names[ctrl_mode]);
    renderer_fill_rect(0, TITLE_CTRL_Y - 2, TFT_WIDTH, 18, COLOR_BLACK);
    renderer_draw_text(centered_x(line,2), TITLE_CTRL_Y, line, COLOR_YELLOW, COLOR_BLACK, 2);
    renderer_flush();
}

static void draw_title_screen(void) {
    renderer_clear(COLOR_BLACK);

    renderer_draw_text(centered_x("BREAKOUT",3), CY-100, "BREAKOUT", COLOR_GREEN, COLOR_BLACK, 3);
    renderer_draw_text(centered_x("PULSA PARA JUGAR",2), CY-10, "PULSA PARA JUGAR", COLOR_MAGENTA, COLOR_BLACK, 2);
    renderer_draw_text(centered_x("MUEVE: MOVER",2), CY+30, "MUEVE: MOVER", COLOR_CYAN, COLOR_BLACK, 2);
    renderer_draw_text(centered_x("BOTON: LANZAR/DISPARAR",2), CY+54, "BOTON: LANZAR/DISPARAR", COLOR_CYAN, COLOR_BLACK, 2);

    int deco_y = CY -65;
    int deco_x = CX - 60;

    for (int c = 0; c < 4; c++) {
        renderer_fill_rect(deco_x + (c * 32), deco_y, 30, 8, COLOR_RED);
        renderer_fill_rect(deco_x + (c * 32), deco_y + 10, 30, 8, COLOR_YELLOW);
        renderer_fill_rect(deco_x + (c * 32), deco_y + 20, 30, 8, COLOR_GREEN);
    }

    renderer_fill_rect(CX - 18, deco_y + 38, 36, 5, COLOR_WHITE);
    renderer_fill_rect(CX - 3, deco_y + 29, 5, 5, COLOR_WHITE);

    draw_title_ctrl_mode();

    prev_bottom_msg[0]='\0';
    renderer_flush();
}

static void draw_over_screen(void) {
    renderer_clear(COLOR_BLACK);
    renderer_draw_text(centered_x("GAME OVER",3), CY-30, "GAME OVER", COLOR_YELLOW, COLOR_BLACK, 3);
    char buf[24];
    snprintf(buf, sizeof(buf), "PUNTOS: %d", score);
    renderer_draw_text(centered_x(buf,2), CY+8, buf, COLOR_WHITE, COLOR_BLACK, 2);
    if (!demo)
        renderer_draw_text(centered_x("PULSA PARA CONTINUAR",2), CY+40,
                            "PULSA PARA CONTINUAR", COLOR_WHITE, COLOR_BLACK, 2);
    renderer_flush();
}

static void draw_scores_screen(void) {
    renderer_clear(COLOR_BLACK);
    highscores_draw(BRK_GAME_ID, "BREAKOUT", 20);
    renderer_flush();
}

static void draw_levelup_screen(void) {
    renderer_clear(COLOR_BLACK);
    char buf[24];
    snprintf(buf, sizeof(buf), "NIVEL %d", level);
    renderer_draw_text(centered_x(buf,3), CY-20, buf, COLOR_YELLOW, COLOR_BLACK, 3);
    renderer_draw_text(centered_x(layout_names[(level-1)%NL],2), CY+16,
                        layout_names[(level-1)%NL], COLOR_WHITE, COLOR_BLACK, 2);
    renderer_flush();
}

// ---------------------------------------------------------------------------
// Física
// ---------------------------------------------------------------------------
static bool warp_active(void) {
    return active_pu == PU_WARP;
}

static void break_brick(int r, int c) {
    uint8_t type = bricks[r][c];
    if (type == 0) return;
    if (type == BRICK_INDESTRUCTIBLE) {
        sound_effect_select();
        return;
    }
    score += BRICK_PTS_BY_TYPE[type] * level;
    bricks[r][c] = 0;
    bricks_left--;
    bricks_broken++;

    renderer_fill_rect(BRICK_X0, BRICK_Y0, BRICK_COLS * BRICK_W, BRICK_ROWS * BRICK_H, COLOR_BLACK);
    draw_bricks_full();
    renderer_flush();

    sound_effect_explosion();

    int bx,by,bw,bh;
    brick_rect(r,c,&bx,&by,&bw,&bh);
    powerup_spawn(bx+bw/2, by+bh);

    if (bricks_broken % BRICKS_PER_ACCEL == 0 && ball_spd < BALL_SPD_MAX_PPS) {
        float old_spd = ball_spd;
        ball_spd += BALL_SPD_INC_PPS;
        if (ball_spd > BALL_SPD_MAX_PPS) ball_spd = BALL_SPD_MAX_PPS;
        ball_bxf = ball_bxf * ball_spd / old_spd;
        ball_byf = ball_byf * ball_spd / old_spd;
    }
}

static bool ball_hits_bricks(void) {
    int cx = ball_x + BALL_SZ/2, cy = ball_y + BALL_SZ/2;
    if (cx < BRICK_X0 || cy < BRICK_Y0) return false;
    int c = (cx - BRICK_X0) / BRICK_W;
    int r = (cy - BRICK_Y0) / BRICK_H;
    if (c<0||c>=BRICK_COLS||r<0||r>=BRICK_ROWS) return false;
    if (bricks[r][c] == 0) return false;

    int bx,by,bw,bh;
    brick_rect(r,c,&bx,&by,&bw,&bh);

    if (bricks[r][c] == BRICK_INDESTRUCTIBLE) {
        if (r == last_ind_r && c == last_ind_c) {
            return true;
        }

        sound_effect_select();
        int overlap_l = (ball_x+BALL_SZ) - bx;
        int overlap_r = (bx+bw) - ball_x;
        int overlap_t = (ball_y+BALL_SZ) - by;
        int overlap_b = (by+bh) - ball_y;
        int min_x = overlap_l < overlap_r ? overlap_l : overlap_r;
        int min_y = overlap_t < overlap_b ? overlap_t : overlap_b;

        if (min_x < min_y) ball_bxf = -ball_bxf;
        else                ball_byf = -ball_byf;

        last_ind_r = r;
        last_ind_c = c;
        ind_cooldown = 12;
        return true;
    }

    int overlap_l = (ball_x+BALL_SZ) - bx;
    int overlap_r = (bx+bw) - ball_x;
    int overlap_t = (ball_y+BALL_SZ) - by;
    int overlap_b = (by+bh) - ball_y;
    int min_x = overlap_l < overlap_r ? overlap_l : overlap_r;
    int min_y = overlap_t < overlap_b ? overlap_t : overlap_b;

    if (min_x < min_y) ball_bxf = -ball_bxf;
    else                ball_byf = -ball_byf;

    break_brick(r,c);
    return true;
}

static void update_ball(float dt) {
    if (ball_held) {
        ball_xf = (float)(pad_x + pad_w/2 - BALL_SZ/2);
        ball_yf = (float)(PAD_Y - BALL_SZ - 2);
        ball_x = (int)(ball_xf + 0.5f);
        ball_y = (int)(ball_yf + 0.5f);
        return;
    }

    ball_xf += ball_bxf * dt;
    ball_yf += ball_byf * dt;
    ball_x = (int)(ball_xf + 0.5f);
    ball_y = (int)(ball_yf + 0.5f);

    if (ball_x <= PLAY_X) {
        ball_x = PLAY_X; ball_xf = (float)PLAY_X;
        ball_bxf = fabsf(ball_bxf); sound_effect_move();
    }
    if (ball_x + BALL_SZ >= PLAY_X+PLAY_W) {
        ball_x = PLAY_X+PLAY_W-BALL_SZ; ball_xf = (float)ball_x;
        ball_bxf = -fabsf(ball_bxf); sound_effect_move();
    }
    if (ball_y <= PLAY_Y) {
        ball_y = PLAY_Y; ball_yf = (float)PLAY_Y;
        ball_byf = fabsf(ball_byf); sound_effect_move();
    }

    ball_hits_bricks();

    // Pala
    if (ball_byf > 0 &&
        ball_x+BALL_SZ > pad_x && ball_x < pad_x+pad_w &&
        ball_y+BALL_SZ > PAD_Y && ball_y < PAD_Y+PAD_H) {
        int diff = (ball_x+BALL_SZ/2) - (pad_x+pad_w/2);
        ball_bxf = clampf(diff * ball_spd / (pad_w/2), -ball_spd, ball_spd);
        float bx2 = ball_bxf*ball_bxf;
        float remain = ball_spd*ball_spd - bx2;
        ball_byf = -(remain > 0.0f ? sqrtf(remain) : ball_spd*0.5f);
        if (ball_byf > -ball_spd/3.0f) ball_byf = -ball_spd/3.0f;
        ball_y = PAD_Y - BALL_SZ - 1;
        ball_yf = (float)ball_y;
        if (active_pu == PU_MAGNET) { ball_magnet = true; ball_held = true; ball_bxf=ball_byf=0.0f; }
        sound_effect_shoot();
    }

    // Bola perdida
    if (ball_y > PLAY_Y+PLAY_H) {
        sound_effect_lose_point();
        lives--;
        if (lives <= 0) {
            pause_cnt = 0;
            brk_state = BRK_DEAD;
        } else {
            pause_cnt = TICKS_S;
            brk_state = BRK_DEAD;
        }
    }
}

static void update_powerups(void) {
    for (int i=0;i<MAX_POWERUPS;i++) {
        if (!powerups[i].active) continue;
        powerups[i].y += PU_SPEED;
        if (powerups[i].y > PLAY_Y+PLAY_H) { powerups[i].active=false; continue; }
        if (powerups[i].x+PU_W > pad_x && powerups[i].x < pad_x+pad_w &&
            powerups[i].y+PU_H > PAD_Y && powerups[i].y < PAD_Y+PAD_H) {
            powerups[i].active = false;
            sound_effect_select();
            activate_powerup(powerups[i].type);
        }
    }
}

static void update_bullets(void) {
    if (shoot_cooldown>0) shoot_cooldown--;
    for (int i=0;i<MAX_BULLETS;i++) {
        if (!bullets[i].active) continue;
        bullets[i].y -= 4;
        if (bullets[i].y < PLAY_Y) { bullets[i].active=false; continue; }
        int cx=bullets[i].x+1, cy=bullets[i].y;
        if (cy < BRICK_Y0) continue;
        int c=(cx-BRICK_X0)/BRICK_W, r=(cy-BRICK_Y0)/BRICK_H;
        if (c>=0&&c<BRICK_COLS&&r>=0&&r<BRICK_ROWS&&bricks[r][c]!=0) {
            bullets[i].active=false;
            break_brick(r,c);
        }
    }
}

static void try_shoot(void) {
    if (active_pu != PU_SHOOT || shoot_cooldown>0) return;
    for (int i=0;i<MAX_BULLETS;i++) {
        if (bullets[i].active) continue;
        bullets[i].active=true;
        bullets[i].x = pad_x+pad_w/2-1;
        bullets[i].y = PAD_Y-6;
        shoot_cooldown = SHOOT_COOLDOWN;
        sound_effect_shoot();
        return;
    }
}

// ---------------------------------------------------------------------------
// IA de la demo -- ahora tambien en px/s (antes PAD_AI_SPEED px/tick)
// ---------------------------------------------------------------------------
static void demo_ai(float dt) {
    int target = ball_x + BALL_SZ/2;
    int center = pad_x + pad_w/2;
    float step = PAD_AI_SPEED_PPS * dt;
    if (center < target-2) pad_xf = clampf(pad_xf + step, (float)PLAY_X, (float)(PLAY_X+PLAY_W-pad_w));
    if (center > target+2) pad_xf = clampf(pad_xf - step, (float)PLAY_X, (float)(PLAY_X+PLAY_W-pad_w));
    pad_x = (int)(pad_xf + 0.5f);
}

// ---------------------------------------------------------------------------
// Tick principal -- ahora recibe dt real (segundos)
// ---------------------------------------------------------------------------
static void brk_tick(float dt) {
    blink++;

    if (demo) {
        bool any = controls_menu_select() || controls_get_raw_delta_x(0) != 0;
        if (any || ++demo_ticks >= TICKS_S * 40) { g_done = true; return; }
    }

    switch (brk_state) {

    case BRK_TITLE:
        if (controls_menu_up() || controls_menu_down()) {
            ctrl_mode = (ctrl_mode == BRK_CTRL_INERTIA) ? BRK_CTRL_DIRECT : BRK_CTRL_INERTIA;
            pad_vel = 0.0f; // sin velocidad heredada si se entra en INERCIA
            draw_title_ctrl_mode();
        }
        if (controls_menu_select()) {
            game_start();
            sound_stop_menu_music();
        }
        break;

    case BRK_SERVE:
        if (!demo) {
            pad_update(dt);
        } else {
            demo_ai(dt);
        }
        ball_x = pad_x + pad_w/2 - BALL_SZ/2;
        ball_xf = (float)ball_x;
        if (demo || controls_menu_select()) {
            ball_held = false;
            brk_state = BRK_PLAYING;
        }
        draw_playing_frame();
        break;

    case BRK_PLAYING:
        if (!demo) {
            pad_update(dt);
            if (controls_menu_select()) {
                if (ball_magnet) {
                    ball_magnet=false; ball_held=false;
                    float sign = (pad_x & 1) ? 1.0f : -1.0f;
                    ball_bxf = ball_spd * 0.5f * sign;
                    ball_byf = -ball_spd;
                } else {
                    try_shoot();
                }
            }

            if (warp_active() &&
                ((warp_side==0 && pad_x <= PLAY_X) ||
                 (warp_side==1 && pad_x+pad_w >= PLAY_X+PLAY_W))) {
                bricks_left = 0;
            }
        } else {
            demo_ai(dt);
            try_shoot();
        }

        if (active_pu_timer > 0 && --active_pu_timer <= 0) deactivate_powerup();

        update_ball(dt);
        update_powerups();
        update_bullets();

        if (bricks_left <= 0 && brk_state == BRK_PLAYING) {
            sound_effect_success();
            draw_playing_frame();
            pause_cnt = TICKS_S * 2;
            brk_state = BRK_LEVELUP;
            draw_levelup_screen();
            break;
        }

        draw_playing_frame();
        break;

    case BRK_DEAD:
        draw_playing_frame();
        if (--pause_cnt <= 0) {
            if (lives <= 0) {
                sound_effect_game_over();
                draw_playing_frame();
                if (!demo && highscores_is_top(BRK_GAME_ID, score)) {
                    highscores_enter(BRK_GAME_ID, (uint32_t)score);
                }
                pause_cnt = 0;
                brk_state = BRK_OVER;
                draw_over_screen();
            } else {
                serve_reset();
                brk_state = BRK_SERVE;
            }
        }
        break;

    case BRK_LEVELUP:
        if (--pause_cnt <= 0) {
            level++;
            level_start();
        }
        break;

    case BRK_OVER:
        if (++pause_cnt > TICKS_S) {
            if (controls_menu_select() || pause_cnt > TICKS_S*8) {
                pause_cnt = 0;
                brk_state = BRK_SCORES;
                draw_scores_screen();
            }
        }
        break;

    case BRK_SCORES:
        if (++pause_cnt > TICKS_S*8) g_done = true;
        if (controls_menu_select()) g_done = true;
        break;
    }
}

// ---------------------------------------------------------------------------
// API pública
// ---------------------------------------------------------------------------
void game_breakout_run(game_mode_t mode) {
    demo = (mode == GAME_MODE_DEMO);
    blink = 0;
    pause_cnt = 0;
    demo_ticks = 0;
    pad_vel = 0.0f;
    g_done = false;
    field_needs_redraw = true;
    prev_ball_x = prev_pad_x = -1;
    prev_score_hud = prev_lives_hud = prev_level_hud = -1;
    prev_bottom_msg[0] = '\0';
    for (int i=0;i<MAX_BULLETS;i++) prev_bullet_active[i]=false;
    for (int i=0;i<MAX_POWERUPS;i++) prev_pu_active[i]=false;

    if (demo) {
        lives = 1; level = 1; score = 0;
        rng_state_v = (uint32_t)esp_timer_get_time();
        level_start();
    } else {
        brk_state = BRK_TITLE;
        draw_title_screen();
        sound_start_menu_music();
    }

    // Mismo bucle a ritmo fijo + dt real que pong.c (ver comentario largo
    // en pong.c sobre por que sleep_ms(8)/vTaskDelay(8) no funcionaba).
    const TickType_t period_ticks = pdMS_TO_TICKS(1000 / TARGET_FPS);
    TickType_t last_wake = xTaskGetTickCount();
    int64_t last_time_us = esp_timer_get_time();

    while (!g_done) {
        controls_update();

        int64_t now_us = esp_timer_get_time();
        float dt = (float)(now_us - last_time_us) / 1000000.0f;
        last_time_us = now_us;
        if (dt > MAX_DT) dt = MAX_DT;
        if (dt <= 0.0f) dt = 1.0f / TARGET_FPS;

        brk_tick(dt);
        sound_update();

        vTaskDelayUntil(&last_wake, period_ticks ? period_ticks : 1);
    }

    highscores_flush();
}