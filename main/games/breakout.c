/**
 * breakout.c -- portado de ArcadePi (https://github.com/JuanLPerea/ArcadePi),
 * mismo concepto (10 layouts de nivel, 5 power-ups, warp, imán,
 * disparo), adaptado a ArcadeColor. Mismos principios de adaptación
 * que pong.c/space_invaders.c/asteroids.c:
 *
 *  - Resolución: campo a pantalla casi completa (320x240 apaisada)
 *    en vez de 768x576. Se mantiene la cuadrícula 10x7 de ladrillos
 *    (para conservar la forma de los layouts: diamante, corazón,
 *    alien...) pero mucho más estrechos.
 *  - Texturas simplificadas: las tramas densas del original
 *    (diagonales, rombos, rayas) serían ilegibles a 30x10px -- cada
 *    tipo de ladrillo se pinta con un COLOR sólido distinto en su
 *    lugar, en vez de una textura.
 *  - Controles: controls_get_raw_delta() para la pala,
 *    controls_menu_select() para lanzar/disparar/soltar imán
 *    (cualquiera de los 6 botones, como en Pong).
 *  - Render incremental: bola y pala con su propio flush; balas y
 *    power-ups agrupados (son pocos y pequeños). Los ladrillos son
 *    estáticos -- se dibujan una vez al entrar en el nivel y solo se
 *    borra la celda exacta que se rompe, no hace falta más.
 *  - Sonido: SFX_BALL_WALL/BALL_PAD/BRK_BRICK/etc. del original no
 *    existen tal cual -- se mapean a sound_effect_move/shoot/
 *    explosion/select. "Bola perdida" reutiliza
 *    sound_effect_lose_point() (la secuencia descendente que ya
 *    construimos para Pong) -- encaja perfecto para esto también.
 *  - Sin hs_input/BRK_ENTER_NAME: highscores_enter() bloqueante,
 *    como en los tres juegos anteriores.
 *  - Bucle propio: game_breakout_run(mode) con su propio bucle.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "breakout.h"
#include "renderer.h"
#include "controls.h"
#include "highscores.h"
#include "sound.h"

// ---------------------------------------------------------------------------
// Área de juego -- literales fijos (ver el mismo comentario, más
// largo, en pong.c/space_invaders.c/asteroids.c sobre por qué no
// TFT_WIDTH/TFT_HEIGHT aquí).
// ---------------------------------------------------------------------------
#define SCREEN_W 320
#define SCREEN_H 240

#define PLAY_X   4
#define PLAY_Y   3
#define PLAY_W   (SCREEN_W - 2 * PLAY_X)   // 312
#define PLAY_H   (SCREEN_H - 2 * PLAY_Y)   // 234
#define CX       (PLAY_X + PLAY_W / 2)
#define CY       (PLAY_Y + PLAY_H / 2)

#define TICKS_S  60   // referencia nominal para pausas simples (no rítmicas)

// ---------------------------------------------------------------------------
// Pala -- reescalada
// ---------------------------------------------------------------------------
#define PAD_H         6
#define PAD_Y        (PLAY_Y + PLAY_H - 16)
#define PAD_W_BASE   44
#define PAD_W_WIDE   70   // power-up WIDE
#define PAD_W_MIN    24
#define PAD_W_SHRINK  3

// Inercia de la pala -- mismo esquema que Pong: enc_momentum()
// acelera la velocidad según las transiciones crudas del encoder
// (controls_get_raw_delta), con un tope y decayendo por fricción
// cuando no se gira. Se llama todos los ticks (incluso con delta=0,
// para que decaiga).
#define PAD_ACCEL      2
#define PAD_VEL_MAX    8
#define PAD_DECAY_NUM  6
#define PAD_DECAY_DEN 10

static int pad_vel = 0;
#define PAD_AI_SPEED 2   // movimiento directo simple para la IA de la demo (no usa encoder)

static int enc_momentum(int enc_raw, int *vel) {
    if (enc_raw > 0) {
        *vel += PAD_ACCEL * enc_raw;
        if (*vel > PAD_VEL_MAX) *vel = PAD_VEL_MAX;
    } else if (enc_raw < 0) {
        *vel += PAD_ACCEL * enc_raw;
        if (*vel < -PAD_VEL_MAX) *vel = -PAD_VEL_MAX;
    } else {
        *vel = *vel * PAD_DECAY_NUM / PAD_DECAY_DEN;
    }
    return *vel;
}

#define WARP_GAP_H   40
#define WARP_GAP_Y   (PAD_Y - (WARP_GAP_H - PAD_H)/2)

// ---------------------------------------------------------------------------
// Bola -- reescalada (mismo esquema de subpíxel /64 que el original,
// independiente del tamaño de pantalla, solo cambia la magnitud)
// ---------------------------------------------------------------------------
#define BALL_SZ         6
#define BALL_SPD0     100
#define BALL_SPD_INC    8
#define BALL_SPD_MAX  280
#define BRICKS_PER_ACCEL 3

// ---------------------------------------------------------------------------
// Ladrillos -- se mantiene la cuadrícula 10x7 (conserva la forma de
// los layouts), mucho más estrechos
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
#define PU_SPEED      1

#define PU_DURATION    (TICKS_S * 15)
#define SHOOT_COOLDOWN 20
#define MAX_BULLETS     3

typedef struct { int x, y; bool active; uint8_t type; } PowerUp;
typedef struct { int x, y; bool active; } Bullet;

// Letras de power-up: F=disparo(Fire), W=warp, L=ancho(Large), M=imán(Magnet), U=vida(Up)
static const char * const pu_labels[6] = { "?", "F", "W", "L", "M", "U" };

// ---------------------------------------------------------------------------
// Tipos de ladrillo -- color sólido por tipo en vez de textura (ver
// cabecera del archivo)
// ---------------------------------------------------------------------------
#define BRICK_INDESTRUCTIBLE 9

static const int BRICK_PTS_BY_TYPE[10] = { 0, 4, 3, 3, 2, 2, 2, 1, 1, 0 };
static const uint16_t BRICK_COLOR_BY_TYPE[10] = {
    COLOR_BLACK,   // 0 vacío (no se dibuja)
    COLOR_RED,     // 1
    COLOR_YELLOW,  // 2
    COLOR_YELLOW,  // 3
    COLOR_GREEN,   // 4
    COLOR_GREEN,   // 5
    COLOR_CYAN,    // 6
    COLOR_CYAN,    // 7
    COLOR_MAGENTA, // 8
    COLOR_WHITE,   // 9 indestructible
};

#define PU_SPAWN_RATE 5   // 1 de cada 5 ladrillos genera power-up

// ---------------------------------------------------------------------------
// Layouts de nivel (BRICK_ROWS x BRICK_COLS = 7x10) -- idénticos al
// original, no dependen de resolución
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

static int pad_x, pad_w;
static int ball_x, ball_y, ball_bx, ball_by, ball_fx, ball_fy;
static bool ball_held, ball_magnet;

static int      lives, level, score, bricks_broken, ball_spd;
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
static int     warp_side; // 0 = hueco a la izquierda, 1 = a la derecha

static uint32_t rng_state_v = 12345;
static uint32_t rng_next(void) {
    rng_state_v ^= rng_state_v << 13;
    rng_state_v ^= rng_state_v >> 17;
    rng_state_v ^= rng_state_v << 5;
    return rng_state_v;
}

static int clamp(int v, int lo, int hi) { return v<lo?lo:v>hi?hi:v; }
static int iabs_brk(int v) { return v<0?-v:v; }

// Rastro de render incremental -- declarado aquí arriba (no junto a
// las funciones de dibujo que lo usan) para que serve_reset() y
// draw_field_static() puedan resetearlo sin problemas de orden.
static int  prev_ball_x=-1, prev_ball_y=-1;
static int  prev_pad_x=-1, prev_pad_w=-1;
static int  prev_bullet_x[MAX_BULLETS], prev_bullet_y[MAX_BULLETS];
static bool prev_bullet_active[MAX_BULLETS];
static int  prev_pu_x[MAX_POWERUPS], prev_pu_y[MAX_POWERUPS];
static bool prev_pu_active[MAX_POWERUPS];

//static int indestructible_cooldown = 0;
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

static void deactivate_powerup(void) {
    if (active_pu == PU_WIDE) pad_w = clamp(PAD_W_BASE - (level-1)*PAD_W_SHRINK, PAD_W_MIN, PAD_W_BASE);
    if (active_pu == PU_MAGNET) {
        ball_magnet = false;
        // Si la bola seguía pegada a la pala por el imán, hay que
        // soltarla aquí: si no, se queda congelada (vx=vy=0) y ya
        // no responde a los botones, porque el lanzamiento manual
        // solo comprobaba ball_magnet, que aquí ya se ha puesto a
        // false (por timeout o por cogerse otro power-up distinto).
        if (ball_held) {
            ball_held = false;
            ball_bx = ball_spd*((pad_x&1)?1:-1)/2;
            ball_by = -ball_spd;
        }
    }
    if (active_pu == PU_WARP) {
        // Borra el hueco (antes se "restauraba" con COLOR_WHITE como
        // si hubiera una pared lateral, pero esas paredes nunca se
        // dibujan -> quedaban marcas blancas fantasma en el borde).
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
        case PU_MAGNET: ball_magnet = false; break; // se activa al TOCAR la pala, no antes
        case PU_WARP: {
            // Hueco a un solo lado, a la altura de la pala
            warp_side = rng_next() & 1;
            int wx = warp_side ? PLAY_X+PLAY_W-3 : PLAY_X;
            renderer_fill_rect(wx, WARP_GAP_Y, 3, WARP_GAP_H, COLOR_CYAN);
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
    // Restablece la velocidad de la bola al valor inicial del nivel actual
    ball_spd = BALL_SPD0 + (level - 1) * BALL_SPD_INC * 2;
    if (ball_spd > BALL_SPD_MAX) ball_spd = BALL_SPD_MAX;

    ball_x = pad_x + pad_w/2 - BALL_SZ/2;
    ball_y = PAD_Y - BALL_SZ - 2;
    int sign = (time_us_32() & 1) ? 1 : -1;
    ball_bx = ball_spd * sign;
    ball_by = -ball_spd;
    ball_fx = 0; ball_fy = 0;
    ball_held = true;
    ball_magnet = false;
    snd_cooldown = 0;
    bullets_clear();

    prev_ball_x = -1;
    prev_pad_x  = -1;
}

static bool field_needs_redraw = true;

static void level_start(void) {
    init_bricks();
    pad_w  = clamp(PAD_W_BASE - (level-1)*PAD_W_SHRINK, PAD_W_MIN, PAD_W_BASE);
    pad_x  = CX - pad_w/2;
    ball_spd = BALL_SPD0 + (level-1)*BALL_SPD_INC*2;
    if (ball_spd > BALL_SPD_MAX) ball_spd = BALL_SPD_MAX;
    bricks_broken = 0;
    powerups_clear();
    bullets_clear();
    deactivate_powerup();
    warp_spawned = (rng_next() % 4 != 0); // true = YA gastado (no aparece este nivel)
    serve_reset();
    brk_state = BRK_SERVE;
    pause_cnt = 0;
    field_needs_redraw = true;
}

static void game_start(void) {
    lives = 3; level = 1; score = 0;
    rng_state_v = (uint32_t)time_us_32();
    level_start();
}

// ---------------------------------------------------------------------------
// Render incremental -- mismo patrón que pong.c (ball/paddle propios,
// bullets/powerups agrupados). Los ladrillos son estáticos: se
// dibujan todos al entrar en el nivel (draw_field_static) y solo se
// borra la celda exacta que se rompe, en el momento en que se rompe.
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

static void erase_brick(int r, int c) {
    int bx,by,bw,bh;
    brick_rect(r,c,&bx,&by,&bw,&bh);
    // Borramos dejando intacto el borde de separación para no afectar a los ladrillos vecinos
    renderer_fill_rect(bx, by, bw , bh , COLOR_BLACK);
    renderer_flush();
}

static void draw_ball_if_moved(void) {
    if (ball_x == prev_ball_x && ball_y == prev_ball_y) return;
    if (prev_ball_x >= 0) renderer_fill_rect(prev_ball_x, prev_ball_y, BALL_SZ, BALL_SZ, COLOR_BLACK);
    renderer_fill_rect(ball_x, ball_y, BALL_SZ, BALL_SZ, ball_magnet ? COLOR_MAGENTA : COLOR_WHITE);
    prev_ball_x = ball_x; prev_ball_y = ball_y;
    renderer_flush();
}

static void draw_paddle_if_moved(void) {
    if (pad_x == prev_pad_x && pad_w == prev_pad_w) return;
    if (prev_pad_x >= 0)
        renderer_fill_rect(prev_pad_x, PAD_Y, prev_pad_w, PAD_H, COLOR_BLACK);
    uint16_t color = (active_pu == PU_WIDE) ? COLOR_CYAN : COLOR_WHITE;
    renderer_fill_rect(pad_x, PAD_Y, pad_w, PAD_H, color);
    // Hueco de warp, si el nivel lo tiene y no se ha activado ya
    prev_pad_x = pad_x; prev_pad_w = pad_w;
    renderer_flush();
}

/*
 * CORREGIDO: antes esta función no reseteaba el rastro de bola/
 * pala/balas/power-ups tras el renderer_clear(). Como la pantalla se
 * borra pero prev_pad_x/prev_ball_x seguían con el valor de ANTES
 * del borrado, draw_paddle_if_moved() y draw_ball_if_moved() creían
 * que "no había cambiado nada" y no redibujaban -- la pala/bola solo
 * volvían a aparecer en cuanto el jugador movía el encoder (lo que
 * dispara un valor de pad_x distinto de verdad). Pasaba tanto al
 * empezar partida como al perder una vida si esta función llegaba a
 * ejecutarse en ese momento.
 */
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
        if (prev_bullet_active[i]) renderer_fill_rect(prev_bullet_x[i], prev_bullet_y[i], 2, 6, COLOR_BLACK);
        if (show) renderer_fill_rect(bullets[i].x, bullets[i].y, 2, 6, COLOR_YELLOW);
        prev_bullet_x[i]=bullets[i].x; prev_bullet_y[i]=bullets[i].y; prev_bullet_active[i]=show;
        any=true;
    }
    if (any) renderer_flush();
}

static void draw_powerups_if_moved(void) {
    bool any=false;
    for (int i=0;i<MAX_POWERUPS;i++) {
        bool show = powerups[i].active;
        if (!show && !prev_pu_active[i]) continue;
        if (prev_pu_active[i]) renderer_fill_rect(prev_pu_x[i], prev_pu_y[i], PU_W, PU_H, COLOR_BLACK);
        if (show) {
            renderer_fill_rect(powerups[i].x, powerups[i].y, PU_W, PU_H, COLOR_BLUE);
            renderer_draw_text(powerups[i].x+PU_W/2-3, powerups[i].y+PU_H/2-4, pu_labels[powerups[i].type], COLOR_WHITE, COLOR_BLUE, 1);
        }
        prev_pu_x[i]=powerups[i].x; prev_pu_y[i]=powerups[i].y; prev_pu_active[i]=show;
        any=true;
    }
    if (any) renderer_flush();
}

// ---------------------------------------------------------------------------
// HUD -- redibujado forzado periódico además de por cambio (lección
// aprendida de asteroids.c: la bola puede pisarlo al rebotar arriba)
// ---------------------------------------------------------------------------
static int prev_score_hud=-1, prev_lives_hud=-1, prev_level_hud=-1;
static absolute_time_t next_hud_refresh;
static char prev_bottom_msg[40] = "";

static void draw_hud_if_changed(void) {
    char buf[16];
    bool changed=false;
    bool force = time_reached(next_hud_refresh);
    if (force) next_hud_refresh = make_timeout_time_ms(700);

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
static void draw_title_screen(void) {
    renderer_clear(COLOR_BLACK);
    
    // Título y opciones principales
    renderer_draw_text(centered_x("BREAKOUT",3), CY-100, "BREAKOUT", COLOR_GREEN, COLOR_BLACK, 3);
    renderer_draw_text(centered_x("PULSA PARA JUGAR",2), CY-10, "PULSA PARA JUGAR", COLOR_MAGENTA, COLOR_BLACK, 2);
    renderer_draw_text(centered_x("GIRA: MOVER",2), CY+30, "GIRA: MOVER", COLOR_CYAN, COLOR_BLACK, 2);
    renderer_draw_text(centered_x("BOTON: LANZAR/DISPARAR",2), CY+54, "BOTON: LANZAR/DISPARAR", COLOR_CYAN, COLOR_BLACK, 2);

    // --- DIBUJITO DECORATIVO (Mini tablero de Breakout en la parte inferior) ---
    int deco_y = CY -65;
    int deco_x = CX - 60;
    
    // Unas pequeñas filas de ladrillos de colores decorativos
    for (int c = 0; c < 4; c++) {
        renderer_fill_rect(deco_x + (c * 32), deco_y, 30, 8, COLOR_RED);
        renderer_fill_rect(deco_x + (c * 32), deco_y + 10, 30, 8, COLOR_YELLOW);
        renderer_fill_rect(deco_x + (c * 32), deco_y + 20, 30, 8, COLOR_GREEN);
    }
    
    // Una mini pala y la pelota rebotando
    renderer_fill_rect(CX - 18, deco_y + 38, 36, 5, COLOR_WHITE); // Pala
    renderer_fill_rect(CX - 3, deco_y + 29, 5, 5, COLOR_WHITE);   // Pelota

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
    
    // Limpiamos toda la zona de ladrillos y redibujamos los que quedan vivos de una sola vez
    renderer_fill_rect(BRICK_X0, BRICK_Y0, BRICK_COLS * BRICK_W, BRICK_ROWS * BRICK_H, COLOR_BLACK);
    draw_bricks_full();
    renderer_flush();

    sound_effect_explosion();

    int bx,by,bw,bh;
    brick_rect(r,c,&bx,&by,&bw,&bh);
    powerup_spawn(bx+bw/2, by+bh);

    if (bricks_broken % BRICKS_PER_ACCEL == 0 && ball_spd < BALL_SPD_MAX) {
        int old_spd = ball_spd;
        ball_spd += BALL_SPD_INC;
        if (ball_spd > BALL_SPD_MAX) ball_spd = BALL_SPD_MAX;
        ball_bx = ball_bx * ball_spd / old_spd;
        ball_by = ball_by * ball_spd / old_spd;
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
        // Si es el mismo bloque que acabamos de golpear recientemente, lo ignoramos para salir
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

        if (min_x < min_y) ball_bx = -ball_bx;
        else                ball_by = -ball_by;

        // Registramos este bloque y activamos un pequeño periodo de gracia
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

    if (min_x < min_y) ball_bx = -ball_bx;
    else                ball_by = -ball_by;

    break_brick(r,c);
    return true;
}

static void update_ball(void) {
    if (ball_held) {
        ball_x = pad_x + pad_w/2 - BALL_SZ/2;
        ball_y = PAD_Y - BALL_SZ - 2;
        return;
    }

    ball_fx += ball_bx; ball_fy += ball_by;
    while (ball_fx >= 64) { ball_x++; ball_fx -= 64; }
    while (ball_fx <= -64){ ball_x--; ball_fx += 64; }
    while (ball_fy >= 64) { ball_y++; ball_fy -= 64; }
    while (ball_fy <= -64){ ball_y--; ball_fy += 64; }

    if (ball_x <= PLAY_X) {
        ball_x = PLAY_X; ball_bx = iabs_brk(ball_bx); sound_effect_move();
    }
    if (ball_x + BALL_SZ >= PLAY_X+PLAY_W) {
        ball_x = PLAY_X+PLAY_W-BALL_SZ; ball_bx = -iabs_brk(ball_bx); sound_effect_move();
    }
    if (ball_y <= PLAY_Y) { ball_y = PLAY_Y; ball_by = iabs_brk(ball_by); sound_effect_move(); }

    ball_hits_bricks();

    // Pala
    if (ball_by > 0 &&
        ball_x+BALL_SZ > pad_x && ball_x < pad_x+pad_w &&
        ball_y+BALL_SZ > PAD_Y && ball_y < PAD_Y+PAD_H) {
        int diff = (ball_x+BALL_SZ/2) - (pad_x+pad_w/2);
        ball_bx = clamp(diff * ball_spd / (pad_w/2), -ball_spd, ball_spd);
        int bx2 = ball_bx*ball_bx, remain = ball_spd*ball_spd - bx2;
        ball_by = -(int)(remain>0 ? (int)(remain*100/(ball_spd>0?ball_spd:1))/100+1 : ball_spd/2);
        if (ball_by > -ball_spd/3) ball_by = -ball_spd/3;
        ball_y = PAD_Y - BALL_SZ - 1;
        if (active_pu == PU_MAGNET) { ball_magnet = true; ball_held = true; ball_bx=ball_by=0; }
        sound_effect_shoot();
    }

    // Bola perdida
    if (ball_y > PLAY_Y+PLAY_H) {
        sound_effect_lose_point();
        lives--;
        if (lives <= 0) {
            pause_cnt = 0;
            brk_state = BRK_DEAD; // el tick decide GAME_OVER vs respawn según lives
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
            sound_effect_powerup();
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
// IA de la demo
// ---------------------------------------------------------------------------
static void demo_ai(void) {
    int target = ball_x + BALL_SZ/2;
    int center = pad_x + pad_w/2;
    if (center < target-2) pad_x = clamp(pad_x+PAD_AI_SPEED, PLAY_X, PLAY_X+PLAY_W-pad_w);
    if (center > target+2) pad_x = clamp(pad_x-PAD_AI_SPEED, PLAY_X, PLAY_X+PLAY_W-pad_w);
}

// ---------------------------------------------------------------------------
// Tick principal
// ---------------------------------------------------------------------------
static void brk_tick(void) {
    blink++;

    if (demo) {
        bool any = controls_menu_select() || controls_get_raw_delta(0) != 0;
        if (any || ++demo_ticks >= TICKS_S * 40) { g_done = true; return; }
    }

    switch (brk_state) {

    case BRK_TITLE:
        if (controls_menu_select()) {
            game_start();
            sound_stop_menu_music();
        }
        break;

    case BRK_SERVE:
        if (!demo) {
            int d = controls_get_raw_delta(0);
            pad_x = clamp(pad_x + enc_momentum(d, &pad_vel), PLAY_X, PLAY_X+PLAY_W-pad_w);
        } else {
            demo_ai();
        }
        ball_x = pad_x + pad_w/2 - BALL_SZ/2;
        if (demo || controls_menu_select()) {
            ball_held = false;
            brk_state = BRK_PLAYING;
        }
        draw_playing_frame();
        break;

    case BRK_PLAYING:
        if (!demo) {
            int d = controls_get_raw_delta(0);
            pad_x = clamp(pad_x + enc_momentum(d, &pad_vel), PLAY_X, PLAY_X+PLAY_W-pad_w);
            if (controls_menu_select()) {
                if (ball_magnet) { ball_magnet=false; ball_held=false; ball_bx=ball_spd*((pad_x&1)?1:-1)/2; ball_by=-ball_spd; }
                else try_shoot();
            }


            /*
            // --- ATAJO DE DEPURACIÓN: Salto de nivel con el switch de J2 ---
            // (Reemplaza 'controls_p2_select()' por la función de tu librería de controls para el J2)
            if (controls_button_pressed(4) == true) {
                bricks_left = 0;
            }
            */

            // La pala ha llegado al lado donde el warp abrió su
            // escapatoria: se pasa de nivel directamente.
            if (warp_active() &&
                ((warp_side==0 && pad_x <= PLAY_X) ||
                 (warp_side==1 && pad_x+pad_w >= PLAY_X+PLAY_W))) {
                bricks_left = 0;
            }
        } else {
            demo_ai();
            try_shoot();
        }

        if (active_pu_timer > 0 && --active_pu_timer <= 0) deactivate_powerup();

        update_ball();
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
                sound_effect_teleport();
                draw_playing_frame();
                if (!demo && highscores_is_top(BRK_GAME_ID, score)) {
                    highscores_enter(BRK_GAME_ID, (uint32_t)score); // bloqueante
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
    pad_vel = 0;
    g_done = false;
    field_needs_redraw = true;
    prev_ball_x = prev_pad_x = -1;
    prev_score_hud = prev_lives_hud = prev_level_hud = -1;
    prev_bottom_msg[0] = '\0';
    for (int i=0;i<MAX_BULLETS;i++) prev_bullet_active[i]=false;
    for (int i=0;i<MAX_POWERUPS;i++) prev_pu_active[i]=false;

    if (demo) {
        lives = 1; level = 1; score = 0;
        rng_state_v = (uint32_t)time_us_32();
        level_start();
    } else {
        brk_state = BRK_TITLE;
        draw_title_screen();
        sound_start_menu_music();
    }

    while (!g_done) {
        controls_update();
        brk_tick();
        sound_update();
        sleep_ms(8);
    }

    highscores_flush();
}
