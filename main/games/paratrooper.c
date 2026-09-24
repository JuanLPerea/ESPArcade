/**
 * paratrooper.c -- portado de la versión Pi Pico Arcade v5 (768x576,
 * callbacks de dibujo/tick, renderer_draw_rect inmediato, hs_input
 * bloqueante por callback) a ArcadeColor (320x240, framebuffer SPI
 * con flush incremental, bucle propio, highscores_enter() bloqueante).
 * Mismo criterio de adaptación que asteroids.c:
 *
 *  - Resolución: 320x240 en vez de 768x576. TODA la geometría del
 *    juego (torreta, helicópteros, soldados, paracaídas, avión,
 *    bombas, ranuras de aterrizaje...) se ha reescalado a mano
 *    (~0.5-0.6x, no proporcionalmente exacta) y algunos trazos muy
 *    finos del original (el hueco triangular de la cabina del heli,
 *    la hélice de cola animada en 4 fases geométricas) se han
 *    simplificado a versiones más robustas a este tamaño de pantalla
 *    -- siguen siendo reconocibles pero no son replicas pixel-a-pixel.
 *  - FÍSICA CON TIEMPO DELTA REAL (g_dt_scale, ver asteroids.c): el
 *    original asumía un tick fijo de ~16ms a 62Hz. Aquí, con muchas
 *    entidades a la vez (helis + paracaidistas + soldados marchando +
 *    bombas + balas + partículas), el coste de cada flush SPI varía
 *    frame a frame igual que en Asteroids, así que todo movimiento
 *    continuo se escala por el tiempo real transcurrido. Las
 *    aceleraciones/fricciones puntuales (gravedad de partículas,
 *    apertura de paracaídas) se aplican tal cual una vez por tick,
 *    sin escalar -- mismo criterio que la fricción de la nave en
 *    asteroids.c -- solo la INTEGRACIÓN de posición se escala por dt.
 *    La marcha/escalada de la torre (1px cada N ticks) se deja en
 *    ticks discretos, sin dt, igual que el tic-tac de Asteroids.
 *  - Controles: Enc1 (controls_get_raw_delta(0)) gira el cañón;
 *    BTN_J1_A/BTN_J1_B (cualquiera de los dos) dispara; los switches
 *    de encoder (índices 4 y 5 en button_pins[], NO los números de
 *    pin -- ver el mismo aviso de bug en asteroids.c) salen al menú
 *    en cualquier momento, igual que "SW1: salir" en el original.
 *  - Sonido: sound.c no tiene el enum de efectos con datos de nota
 *    embebidos del original (SFX_FIRE, SFX_AST_*, SFX_BALL_*...);
 *    cada uno se mapea al efecto más parecido de nuestro motor
 *    (sound_effect_shoot/explosion/move/success/game_over...), y el
 *    "avión" reutiliza sound_siren_start()/stop() como aviso de
 *    aproximación, igual que el OVNI de asteroids.c.
 *  - Sin hs_input/PT_ENTER_NAME: highscores_enter() bloqueante, como
 *    en asteroids.c/pong.c/space_invaders.c.
 *  - Bucle propio: paratrooper_run()/paratrooper_demo() con su
 *    propio bucle (controls_update + tick + sound_update + sleep),
 *    nada de callbacks de dibujo/tick registrados aparte -- la firma
 *    pública (paratrooper_init/run/demo, GameResult) es la que ya
 *    fija paratrooper.h, no la de asteroids.c (game_mode_t).
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "paratrooper.h"
#include "renderer.h"
#include "controls.h"
#include "highscores.h"
#include "sound.h"

// ---------------------------------------------------------------------------
// Pantalla / área de juego -- literales fijos, ver el mismo comentario
// (más largo) en asteroids.c sobre por qué no usar TFT_WIDTH/HEIGHT aquí.
// ---------------------------------------------------------------------------
#define SCREEN_W 320
#define SCREEN_H 240

#define PLAY_X   4
#define PLAY_Y   3
#define PLAY_W   (SCREEN_W - 2 * PLAY_X)   // 312
#define PLAY_H   (SCREEN_H - 2 * PLAY_Y)   // 234
#define CX       (PLAY_X + PLAY_W / 2)
#define CY       (PLAY_Y + PLAY_H / 2)

#define TICKS_S  60   // referencia nominal SOLO para convertir a ms (ver g_dt_scale)

/*
 * OJO -- mismo bug de controls.c que en asteroids.c: controls_button_pressed()
 * /controls_button_down() toman su argumento como ÍNDICE dentro de
 * button_pins[] (0..5), no el número de pin GPIO. button_pins[] tiene el
 * orden J1_A, J1_B, J2_A, J2_B, ENC1_SW, ENC2_SW -- de ahí el 4 y el 5.
 */
#define BTN_IDX_ENC1_SW  4
#define BTN_IDX_ENC2_SW  5

// ---------------------------------------------------------------------------
// Punto fijo Q8
// ---------------------------------------------------------------------------
#define FP         256
#define PX2FP(x)   ((int32_t)(x) * FP)
#define FP2PX(x)   ((int)((x) / FP))

// ---------------------------------------------------------------------------
// Tiempo delta real -- idéntico mecanismo que asteroids.c.
// ---------------------------------------------------------------------------
static absolute_time_t last_tick_time;
static int32_t g_dt_scale = FP;

static void update_dt_scale(void) {
    int64_t elapsed_us = absolute_time_diff_us(last_tick_time, get_absolute_time());
    last_tick_time = get_absolute_time();

    int32_t elapsed_ms = (int32_t)(elapsed_us / 1000);
    if (elapsed_ms < 1)  elapsed_ms = 1;
    if (elapsed_ms > 50) elapsed_ms = 50; // limita saltos tras una pausa larga (highscores_enter())

    g_dt_scale = elapsed_ms * FP / 16;
}

// ---------------------------------------------------------------------------
// Trigonometría -- idéntica al original (tabla de grados, no de pasos).
// ---------------------------------------------------------------------------
static const int16_t SIN90[91] = {
  0,4,9,13,18,22,27,31,36,40,44,49,53,57,62,66,70,74,79,83,
  87,91,95,99,103,107,111,115,119,122,126,130,133,137,140,144,
  147,150,154,157,160,163,166,169,172,175,177,180,182,185,187,
  190,192,194,196,198,200,202,204,205,207,208,210,211,212,213,
  214,215,216,217,218,218,219,219,220,220,220,220,220,220,220,
  221,221,221,222,222,222,222,223,223,256
};
static int16_t sin_deg(int d) {
    d = d % 360; if (d < 0) d += 360;
    if (d <= 90)  return  SIN90[d];
    if (d <= 180) return  SIN90[180-d];
    if (d <= 270) return -SIN90[d-180];
    return             -SIN90[360-d];
}
static int16_t cos_deg(int d) { return sin_deg(d + 90); }
static inline int16_t can_dx(int a) { return  sin_deg(a); }
static inline int16_t can_dy(int a) { return -cos_deg(a); }

// ---------------------------------------------------------------------------
// Soldado -- SOLDIER_H = altura pies-cabeza. BASE_H = 2*SOLDIER_H exactos
// para que la escalera de la torre encaje.
// ---------------------------------------------------------------------------
#define SOLDIER_H    15

// ---------------------------------------------------------------------------
// Torreta
// ---------------------------------------------------------------------------
#define TURRET_X     CX
#define TURRET_Y     (PLAY_Y + PLAY_H - 12)   // pie de la base = suelo
#define BASE_H       (2 * SOLDIER_H)          // 30
#define BASE_W       24
#define BODY_RX      7
#define BODY_RY      13
#define CANNON_LEN   16
#define CANNON_W     2
#define TURRET_R     10

#define GROUND_Y     (TURRET_Y - 1)

#define CANNON_OX    TURRET_X
#define CANNON_OY    (TURRET_Y - BASE_H - BODY_RY/2)

// ---------------------------------------------------------------------------
// Ranuras de aterrizaje -- 7 por lado, X fija al aterrizar.
// ---------------------------------------------------------------------------
#define NUM_SLOTS    7
#define SLOT_MARGIN  (BASE_W/2 + 6)
#define SLOT_EDGE    10
#define SLOT_SPACE_L (TURRET_X - PLAY_X - SLOT_MARGIN - SLOT_EDGE)
#define SLOT_SPACE_R (PLAY_X + PLAY_W - TURRET_X - SLOT_MARGIN - SLOT_EDGE)
#define SLOT_X_L(n)  (TURRET_X - SLOT_MARGIN - ((n) * SLOT_SPACE_L) / (NUM_SLOTS-1))
#define SLOT_X_R(n)  (TURRET_X + SLOT_MARGIN + ((n) * SLOT_SPACE_R) / (NUM_SLOTS-1))
#define SLOT_X(s,n)  ((s)==0 ? SLOT_X_L(n) : SLOT_X_R(n))
#define SLOT_STACK   3

// ---------------------------------------------------------------------------
// Helicópteros -- 2 franjas fijas. HELI_SPD en px/tick "nominal" (no FP),
// escalado por g_dt_scale en update_helis() -- ver cabecera del archivo.
// ---------------------------------------------------------------------------
#define LANE0_Y       (PLAY_Y + 15)
#define LANE1_Y       (PLAY_Y + 36)
#define HELI_SPD      1
#define HELI_GAP      (TICKS_S * 2)
#define HELI_MIN_SEP  80
#define DROP_PROB     5

#define ATTACK_DUR         (TICKS_S * 6)
#define ATTACK_INTERVAL    (TICKS_S * 20)
#define DROP_PROB_ATTACK   3

// ---------------------------------------------------------------------------
// Cañón
// ---------------------------------------------------------------------------
#define ANGLE_MIN_DEG  (-75)
#define ANGLE_MAX_DEG  ( 75)
#define ANGLE_UP_DEG   (  0)
#define CANNON_ROT_SPD  2

// ---------------------------------------------------------------------------
// Torre / escalera -- march/climb se dejan en ticks discretos (sin dt),
// igual que el tic-tac de asteroids.c.
// ---------------------------------------------------------------------------
#define TOWER_COUNT   4
#define MARCH_TICKS   2
#define CLIMB_TICKS   1

// ---------------------------------------------------------------------------
// Constantes de entidades -- reducidas respecto al original (pantalla
// más pequeña, menos carga de render incremental), mismo criterio que
// asteroids.c.
// ---------------------------------------------------------------------------
#define MAX_HELIS      8
#define MAX_PARAS_AIR  5
#define MAX_PARAS      24
#define MAX_BULLETS    6
#define MAX_BOMBS      2
#define MAX_PARTICLES  32

#define PTS_HELI            150
#define PTS_PARA_AIR         25
#define PTS_PARA_CHUTE_HIT   15
#define PTS_PARA_GROUND      10
#define PTS_PLANE           500
#define PTS_BOMB_DODGE       50
#define PTS_WAVE_BONUS      500

#define BULLET_SPD   4   // px/tick nominal, ver player_fire()
#define PLANE_SPD    2   // px/tick nominal

#define CHUTE_OPEN_Y_MIN  (PLAY_Y + PLAY_H/5)
#define CHUTE_OPEN_Y_MAX  (PLAY_Y + PLAY_H*2/3)

// ---------------------------------------------------------------------------
// Tipos
// ---------------------------------------------------------------------------
typedef struct {
    int32_t x;          // FP
    int     y;           // Y fija de la franja
    int     vx;           // px/tick nominal, con signo (dirección)
    bool    active;
    int     drop_timer, anim_timer;
    int     rotor_frame, tail_frame;
} Heli;

typedef struct {
    int32_t x, y, vy;    // FP
    bool    active, chute_open, chute_shot, landed;
    int     side;         // 0=izq 1=der
    int     slot;         // ranura asignada, -1=en vuelo
    int     stack;        // nivel de apilamiento (0=suelo)
    int     chute_open_y;
    bool    marching, climbing;
    int     march_order;  // 0..3 en la fila de ataque, -1=sin asignar
    int32_t target_x, target_y;
    int     anim_timer;
} Para;

typedef struct { int32_t x, y, vx, vy; bool active; } Bullet;
typedef struct { int32_t x, y, vx, vy, ay; bool active; } Bomb;
typedef struct {
    int32_t x, y; int vx; bool active;
    int bomb_dropped, anim_timer, drop_delay;
} Plane;
typedef struct { int32_t x, y, vx, vy; int life, life_max; bool active; } Particle;

// ---------------------------------------------------------------------------
// Estados
// ---------------------------------------------------------------------------
typedef enum {
    PT_TITLE, PT_PLAYING, PT_JET_PASS, PT_DEAD, PT_GAMEOVER, PT_SCORES
} PtState;

static PtState state;
static int     blink;
static bool    demo_mode;
static int     pause_cnt, demo_ticks;
static int     score, wave;
static bool    g_done;

// Cañón
static int  cannon_angle_deg;
static int  cannon_rot_dir;
static int  cannon_inv;
static bool cannon_alive;
static int  cannon_enc_acc;
static bool fire_held;

// Suelo
static int  slot_count[2][NUM_SLOTS];
static int  ground_total[2];

// Torre marchante
static bool tower_active;
static int  tower_side;
static bool helis_frozen;

// Oleada
static int  wave_heli_budget;
static int  wave_helis_spawned;
static bool wave_done;
static int  jet_delay;
static bool jet_launched;

// Modo ataque
static bool attack_mode;
static int  attack_timer;
static int  attack_cooldown;

static int lane_timer[2];

// Entidades
static Heli     helis[MAX_HELIS];
static Para     paras[MAX_PARAS];
static Bullet   bullets[MAX_BULLETS];
static Bomb     bombs[MAX_BOMBS];
static Plane    plane;
static Particle particles[MAX_PARTICLES];

// ---------------------------------------------------------------------------
// Utilidades
// ---------------------------------------------------------------------------
static int  rnd(int n)  { return n > 0 ? rand() % n : 0; }
static int  absi(int x) { return x < 0 ? -x : x; }
static bool chit(int ax, int ay, int ra, int bx, int by, int rb) {
    int dx = ax-bx, dy = ay-by, r = ra+rb;
    return (dx*dx + dy*dy) <= r*r;
}
static int count_paras_air(void) {
    int n = 0;
    for (int i=0;i<MAX_PARAS;i++)
        if (paras[i].active && !paras[i].landed) n++;
    return n;
}

// ---------------------------------------------------------------------------
// Partículas
// ---------------------------------------------------------------------------
static void spawn_expl(int wx, int wy, int n, int spd, int life) {
    static const int16_t S32[32] = {
        0,50,98,142,181,213,237,251,256,251,237,213,181,142,98,50,
        0,-50,-98,-142,-181,-213,-237,-251,-256,-251,-237,-213,-181,-142,-98,-50};
    int done = 0;
    for (int i=0;i<MAX_PARTICLES && done<n;i++) {
        if (particles[i].active) continue;
        int a = rnd(32), s = spd/2 + rnd(spd/2+1);
        particles[i].active = true;
        particles[i].x = PX2FP(wx); particles[i].y = PX2FP(wy);
        particles[i].vx = (int32_t)s*S32[(a+8)&31]/256;
        particles[i].vy = (int32_t)s*S32[a]/256;
        particles[i].life = life + rnd(life/3+1);
        particles[i].life_max = particles[i].life;
        done++;
    }
}
static void upd_particles(void) {
    for (int i=0;i<MAX_PARTICLES;i++) {
        Particle *p = &particles[i]; if (!p->active) continue;
        p->vy += FP/8;   // gravedad, aplicada tal cual una vez por tick
        p->x  += p->vx * g_dt_scale / FP;
        p->y  += p->vy * g_dt_scale / FP;
        if (--p->life <= 0) p->active = false;
    }
}

// ---------------------------------------------------------------------------
// Primitivas de dibujo vectorial -- líneas por Bresenham, 1px (a esta
// escala ya reducida no hace falta engordarlas como en asteroids.c).
// ---------------------------------------------------------------------------
static void line(int x0, int y0, int x1, int y1, uint16_t color) {
    int dx = x1-x0; if (dx<0) dx=-dx;
    int dy = y1-y0; if (dy<0) dy=-dy;
    int sx = x0<x1 ? 1:-1, sy = y0<y1 ? 1:-1;
    int err = dx - dy;
    for (;;) {
        if (x0>=0 && y0>=0) renderer_fill_rect(x0, y0, 1, 1, color);
        if (x0==x1 && y0==y1) break;
        int e2 = 2*err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

// Isqrt entero de num/den por Newton -- usado por las dos primitivas
// rellenas de abajo (cúpula, dosel).
static int isqrt_ratio(int32_t num, int32_t den) {
    if (num <= 0) return 0;
    int32_t q = num/den;
    int w = 0;
    if (q > 0) {
        int32_t r = q;
        for (int k=0;k<8;k++) { int32_t r2=(r+q/r)>>1; if (r2>=r) break; r=r2; }
        w = (int)r;
    }
    while ((int32_t)(w+1)*(w+1)*den <= num) w++;
    return w;
}

// Semielipse superior rellena (cúpula de la torreta).
static void fill_top_ellipse(int cx, int cy, int rx, int ry, uint16_t color) {
    for (int row=0; row<=ry; row++) {
        int w = isqrt_ratio((int32_t)rx*rx*((int32_t)ry*ry-(int32_t)row*row), (int32_t)ry*ry);
        if (w <= 0 && row < ry) continue;
        renderer_fill_rect(cx-w, cy-row, 2*w+1, 1, color);
    }
}

// Elipse completa rellena (fuselaje del helicóptero).
static void fill_ellipse(int cx, int cy, int rx, int ry, uint16_t color) {
    for (int row=-ry; row<=ry; row++) {
        int w = isqrt_ratio((int32_t)rx*rx*((int32_t)ry*ry-(int32_t)row*row), (int32_t)ry*ry);
        if (w <= 0) continue;
        renderer_fill_rect(cx-w, cy+row, 2*w+1, 1, color);
    }
}

// Dosel de paracaídas: ancho arriba, estrecho abajo (cy = borde inferior,
// de donde arrancan las cuerdas).
static void draw_chute_filled(int cx, int cy, int rw, int rh, uint16_t color) {
    for (int row=0; row<=rh; row++) {
        int w = isqrt_ratio((int32_t)rw*rw*(rh-row), rh);
        if (row < rh && w < 1) w = 1;
        if (w > 0) renderer_fill_rect(cx-w, cy-row, 2*w+1, 1, color);
    }
}

// ---------------------------------------------------------------------------
// Colores -- paleta CGA de 4 colores (negro/cian/magenta/blanco), como
// en el original: torreta base blanca + cañón cian, helicópteros cuerpo
// blanco + detalles magenta, avión cian.
// ---------------------------------------------------------------------------
#define COLOR_TURRET_BASE  COLOR_WHITE
#define COLOR_TURRET_GUN   COLOR_CYAN
#define COLOR_HELI_BODY    COLOR_WHITE
#define COLOR_HELI_ACCENT  COLOR_MAGENTA
#define COLOR_SOLDIER      COLOR_GREEN
#define COLOR_CHUTE        COLOR_CYAN
#define COLOR_JET          COLOR_CYAN
#define COLOR_BOMB         COLOR_MAGENTA
#define COLOR_BULLET       COLOR_WHITE
#define COLOR_PART         COLOR_MAGENTA

// ---------------------------------------------------------------------------
// Gestión de ranuras y torre
// ---------------------------------------------------------------------------
static void kill_slot(int side, int slot) {
    int n = 0;
    for (int i=0;i<MAX_PARAS;i++) {
        Para *p = &paras[i];
        if (!p->active || !p->landed || p->side!=side || p->slot!=slot) continue;
        spawn_expl(FP2PX(p->x), GROUND_Y - p->stack*SOLDIER_H, 4, 3, 14);
        p->active = false; n++;
    }
    if (n > 0) {
        score += PTS_PARA_GROUND*n;
        slot_count[side][slot] = 0;
        ground_total[side] -= n;
        if (ground_total[side] < 0) ground_total[side] = 0;
        if (tower_active && tower_side==side && ground_total[side]<TOWER_COUNT) {
            tower_active = false; helis_frozen = false;
        }
        sound_effect_explosion();
    }
}

static int find_slot(int side, int land_x) {
    int best=-1, bd=0x7fffffff;
    for (int n=0;n<NUM_SLOTS;n++) {
        if (slot_count[side][n]>=SLOT_STACK) continue;
        int d = absi(SLOT_X(side,n)-land_x);
        if (d<bd) { bd=d; best=n; }
    }
    if (best<0) {
        for (int n=0;n<NUM_SLOTS;n++) {
            int d = absi(SLOT_X(side,n)-land_x);
            if (d<bd) { bd=d; best=n; }
        }
    }
    return best;
}

static int pick_slot_x(int side) {
    int tries[NUM_SLOTS];
    for (int i=0;i<NUM_SLOTS;i++) tries[i]=i;
    for (int i=NUM_SLOTS-1;i>0;i--) {
        int j=rnd(i+1); int t=tries[i]; tries[i]=tries[j]; tries[j]=t;
    }
    for (int i=0;i<NUM_SLOTS;i++) {
        int n = tries[i];
        if (slot_count[side][n]<SLOT_STACK) return SLOT_X(side,n);
    }
    return SLOT_X(side, rnd(NUM_SLOTS));
}

// ---------------------------------------------------------------------------
// Reset de ola / inicio de partida
// ---------------------------------------------------------------------------
static void reset_wave(void) {
    for (int i=0;i<MAX_HELIS;i++) helis[i].active=false;
    for (int i=0;i<MAX_PARAS;i++) if (!paras[i].landed) paras[i].active=false;
    for (int i=0;i<MAX_BULLETS;i++) bullets[i].active=false;
    for (int i=0;i<MAX_BOMBS;i++)   bombs[i].active=false;
    plane.active=false;
    for (int i=0;i<MAX_PARTICLES;i++) particles[i].active=false;
    for (int s=0;s<2;s++) {
        ground_total[s]=0;
        for (int n=0;n<NUM_SLOTS;n++) slot_count[s][n]=0;
    }
    tower_active=false; helis_frozen=false;
    wave_done=false; jet_delay=0; jet_launched=false;
    attack_mode=false; attack_timer=0;
    attack_cooldown=ATTACK_INTERVAL;
    wave_heli_budget = 12 + wave*2; if (wave_heli_budget>48) wave_heli_budget=48;
    wave_helis_spawned=0;
    lane_timer[0]=TICKS_S;
    lane_timer[1]=TICKS_S*2+rnd(TICKS_S);
    sound_siren_stop(); // por si el avión seguía sonando
}

static void game_start(void) {
    score=0; wave=1;
    cannon_angle_deg=ANGLE_UP_DEG; cannon_rot_dir=0;
    cannon_alive=true; cannon_inv=TICKS_S*2; cannon_enc_acc=0;
    fire_held=false;
    reset_wave();
    // Jingle de inicio (una sola pasada, ~8.6s) -- se apaga solo y a
    // partir de ahí solo se oyen los efectos (sound_effect_shoot/
    // explosion/..., y la sirena del avión, que cede/retoma el canal 2
    // sin cortar la música mientras suena -- ver sound_update() y
    // sound_siren_stop() en sound.c).
    sound_start_paratrooper_music();
    state=PT_PLAYING;
}

// ---------------------------------------------------------------------------
// Spawn
// ---------------------------------------------------------------------------
static void spawn_heli(int lane) {
    int dir = (lane==0) ? 1 : -1;
    int lane_y = (lane==0) ? LANE0_Y : LANE1_Y;
    int entry_x = (dir==1) ? (PLAY_X-20) : (PLAY_X+PLAY_W+20);

    for (int i=0;i<MAX_HELIS;i++) {
        if (!helis[i].active) continue;
        if (helis[i].y != lane_y) continue;
        int hx = FP2PX(helis[i].x);
        if (absi(hx-entry_x) < HELI_MIN_SEP) return;
    }

    for (int i=0;i<MAX_HELIS;i++) {
        if (helis[i].active) continue;
        helis[i].active = true;
        helis[i].x = PX2FP(entry_x);
        helis[i].y = lane_y;
        helis[i].vx = dir*HELI_SPD;
        helis[i].drop_timer = TICKS_S*2 + rnd(TICKS_S*3);
        helis[i].anim_timer = 0;
        helis[i].rotor_frame = 0; helis[i].tail_frame = 0;
        break;
    }
}

static void spawn_para_at_slot(int side, int hy) {
    if (count_paras_air() >= MAX_PARAS_AIR) return;
    int sx = pick_slot_x(side);
    for (int i=0;i<MAX_PARAS;i++) {
        if (paras[i].active) continue;
        Para *p = &paras[i];
        p->active = true;
        p->x = PX2FP(sx); p->y = PX2FP(hy+8);
        p->vy = PX2FP(1);
        p->chute_open = p->chute_shot = p->landed = false;
        p->marching = p->climbing = false;
        p->march_order = -1;
        p->target_x = p->target_y = 0;
        p->slot = -1; p->stack = 0;
        p->side = side;
        p->anim_timer = 0;
        p->chute_open_y = CHUTE_OPEN_Y_MIN + rnd(CHUTE_OPEN_Y_MAX-CHUTE_OPEN_Y_MIN);
        break;
    }
}

static void spawn_para(int hx, int hy) {
    int side = (hx < TURRET_X) ? 0 : 1;
    spawn_para_at_slot(side, hy);
}

static void player_fire(void) {
    for (int i=0;i<MAX_BULLETS;i++) {
        if (bullets[i].active) continue;
        int dx = can_dx(cannon_angle_deg), dy = can_dy(cannon_angle_deg);
        int tip_x = CANNON_OX + dx*CANNON_LEN/256;
        int tip_y = CANNON_OY + dy*CANNON_LEN/256;
        bullets[i].active = true;
        bullets[i].x = PX2FP(tip_x); bullets[i].y = PX2FP(tip_y);
        bullets[i].vx = (int32_t)BULLET_SPD*dx/256*FP;
        bullets[i].vy = (int32_t)BULLET_SPD*dy/256*FP;
        if (score > 0) score--;
        sound_effect_shoot();
        return;
    }
}

static void launch_jet(void) {
    if (plane.active) return;
    int dir = rnd(2) ? 1 : -1;
    plane.active = true;
    plane.vx = dir*PLANE_SPD;
    plane.x = PX2FP((dir==1) ? (PLAY_X-30) : (PLAY_X+PLAY_W+30));
    plane.y = PX2FP(PLAY_Y+12+rnd(25));
    plane.bomb_dropped = 0;
    plane.anim_timer = plane.drop_delay = 0;
    sound_siren_start();
}

// ---------------------------------------------------------------------------
// Update helicópteros
// ---------------------------------------------------------------------------
static void update_helis(void) {
    for (int i=0;i<MAX_HELIS;i++) {
        Heli *h = &helis[i]; if (!h->active) continue;
        h->anim_timer++;
        if (h->anim_timer >= 3) {
            h->anim_timer = 0;
            h->rotor_frame = (h->rotor_frame+1) & 3;
            h->tail_frame  = (h->tail_frame+1) & 1;
        }
        if (!helis_frozen) h->x += (int32_t)h->vx * g_dt_scale;
        int hx = FP2PX(h->x);
        if (hx < PLAY_X-60 || hx > PLAY_X+PLAY_W+60) { h->active=false; continue; }
        if (!tower_active && !wave_done && cannon_alive) {
            if (--h->drop_timer <= 0) {
                /*
                 * Antes, en modo ataque DROP_PROB_ATTACK=1 hacía que CADA
                 * helicóptero soltara un paracaidista con un 100% de
                 * probabilidad cada 0.5-1s -- con varios helis a la vez
                 * eso los tiraba casi todos de golpe. Ahora el intervalo
                 * de comprobación es más corto (más a menudo se decide si
                 * toca soltar), pero la probabilidad real de soltar en
                 * cada comprobación es baja, así que en la práctica caen
                 * uno a uno, repartidos en el tiempo, en vez de en racha.
                 */
                int base_interval = attack_mode
                    ? (TICKS_S/3 + rnd(TICKS_S/3))
                    : (TICKS_S*2 + rnd(TICKS_S*2));
                h->drop_timer = base_interval;
                int prob = attack_mode ? DROP_PROB_ATTACK : DROP_PROB;
                if (rnd(prob) == 0) {
                    spawn_para(FP2PX(h->x), h->y);
                    sound_effect_move();
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Update paracaidistas
// ---------------------------------------------------------------------------
static void update_paras(void) {
    for (int i=0;i<MAX_PARAS;i++) {
        Para *p = &paras[i]; if (!p->active || p->landed) continue;
        p->anim_timer++; if (p->anim_timer >= 6) p->anim_timer = 0;

        if (p->chute_shot) {
            p->vy += PX2FP(1)/3; if (p->vy > PX2FP(6)) p->vy = PX2FP(6);
        } else if (p->chute_open) {
            p->vy = FP*3/4;   // antes 1px/tick (FP) -- ahora 0.75px/tick, más fácil de acertar
        } else {
            p->vy += PX2FP(1)/6; if (p->vy > PX2FP(3)) p->vy = PX2FP(3);
            if (FP2PX(p->y) >= p->chute_open_y) { p->chute_open = true; p->vy = FP*3/4; }
        }
        p->y += p->vy * g_dt_scale / FP;

        int side = (FP2PX(p->x) < TURRET_X) ? 0 : 1;

        if (p->chute_shot && FP2PX(p->y) >= GROUND_Y) {
            int lx = FP2PX(p->x);
            int kill_s = -1;
            if (p->slot>=0 && slot_count[side][p->slot]>0) {
                kill_s = p->slot;
            } else {
                for (int n=0;n<NUM_SLOTS;n++)
                    if (slot_count[side][n]>0 && SLOT_X(side,n)==lx) { kill_s=n; break; }
            }
            if (kill_s>=0) kill_slot(side, kill_s);
            spawn_expl(lx, GROUND_Y, 5, 3, 14);
            p->active = false; continue;
        }

        if (!p->chute_shot && FP2PX(p->y) >= GROUND_Y) {
            p->y = PX2FP(GROUND_Y);
            p->landed = true; p->side = side;
            p->marching = false; p->climbing = false;
            p->march_order = -1;

            int land_x = FP2PX(p->x);
            int sn = find_slot(side, land_x);
            p->slot = sn;
            p->stack = slot_count[side][sn];
            slot_count[side][sn]++;
            p->x = PX2FP(SLOT_X(side,sn));
            p->y = PX2FP(GROUND_Y - p->stack*SOLDIER_H);

            ground_total[side]++;
            sound_effect_move();

            if (ground_total[side] >= TOWER_COUNT && !tower_active) {
                tower_active = true;
                tower_side = side;
                helis_frozen = true;
                sound_effect_lose_point();
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Update torre -- idéntica secuencia de conquista al original, en ticks
// discretos (sin dt, ver cabecera del archivo).
// ---------------------------------------------------------------------------
static void update_tower(void) {
    if (!tower_active) return;

    int dir = (tower_side==0) ? 1 : -1;
    int base_edge = (tower_side==0) ? (TURRET_X - BASE_W/2) : (TURRET_X + BASE_W/2);

    {
        bool needs_assign = false;
        for (int i=0;i<MAX_PARAS;i++) {
            Para *p = &paras[i];
            if (p->active && p->landed && p->side==tower_side && p->march_order<0)
                needs_assign = true;
        }
        if (needs_assign) {
            int idx[TOWER_COUNT]; int cnt=0;
            for (int i=0;i<MAX_PARAS && cnt<TOWER_COUNT;i++) {
                Para *p = &paras[i];
                if (p->active && p->landed && p->side==tower_side && p->march_order<0)
                    idx[cnt++] = i;
            }
            for (int a=0;a<cnt-1;a++)
                for (int b=a+1;b<cnt;b++) {
                    Para *pa=&paras[idx[a]], *pb=&paras[idx[b]];
                    bool swap=false;
                    if (pb->stack > pa->stack) swap=true;
                    else if (pb->stack==pa->stack) {
                        int da=absi(FP2PX(pa->x)-base_edge), db=absi(FP2PX(pb->x)-base_edge);
                        if (db<da) swap=true;
                    }
                    if (swap) { int t=idx[a]; idx[a]=idx[b]; idx[b]=t; }
                }
            for (int a=0;a<cnt;a++) paras[idx[a]].march_order = a;
        }
    }

    bool march_tick = (blink % MARCH_TICKS)==0;
    bool climb_tick = (blink % CLIMB_TICKS)==0;

    Para *sol[TOWER_COUNT]; for (int k=0;k<TOWER_COUNT;k++) sol[k]=NULL;
    for (int i=0;i<MAX_PARAS;i++) {
        Para *p = &paras[i];
        if (p->active && p->landed && p->side==tower_side && p->march_order>=0 && p->march_order<TOWER_COUNT)
            sol[p->march_order] = p;
    }

    int target_x[TOWER_COUNT], target_y[TOWER_COUNT];
    int beside = base_edge - dir*(SOLDIER_H/2+2);
    target_x[0]=base_edge;  target_y[0]=GROUND_Y;
    target_x[1]=base_edge;  target_y[1]=GROUND_Y-SOLDIER_H;
    target_x[2]=beside;     target_y[2]=GROUND_Y;
    target_x[3]=base_edge;  target_y[3]=CANNON_OY;

    for (int k=0;k<TOWER_COUNT;k++) {
        Para *p = sol[k];
        if (!p) continue;

        bool prev_ready = true;
        if (k>0) {
            Para *prev = sol[k-1];
            if (!prev) prev_ready = false;
            else {
                int px=FP2PX(prev->x), py=FP2PX(prev->y);
                prev_ready = (absi(px-target_x[k-1])<=1 && absi(py-target_y[k-1])<=1);
            }
        }
        if (!prev_ready) continue;

        int cx=FP2PX(p->x), cy=FP2PX(p->y);
        int tx=target_x[k], ty=target_y[k];

        bool stacked_descending = (p->stack>0) && (cy<GROUND_Y);
        if (stacked_descending) {
            if (climb_tick) {
                p->y += PX2FP(1);
                if (FP2PX(p->y) >= GROUND_Y) { p->y = PX2FP(GROUND_Y); p->stack = 0; }
            }
            continue;
        }

        bool at_x = absi(cx-tx)<=1;
        bool at_y = absi(cy-ty)<=1;
        p->marching = !at_x;
        p->climbing = at_x && !at_y;

        if (!at_x && march_tick) {
            int step = (tx>cx) ? 1 : -1;
            p->x += PX2FP(step);
            if ((step>0 && FP2PX(p->x)>tx) || (step<0 && FP2PX(p->x)<tx)) p->x = PX2FP(tx);
        } else if (at_x && !at_y && climb_tick) {
            int step = (ty<cy) ? -1 : 1;
            p->y += PX2FP(step);
            if ((step<0 && FP2PX(p->y)<ty) || (step>0 && FP2PX(p->y)>ty)) p->y = PX2FP(ty);
        }

        if (k==3 && absi(FP2PX(p->x)-tx)<=1 && absi(FP2PX(p->y)-ty)<=2) {
            cannon_alive = false; tower_active = false;
            spawn_expl(TURRET_X, CANNON_OY, 20, 5, 35);
            sound_effect_explosion();
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Update avión / bombas / balas
// ---------------------------------------------------------------------------
#define BOMB_DROP_DELAY  30

/*
 * Ticks (nominales) que le quedan a la bomba hasta llegar a la
 * ALTURA DE LA TORRETA (CANNON_OY) -- que es donde check_collisions()
 * de verdad comprueba si la bomba acierta, NO cuando toca el suelo
 * (GROUND_Y, bastante más abajo). Antes se apuntaba a GROUND_Y: la
 * corrección de rumbo (vx) se repartía en el tiempo hasta tocar
 * suelo, así que al llegar a la altura de la torreta -- donde de
 * verdad se juzga el impacto -- la bomba aún no había terminado de
 * centrarse, y a veces fallaba por poco aunque "en teoría" iba a
 * caer justo encima. Apuntando a CANNON_OY, la x ya está corregida
 * del todo para cuando importa.
 *
 * Se usa tanto al lanzarla como, cada tick, para RECALCULAR la vx
 * necesaria -- así el impacto siempre cae justo en TURRET_X sin
 * importar que el dt real fluctúe de un tick a otro (antes se
 * calculaba una vez al lanzar asumiendo un tick nominal fijo, y con
 * el dt real variable el número de ticks reales hasta llegar no
 * coincidía con el planeado, así que la bomba caía a un lado u otro
 * de la torreta en vez de encima).
 */
static int bomb_ticks_to_ground(int32_t y, int32_t vy, int32_t ay) {
    int32_t sim_vy=vy, sim_y=y;
    for (int t=1; t<=500; t++) {
        sim_vy += ay; sim_y += sim_vy;
        if (FP2PX(sim_y) >= CANNON_OY) return t;
    }
    return 500;
}

static void update_plane(void) {
    if (!plane.active) return;
    plane.anim_timer++; if (plane.anim_timer>=3) plane.anim_timer=0;
    plane.x += (int32_t)plane.vx * g_dt_scale;

    if (plane.bomb_dropped == 0) {
        if (++plane.drop_delay >= BOMB_DROP_DELAY) {
            plane.bomb_dropped = 1;
            for (int bi=0;bi<MAX_BOMBS;bi++) {
                if (bombs[bi].active) continue;
                int32_t bx = plane.x;
                int32_t by = plane.y + PX2FP(6);
                /*
                 * Caída lenta a propósito (antes tardaba ~1.8s en tocar
                 * el suelo, sin apenas margen para dispararle; ahora
                 * tarda ~3s) para que dé tiempo real a acertarla.
                 */
                int32_t vy0 = 24, ay = 3;
                int tof = bomb_ticks_to_ground(by, vy0, ay);
                int32_t vx = (PX2FP(TURRET_X) - bx) / tof;
                bombs[bi].active=true;
                bombs[bi].x=bx; bombs[bi].y=by;
                bombs[bi].vx=vx; bombs[bi].vy=vy0; bombs[bi].ay=ay;
                sound_play_tone(220, 60);
                break;
            }
        }
    }
    int px = FP2PX(plane.x);
    if (px<PLAY_X-50 || px>PLAY_X+PLAY_W+50) { plane.active=false; sound_siren_stop(); }
}

static void update_bombs(void) {
    for (int i=0;i<MAX_BOMBS;i++) {
        Bomb *b = &bombs[i]; if (!b->active) continue;

        /*
         * Solo recalculamos vx (homing hacia TURRET_X) mientras la
         * bomba TODAVÍA no ha llegado a la altura de la torreta.
         * Una vez pasada esa altura (falló o ya no hay nada que
         * comprobar), "rem" se quedaría clavado en 1 tick para
         * siempre y cada frame intentaría cerrar TODA la distancia
         * restante de golpe -- un bandazo visible en la caída en
         * vez de seguir recta hasta salir de pantalla.
         */
        if (FP2PX(b->y) < CANNON_OY) {
            int rem = bomb_ticks_to_ground(b->y, b->vy, b->ay);
            if (rem < 1) rem = 1;
            b->vx = (PX2FP(TURRET_X) - b->x) / rem;
        }

        b->vy += b->ay;
        b->x  += b->vx * g_dt_scale / FP;
        b->y  += b->vy * g_dt_scale / FP;
        if (FP2PX(b->y) > PLAY_Y+PLAY_H ||
            FP2PX(b->x) < PLAY_X || FP2PX(b->x) > PLAY_X+PLAY_W) {
            b->active = false; score += PTS_BOMB_DODGE;
        }
    }
}

static void update_bullets(void) {
    for (int i=0;i<MAX_BULLETS;i++) {
        Bullet *b = &bullets[i]; if (!b->active) continue;
        b->x += b->vx * g_dt_scale / FP;
        b->y += b->vy * g_dt_scale / FP;
        int bx=FP2PX(b->x), by=FP2PX(b->y);
        if (bx<PLAY_X || bx>PLAY_X+PLAY_W || by<PLAY_Y || by>PLAY_Y+PLAY_H)
            b->active = false;
    }
}

// ---------------------------------------------------------------------------
// Colisiones -- idéntica lógica al original, radios reescalados.
// ---------------------------------------------------------------------------
static void check_collisions(void) {
    for (int bi=0;bi<MAX_BULLETS;bi++) {
        Bullet *bu = &bullets[bi]; if (!bu->active) continue;
        int bx=FP2PX(bu->x), by=FP2PX(bu->y);

        for (int i=0;i<MAX_HELIS;i++) {
            if (!helis[i].active) continue;
            if (chit(bx,by,3, FP2PX(helis[i].x),helis[i].y,10)) {
                bu->active = helis[i].active = false;
                spawn_expl(FP2PX(helis[i].x),helis[i].y,10,4,20);
                score += PTS_HELI; sound_effect_explosion(); break;
            }
        }
        if (!bu->active) continue;

        for (int i=0;i<MAX_PARAS;i++) {
            Para *p = &paras[i];
            if (!p->active || p->landed || p->chute_shot) continue;
            int px=FP2PX(p->x), py=FP2PX(p->y);
            /*
             * Dos hitboxes bien separadas (antes se solapaban casi del
             * todo y en la práctica siempre "ganaba" el dosel):
             *  - Dosel: solo existe con chute_open, centrado en el propio
             *    dosel (py-10, arriba del cuerpo) -- acertarlo solo corta
             *    las cuerdas (chute_shot=true), no mata.
             *  - Soldado: centrado en el cuerpo que cuelga (py+SOLDIER_H/2,
             *    la posición real donde se dibuja con draw_soldier_at),
             *    tanto con el dosel abierto como en caída libre -- acertarlo
             *    mata directamente.
             */
            if (p->chute_open && chit(bx,by,3, px,py-10,8)) {
                bu->active=false; p->chute_open=false; p->chute_shot=true;
                p->vy=PX2FP(2); score+=PTS_PARA_CHUTE_HIT;
                sound_effect_explosion(); break;
            }
            if (chit(bx,by,3, px,py+SOLDIER_H/2, SOLDIER_H/2+3)) {
                bu->active=p->active=false;
                spawn_expl(px,py+SOLDIER_H/2,5,3,14);
                score+=PTS_PARA_AIR; sound_effect_explosion(); break;
            }
        }
        if (!bu->active) continue;

        for (int i=0;i<MAX_PARAS;i++) {
            Para *p = &paras[i]; if (!p->active || !p->landed) continue;
            int px = FP2PX(p->x);
            int top_y = GROUND_Y - slot_count[p->side][p->slot]*SOLDIER_H;
            if (bx>=px-4 && bx<=px+4 && by>=top_y && by<=GROUND_Y) {
                bu->active=false; kill_slot(p->side,p->slot); break;
            }
        }
        if (!bu->active) continue;

        if (plane.active) {
            if (chit(bx,by,3,FP2PX(plane.x),FP2PX(plane.y),9)) {
                bu->active = plane.active = false;
                spawn_expl(FP2PX(plane.x),FP2PX(plane.y),14,5,26);
                score += PTS_PLANE; sound_effect_explosion(); sound_siren_stop();
            }
        }
        if (!bu->active) continue;

        for (int i=0;i<MAX_BOMBS;i++) {
            if (!bombs[i].active) continue;
            int ox=FP2PX(bombs[i].x), oy=FP2PX(bombs[i].y);
            if (absi(ox-TURRET_X) < 30) continue; // zona de seguridad junto a la torreta
            if (chit(bx,by,3,ox,oy,3)) {
                bu->active = bombs[i].active = false;
                spawn_expl(ox,oy,6,3,16);
                score+=PTS_PARA_AIR; sound_effect_explosion(); break;
            }
        }
    }

    if (!cannon_alive || cannon_inv>0) return;

    for (int i=0;i<MAX_BOMBS;i++) {
        Bomb *b = &bombs[i]; if (!b->active) continue;
        int bx = FP2PX(b->x);
        int by = FP2PX(b->y);
    
    // Si la bomba baja a la altura de la torreta (o más) y está alineada horizontalmente con ella
    if (by >= CANNON_OY && absi(bx - TURRET_X) < (BASE_W / 2 + 4)) {
        b->active = cannon_alive = false;
        spawn_expl(CANNON_OX, CANNON_OY, 18, 5, 32);
        sound_effect_explosion(); 
        return;
    }
    }


    for (int i=0;i<MAX_HELIS;i++) {
        if (!helis[i].active) continue;
        if (chit(FP2PX(helis[i].x),helis[i].y,10,CANNON_OX,CANNON_OY,TURRET_R)) {
            helis[i].active = cannon_alive = false;
            spawn_expl(CANNON_OX,CANNON_OY,14,5,28);
            sound_effect_explosion(); return;
        }
    }
}

static bool wave_clear(void) {
    if (wave_helis_spawned < wave_heli_budget) return false;
    for (int i=0;i<MAX_HELIS;i++) if (helis[i].active) return false;
    for (int i=0;i<MAX_PARAS;i++) if (paras[i].active && !paras[i].landed) return false;
    return true;
}

// ---------------------------------------------------------------------------
// DRAW: primitivas de entidades
// ---------------------------------------------------------------------------
static void draw_turret(void) {
    if (!cannon_alive) return;
    if (cannon_inv>0 && (cannon_inv/4)%2==0) return;

    int tx=TURRET_X, ty=TURRET_Y;
    int base_top = ty - BASE_H;

    renderer_fill_rect(tx-BASE_W/2, base_top, BASE_W, BASE_H, COLOR_TURRET_BASE);
    fill_top_ellipse(tx, base_top, BODY_RX, BODY_RY, COLOR_TURRET_BASE);
    renderer_fill_rect(tx-1, base_top-BODY_RY+2, 2, 2, COLOR_BLACK);

    int dx2=can_dx(cannon_angle_deg), dy2=can_dy(cannon_angle_deg);
    int tip_x = CANNON_OX + dx2*CANNON_LEN/256;
    int tip_y = CANNON_OY + dy2*CANNON_LEN/256;
    for (int t=-CANNON_W; t<=CANNON_W; t++) {
        int ox = (int)(t*(-dy2)/256);
        int oy = (int)(t*( dx2)/256);
        line(CANNON_OX+ox, CANNON_OY+oy, tip_x+ox, tip_y+oy, COLOR_TURRET_GUN);
    }
}

#define HELI_RX 11
#define HELI_RY 5

static void draw_heli(const Heli *h) {
    if (!h->active) return;
    int cx=FP2PX(h->x), cy=h->y;
    int d = (h->vx>=0) ? 1 : -1;

    fill_ellipse(cx, cy, HELI_RX, HELI_RY, COLOR_HELI_BODY);
    // hueco de cabina (marca la dirección de la nariz)
    renderer_fill_rect(cx + d*(HELI_RX-4), cy-1, 3, 3, COLOR_BLACK);

    int tail_len = 8;
    int tail_x = (d>0) ? (cx-HELI_RX-tail_len) : (cx+HELI_RX);
    renderer_fill_rect(tail_x, cy-1, tail_len, 2, COLOR_HELI_ACCENT);
    int mast_x = (d>0) ? (tail_x-1) : (tail_x+tail_len);

    if ((h->tail_frame & 1)==0)
        renderer_fill_rect(mast_x-3, cy-1, 7, 1, COLOR_HELI_ACCENT);
    else
        renderer_fill_rect(mast_x, cy-4, 1, 7, COLOR_HELI_ACCENT);

    int mx = cx + d;
    renderer_fill_rect(mx-1, cy-HELI_RY-3, 2, 3, COLOR_HELI_ACCENT);
    static const int8_t RL[4] = {10,7,4,1};
    int rl = RL[h->rotor_frame & 3];
    renderer_fill_rect(mx-rl, cy-HELI_RY-4, 2*rl+1, 1, COLOR_HELI_ACCENT);

    renderer_fill_rect(cx-HELI_RX+2, cy+HELI_RY,   2, 4, COLOR_HELI_ACCENT);
    renderer_fill_rect(cx+HELI_RX-4, cy+HELI_RY,   2, 4, COLOR_HELI_ACCENT);
    renderer_fill_rect(cx-HELI_RX,   cy+HELI_RY+4, 2*HELI_RX, 2, COLOR_HELI_ACCENT);
}

// Soldado, pies en py. walking anima las piernas (marcha/escalada).
static void draw_soldier_at(int cx, int py, bool walking, uint16_t color) {
    if (walking) {
        int phase = (blink/8) & 1;
        int off = (phase==0) ? 2 : -2;
        line(cx-2, py-6, cx-2+off, py, color);
        line(cx+2, py-6, cx+2-off, py, color);
    } else {
        renderer_fill_rect(cx-3, py-6, 2, 6, color);
        renderer_fill_rect(cx+1, py-6, 2, 6, color);
    }
    renderer_fill_rect(cx-2, py-11, 4, 6, color);              // tronco
    line(cx-4, py-10, cx+4, py-10, color);                     // brazos
    renderer_fill_rect(cx-2, py-15, 4, 4, color);               // cabeza
}

static void draw_para(const Para *p, uint16_t color) {
    if (!p->active) return;

    if (p->landed) {
        int px=FP2PX(p->x), py=FP2PX(p->y);
        bool walk = (tower_active && p->side==tower_side && (p->marching||p->climbing));
        draw_soldier_at(px, py, walk, color);
        return;
    }

    int px=FP2PX(p->x), py=FP2PX(p->y);

    if (p->chute_shot) {
        draw_soldier_at(px, py+SOLDIER_H, false, color);
        return;
    }
    if (p->chute_open) {
        int rw=9, rh=7;
        int db = py - rh;
        draw_chute_filled(px, db, rw, rh, COLOR_CHUTE);
        line(px-rw, db, px, py, COLOR_CHUTE);
        line(px+rw, db, px, py, COLOR_CHUTE);
        line(px-2, py, px+2, py, COLOR_CHUTE);
        draw_soldier_at(px, py+SOLDIER_H, false, color);
        return;
    }
    draw_soldier_at(px, py+SOLDIER_H, false, color);
}

static void draw_plane_sprite(uint16_t color) {
    if (!plane.active) return;
    int x=FP2PX(plane.x), y=FP2PX(plane.y), d=(plane.vx>0)?1:-1;
    renderer_fill_rect(x-11, y-1, 22, 2, color);
    line(x+d*11, y, x+d*16, y-1, color);
    line(x+d*11, y, x+d*16, y+1, color);
    line(x-d*4, y-1, x-d*10, y-5, color);
    line(x-d*4, y+1, x-d*10, y+5, color);
    line(x-d*10, y-5, x-d*10, y+5, color);
}

static void draw_bomb_sprite(const Bomb *b, uint16_t color) {
    if (!b->active) return;
    int bx=FP2PX(b->x), by=FP2PX(b->y);
    renderer_fill_rect(bx-2, by-3, 4, 5, color);
    renderer_fill_rect(bx-1, by-4, 2, 2, color);
}

// ---------------------------------------------------------------------------
// Render incremental -- borrado por caja delimitadora, un flush por
// entidad, mismo patrón que asteroids.c.
// ---------------------------------------------------------------------------
typedef struct { int x,y; bool active; } PrevPos;

static PrevPos prev_heli[MAX_HELIS];
static PrevPos prev_para[MAX_PARAS];
static bool    prev_para_landed[MAX_PARAS]; // tamaño de caja usado en el último borrado de cada para
static PrevPos prev_bomb[MAX_BOMBS];
static PrevPos prev_plane_pos = { .active=false };
static PrevPos prev_turret = { .active=false };
static bool    prev_tower_region_active = false;
static bool    field_needs_redraw = true;
static int     prev_score = -1, prev_wave = -1;
static char    prev_center_msg[24] = "";
static char    prev_bottom_msg[40] = "";

/*
 * HELI_BOX_HW tiene que cubrir no solo el fuselaje (HELI_RX) sino la
 * cola completa: tail_len(8) + 1px de separación del mástil + el brazo
 * de la hélice de cola (3px a cada lado) = HELI_RX+8+1+3 = HELI_RX+12
 * en el peor caso. Antes era solo HELI_RX+3 -- por eso la hélice de
 * cola se quedaba sin borrar y dejaba un rastro al moverse.
 */
#define HELI_BOX_HW  (HELI_RX+14)
#define HELI_BOX_UP  (HELI_RY+8)
#define HELI_BOX_DN  (HELI_RY+8)
#define PARA_BOX_HW  12
#define PARA_BOX_UP  28
#define PARA_BOX_DN  18
/*
 * Caja reducida para el soldado YA ATERRIZADO (draw_soldier_at(px,py,...)
 * dibujado directamente en py, sin el offset +SOLDIER_H del paracaidista
 * en el aire): el sprite ocupa solo [py-15, py]. La caja grande de arriba
 * (pensada para el paracaídas abierto, que sobresale ~14px por encima de
 * py y deja el cuerpo ~15px por debajo) se queda muy holgada para este
 * caso -- y como los soldados apilados en una ranura están separados
 * exactamente SOLDIER_H=15px entre sí, un borrado de 28px hacia arriba
 * se comía 13px del soldado de la ranura de encima en cada tick (se
 * borraba y no se volvía a dibujar hasta el turno de ESE soldado en el
 * bucle, de ahí el parpadeo al estar juntos).
 */
#define PARA_LAND_BOX_UP  16
#define PARA_LAND_BOX_DN   2
#define BOMB_BOX_R   6
#define PLANE_BOX_HW 20
#define PLANE_BOX_HH 10
#define TURRET_BOX_HW  (BASE_W/2 + CANNON_LEN + 3)
#define TURRET_BOX_TOP (CANNON_OY - BODY_RY - CANNON_LEN - 2)
#define TURRET_BOX_H   (TURRET_Y - TURRET_BOX_TOP)

static void erase_box(int x, int y, int w, int h) {
    if (x<0) { w+=x; x=0; }
    if (y<0) { h+=y; y=0; }
    if (w<=0 || h<=0) return;
    renderer_fill_rect(x, y, w, h, COLOR_BLACK);
}

static void reset_render_trace(void) {
    for (int i=0;i<MAX_HELIS;i++) prev_heli[i].active=false;
    for (int i=0;i<MAX_PARAS;i++) { prev_para[i].active=false; prev_para_landed[i]=false; }
    for (int i=0;i<MAX_BOMBS;i++) prev_bomb[i].active=false;
    prev_plane_pos.active=false;
    prev_turret.active=false;
    prev_tower_region_active=false;
    prev_score=prev_wave=-1;
    prev_center_msg[0]='\0';
    prev_bottom_msg[0]='\0';
}

static void draw_field_static(void) {
    renderer_clear(COLOR_BLACK);
    renderer_fill_rect(PLAY_X, GROUND_Y, PLAY_W, 2, COLOR_WHITE);
    reset_render_trace();
    field_needs_redraw = false;
    renderer_flush();
}

static void draw_turret_if_changed(void) {
    bool show = cannon_alive;
    if (!show && !prev_turret.active) return;
    // Se borra/redibuja siempre que hay algo que mostrar: el ángulo, el
    // parpadeo de invulnerabilidad o la explosión final pueden cambiar
    // cada tick, y el área es pequeña -- barato de refrescar entero.
    erase_box(TURRET_X-TURRET_BOX_HW, TURRET_BOX_TOP, 2*TURRET_BOX_HW, TURRET_BOX_H);
    if (show) draw_turret();
    prev_turret.active = show;
    renderer_flush();
}

static void draw_helis_if_moved(void) {
    for (int i=0;i<MAX_HELIS;i++) {
        const Heli *h = &helis[i];
        bool show = h->active;
        if (!show && !prev_heli[i].active) continue;
        int cx=FP2PX(h->x), cy=h->y;
        if (prev_heli[i].active)
            erase_box(prev_heli[i].x-HELI_BOX_HW, prev_heli[i].y-HELI_BOX_UP,
                      2*HELI_BOX_HW, HELI_BOX_UP+HELI_BOX_DN);
        if (show) draw_heli(h);
        prev_heli[i].x=cx; prev_heli[i].y=cy; prev_heli[i].active=show;
        renderer_flush();
    }
}

/* ¿Este paracaidista está aterrizado y en el lado que está marchando
 * ahora mismo hacia la torreta? Esos los gestiona draw_tower_region()
 * en un único borrado+redibujado atómico (ver más abajo); el resto
 * sigue el patrón normal de un flush por entidad. */
static bool para_in_active_tower(const Para *p) {
    return tower_active && p->active && p->landed && p->side==tower_side;
}

static void draw_paras_if_moved(void) {
    for (int i=0;i<MAX_PARAS;i++) {
        const Para *p = &paras[i];
        bool show = p->active;
        if (!show && !prev_para[i].active) continue;
        int cx=FP2PX(p->x), cy=FP2PX(p->y);

        if (para_in_active_tower(p)) {
            /* No se toca la pantalla aquí -- solo se mantiene al día su
             * posición "previa" para que, cuando deje de estar en la
             * torre (aterriza otro/se destruye la torre/etc.), el borrado
             * individual de más abajo parta de la posición real y no de
             * una desfasada de antes de empezar a marchar. */
            prev_para[i].x=cx; prev_para[i].y=cy; prev_para[i].active=show;
            prev_para_landed[i] = true;
            continue;
        }

        if (prev_para[i].active) {
            /* La caja de borrado depende de cómo se dibujó la ÚLTIMA vez
             * (aterrizado o cayendo), no de cómo esté ahora: si acaba de
             * aterrizar en este mismo tick hay que borrar todavía la
             * silueta grande del paracaídas, no la pequeña del soldado. */
            int up = prev_para_landed[i] ? PARA_LAND_BOX_UP : PARA_BOX_UP;
            int dn = prev_para_landed[i] ? PARA_LAND_BOX_DN : PARA_BOX_DN;
            erase_box(prev_para[i].x-PARA_BOX_HW, prev_para[i].y-up,
                      2*PARA_BOX_HW, up+dn);
        }
        if (show) draw_para(p, COLOR_SOLDIER);
        prev_para[i].x=cx; prev_para[i].y=cy; prev_para[i].active=show;
        prev_para_landed[i] = p->landed;
        renderer_flush();
    }
}

/*
 * Torre/escalera: mientras tower_active, TODOS los soldados aterrizados
 * del lado que ataca (los que marchan/escalan Y los que aún esperan su
 * turno en su ranura original) se borran y redibujan de una sola vez
 * sobre la franja completa de ese lado, en un único flush -- en vez de
 * objeto por objeto. Están a menudo a menos de 15-20px unos de otros
 * (justo su propia altura), así que con cajas individuales el borrado
 * de uno se comía los píxeles recién dibujados de otro en el mismo
 * frame. Al ser un único borrado+redibujado atómico, eso ya no puede
 * pasar.
 *
 * OJO -- esta franja llega en horizontal justo hasta TURRET_X (x0/x1),
 * pero el sprite de la torreta (ver TURRET_BOX_HW) sobresale ~31px más
 * allá de TURRET_X hacia ESTE mismo lado -- es decir, la mitad del
 * dibujo de la torreta cae dentro de esta franja. El soldado que
 * escala (target_x llega a base_edge = TURRET_X∓12, y hasta CANNON_OY
 * en el peldaño más alto) queda literalmente pegado a la base. El
 * erase_box de aquí arriba borra esa mitad de la torreta, así que hay
 * que volver a dibujarla aquí mismo, en el mismo flush -- si se deja
 * para draw_turret_if_changed() por separado, lo que se borre/dibuje
 * ahí "gana" sobre lo que se acaba de pintar aquí (o al revés, según
 * el orden de llamada) y uno de los dos parpadea cada tick.
 */
static void draw_tower_region(void) {
    if (!tower_active) { prev_tower_region_active = false; return; }

    int side = tower_side;
    int x0 = (side==0) ? PLAY_X : TURRET_X;
    int x1 = (side==0) ? TURRET_X : (PLAY_X+PLAY_W);
    int top = CANNON_OY - SOLDIER_H - 4;   // cubre al soldado que sube hasta el cañón
    int bottom = GROUND_Y + 1;

    erase_box(x0, top, x1-x0, bottom-top);
    if (cannon_alive) draw_turret();   // repone la mitad de la torreta que cae en esta franja
    for (int i=0;i<MAX_PARAS;i++) {
        const Para *p = &paras[i];
        if (!para_in_active_tower(p)) continue;
        draw_para(p, COLOR_SOLDIER);
    }
    prev_tower_region_active = true;
    renderer_flush();
}

static void draw_bombs_if_moved(void) {
    for (int i=0;i<MAX_BOMBS;i++) {
        const Bomb *b = &bombs[i];
        bool show = b->active;
        if (!show && !prev_bomb[i].active) continue;
        int bx=FP2PX(b->x), by=FP2PX(b->y);
        if (prev_bomb[i].active)
            erase_box(prev_bomb[i].x-BOMB_BOX_R, prev_bomb[i].y-BOMB_BOX_R, 2*BOMB_BOX_R, 2*BOMB_BOX_R);
        if (show) draw_bomb_sprite(b, COLOR_BOMB);
        prev_bomb[i].x=bx; prev_bomb[i].y=by; prev_bomb[i].active=show;
        renderer_flush();
    }
}

static void draw_plane_if_moved(void) {
    bool show = plane.active;
    if (!show && !prev_plane_pos.active) return;
    int x=FP2PX(plane.x), y=FP2PX(plane.y);
    if (prev_plane_pos.active)
        erase_box(prev_plane_pos.x-PLANE_BOX_HW, prev_plane_pos.y-PLANE_BOX_HH,
                  2*PLANE_BOX_HW, 2*PLANE_BOX_HH);
    if (show) draw_plane_sprite(COLOR_JET);
    prev_plane_pos.x=x; prev_plane_pos.y=y; prev_plane_pos.active=show;
    renderer_flush();
}

static int prev_bullet_x[MAX_BULLETS], prev_bullet_y[MAX_BULLETS];
static bool prev_bullet_active[MAX_BULLETS];
static int prev_part_x[MAX_PARTICLES], prev_part_y[MAX_PARTICLES];
static bool prev_part_active[MAX_PARTICLES];

static void draw_bullets_if_moved(void) {
    bool any=false;
    for (int i=0;i<MAX_BULLETS;i++) {
        int x=FP2PX(bullets[i].x)-1, y=FP2PX(bullets[i].y)-1;
        bool show = bullets[i].active;
        if (!show && !prev_bullet_active[i]) continue;
        if (prev_bullet_active[i]) renderer_fill_rect(prev_bullet_x[i],prev_bullet_y[i],3,3,COLOR_BLACK);
        if (show) renderer_fill_rect(x,y,3,3,COLOR_BULLET);
        prev_bullet_x[i]=x; prev_bullet_y[i]=y; prev_bullet_active[i]=show;
        any=true;
    }
    if (any) renderer_flush();
}

static void draw_particles_if_moved(void) {
    bool any=false;
    for (int i=0;i<MAX_PARTICLES;i++) {
        int x=FP2PX(particles[i].x), y=FP2PX(particles[i].y);
        bool show = particles[i].active &&
                    x>=PLAY_X && x<PLAY_X+PLAY_W && y>=PLAY_Y && y<PLAY_Y+PLAY_H;
        if (!show && !prev_part_active[i]) continue;
        if (prev_part_active[i]) renderer_fill_rect(prev_part_x[i],prev_part_y[i],2,2,COLOR_BLACK);
        if (show) renderer_fill_rect(x,y,2,2,COLOR_PART);
        prev_part_x[i]=x; prev_part_y[i]=y; prev_part_active[i]=show;
        any=true;
    }
    if (any) renderer_flush();
}

// ---------------------------------------------------------------------------
// HUD
// ---------------------------------------------------------------------------
static void draw_hud_if_changed(void) {
    char buf[16];
    bool changed=false;

    if (score != prev_score) {
        renderer_fill_rect(PLAY_X+2, PLAY_Y+2, 90, 12, COLOR_BLACK);
        snprintf(buf, sizeof(buf), "%06d", score);
        st7789_draw_text(PLAY_X+2, PLAY_Y+2, buf, COLOR_WHITE, COLOR_BLACK, 2);
        prev_score = score; changed = true;
    }
    if (wave != prev_wave) {
        renderer_fill_rect(PLAY_X+PLAY_W-60, PLAY_Y+2, 58, 12, COLOR_BLACK);
        snprintf(buf, sizeof(buf), "OLA %d", wave);
        int x = PLAY_X+PLAY_W-2-(int)st7789_text_width(buf,1);
        st7789_draw_text(x, PLAY_Y+2, buf, COLOR_WHITE, COLOR_BLACK, 1);
        prev_wave = wave; changed = true;
    }
    if (changed) renderer_flush();
}

static int centered_x(const char *text, int scale) {
    int w = (int)st7789_text_width(text, (uint8_t)scale);
    int x = (TFT_WIDTH - w) / 2;
    return (x<0) ? 0 : x;
}

static void update_center_message(const char *target, uint16_t color, int scale) {
    if (strcmp(target, prev_center_msg)==0) return;
    renderer_fill_rect(0, CY-16, TFT_WIDTH, 32, COLOR_BLACK);
    if (target[0]) st7789_draw_text(centered_x(target,scale), CY-10, target, color, COLOR_BLACK, scale);
    strncpy(prev_center_msg, target, sizeof(prev_center_msg)-1);
    prev_center_msg[sizeof(prev_center_msg)-1]='\0';
    renderer_flush();
}

static void update_bottom_message(const char *target, int scale) {
    if (strcmp(target, prev_bottom_msg)==0) return;

    /*
     * Caja de borrado/dibujo dimensionada según "scale" -- antes era
     * un hueco fijo de 16px pensado solo para escala 1; con textos a
     * escala 2 (el doble de alto) se salían por abajo o quedaban mal
     * encajados. Con esto cabe cualquier escala razonable y siempre
     * queda a la misma distancia (2px) del borde inferior del área
     * de juego.
     */
    int text_h = 8*scale;
    int box_h = text_h + 4;
    int box_y = PLAY_Y+PLAY_H-2-box_h;

    renderer_fill_rect(0, box_y, TFT_WIDTH, box_h, COLOR_BLACK);
    if (target[0]) st7789_draw_text(centered_x(target,scale), box_y+2, target, COLOR_WHITE, COLOR_BLACK, scale);
    strncpy(prev_bottom_msg, target, sizeof(prev_bottom_msg)-1);
    prev_bottom_msg[sizeof(prev_bottom_msg)-1]='\0';
    renderer_flush();
}

static void draw_playing_frame(void) {
    if (field_needs_redraw) draw_field_static();

    /*
     * draw_turret_if_changed() va ANTES que draw_tower_region(): la caja
     * de borrado de la torreta invade la franja de marcha/escalada (ver
     * comentario en draw_tower_region), así que quien se dibuje último
     * sobre ese solape es el que se ve bien. Poniendo la torreta primero,
     * draw_tower_region() (que ya redibuja la torreta por su cuenta en
     * esa zona) tiene siempre la última palabra sobre los soldados.
     */
    draw_turret_if_changed();
    draw_paras_if_moved();
    draw_tower_region();
    draw_helis_if_moved();
    draw_plane_if_moved();
    draw_bombs_if_moved();
    draw_bullets_if_moved();
    draw_particles_if_moved();
    draw_hud_if_changed();

    bool bon = (blink/20)%2==0;

    const char *center = "";
    int center_scale = 2;
    if (state==PT_DEAD && bon) { center="TORRETA DESTRUIDA"; }
    update_center_message(center, COLOR_YELLOW, center_scale);

    const char *bottom = "";
    if (demo_mode && bon) bottom = "DEMO - PULSA PARA JUGAR";
    update_bottom_message(bottom, 2);
}

// ---------------------------------------------------------------------------
// Pantallas estáticas
// ---------------------------------------------------------------------------

/*
 * Torreta y paracaidista decorativos para la pantalla de título --
 * mismas primitivas que draw_turret()/draw_para() (fill_top_ellipse,
 * draw_chute_filled, draw_soldier_at, line con can_dx/can_dy), pero
 * en tamaño propio y sin depender del estado de partida
 * (cannon_alive/cannon_angle_deg/Para), para poder colocarlos donde
 * convenga en una pantalla estática. El dibujo anterior (un par de
 * rectángulos sueltos) no llegaba a parecerse a una torreta ni a un
 * paracaidista -- esto reutiliza la forma real del juego, solo que
 * un poco más grande para que se lea bien como decoración.
 */
#define TITLE_TURRET_BASE_W   26
#define TITLE_TURRET_BASE_H   20
#define TITLE_TURRET_RX       9
#define TITLE_TURRET_RY       14
#define TITLE_TURRET_GUN_LEN  18
#define TITLE_TURRET_GUN_W    2
#define TITLE_TURRET_GUN_ANGLE (-18)   // grados, misma convención que cannon_angle_deg

static void draw_title_turret(int tx, int ground_y) {
    int base_top = ground_y - TITLE_TURRET_BASE_H;

    renderer_fill_rect(
        tx - TITLE_TURRET_BASE_W/2, base_top,
        TITLE_TURRET_BASE_W, TITLE_TURRET_BASE_H,
        COLOR_TURRET_BASE
    );
    fill_top_ellipse(tx, base_top, TITLE_TURRET_RX, TITLE_TURRET_RY, COLOR_TURRET_BASE);
    renderer_fill_rect(tx-1, base_top-TITLE_TURRET_RY+2, 2, 2, COLOR_BLACK);

    int ox = tx;
    int oy = base_top - TITLE_TURRET_RY/2;
    int dx = can_dx(TITLE_TURRET_GUN_ANGLE);
    int dy = can_dy(TITLE_TURRET_GUN_ANGLE);
    int tip_x = ox + dx*TITLE_TURRET_GUN_LEN/256;
    int tip_y = oy + dy*TITLE_TURRET_GUN_LEN/256;

    for (int t=-TITLE_TURRET_GUN_W; t<=TITLE_TURRET_GUN_W; t++) {
        int ex = (int)(t*(-dy)/256);
        int ey = (int)(t*( dx)/256);
        line(ox+ex, oy+ey, tip_x+ex, tip_y+ey, COLOR_TURRET_GUN);
    }
}

#define TITLE_PARA_RW 10
#define TITLE_PARA_RH 8

static void draw_title_para(int px, int hang_y) {
    int db = hang_y - TITLE_PARA_RH;
    draw_chute_filled(px, db, TITLE_PARA_RW, TITLE_PARA_RH, COLOR_CHUTE);
    line(px-TITLE_PARA_RW, db, px, hang_y, COLOR_CHUTE);
    line(px+TITLE_PARA_RW, db, px, hang_y, COLOR_CHUTE);
    line(px-2, hang_y, px+2, hang_y, COLOR_CHUTE);
    draw_soldier_at(px, hang_y+SOLDIER_H, false, COLOR_SOLDIER);
}

void draw_title_screen(void) {
    renderer_clear(COLOR_BLACK);

    // --- Título, centrado de verdad (centered_x mide el ancho real
    // de la fuente en vez de asumirlo a mano) ---
    static const char *title = "PARATROOPER";
    st7789_draw_text(centered_x(title, 3), 10, title, COLOR_WHITE, COLOR_BLACK, 3);

    renderer_fill_rect(30, 38, TFT_WIDTH-60, 2, COLOR_TURRET_GUN);

    // --- Escena decorativa: dos paracaidistas bajando hacia la
    // torreta central, con las mismas formas que se ven en partida ---
    draw_title_para(66, 58);
    draw_title_turret(CX, 108);
    draw_title_para(254, 58);

    // --- Instrucciones -- centradas, con hueco de sobra antes del
    // "PULSA A/B PARA JUGAR" parpadeante que pinta pt_tick() más abajo ---
    static const char *line1 = "GIRA Y DISPARA";
    static const char *line2 = "DEFIENDE LA TORRETA";

    st7789_draw_text(centered_x(line1, 2), 155, line1, COLOR_YELLOW, COLOR_BLACK, 2);
    st7789_draw_text(centered_x(line2, 2), 183, line2, COLOR_CYAN, COLOR_BLACK, 2);

    renderer_flush();
}

static void draw_gameover_screen(void) {
    renderer_clear(COLOR_BLACK);
    st7789_draw_text(centered_x("GAME OVER",3), CY-40, "GAME OVER", COLOR_RED, COLOR_BLACK, 3);
    char b[24]; snprintf(b,sizeof(b),"PUNTOS: %d", score);
    st7789_draw_text(centered_x(b,2), CY+4, b, COLOR_WHITE, COLOR_BLACK, 2);
    prev_center_msg[0]='\0';
    prev_bottom_msg[0]='\0';
    renderer_flush();
}

static void draw_scores_screen(void) {
    renderer_clear(COLOR_BLACK);
    highscores_draw(PT_GAME_ID, "PARATROOPER", 20);
    prev_center_msg[0]='\0';
    prev_bottom_msg[0]='\0';
    renderer_flush();
}

// ---------------------------------------------------------------------------
// IA demo -- idéntica al original.
// ---------------------------------------------------------------------------
static int demo_tgt = ANGLE_UP_DEG, demo_fcd = 0;

static int aim_at(int tx, int ty) {
    int ddx=tx-CANNON_OX, ddy=CANNON_OY-ty;
    if (ddy<=0) return ANGLE_UP_DEG;
    int a = ddx*75/(absi(ddx)+ddy/2+1);
    if (a<ANGLE_MIN_DEG) a=ANGLE_MIN_DEG;
    if (a>ANGLE_MAX_DEG) a=ANGLE_MAX_DEG;
    return a;
}

static void demo_ai(void) {
    int best_ang=ANGLE_UP_DEG, best_d=0x7fffffff;
    for (int i=0;i<MAX_HELIS;i++) {
        if (!helis[i].active) continue;
        int hx=FP2PX(helis[i].x), hy=helis[i].y;
        int dx=hx-CANNON_OX, dy=hy-CANNON_OY;
        int d2=dx*dx+dy*dy;
        if (d2<best_d) { best_d=d2; best_ang=aim_at(hx,hy); }
    }
    for (int i=0;i<MAX_PARAS;i++) {
        Para *p=&paras[i]; if (!p->active||p->landed) continue;
        int px=FP2PX(p->x), py=FP2PX(p->y);
        int dx=px-CANNON_OX, dy=py-CANNON_OY;
        int d2=dx*dx+dy*dy;
        if (d2<best_d) { best_d=d2; best_ang=aim_at(px,py); }
    }
    for (int i=0;i<MAX_BOMBS;i++) {
        if (!bombs[i].active) continue;
        int bx=FP2PX(bombs[i].x), by=FP2PX(bombs[i].y);
        int dx=bx-CANNON_OX, dy=by-CANNON_OY;
        int d2=dx*dx+dy*dy;
        if (d2<best_d) { best_d=d2; best_ang=aim_at(bx,by); }
    }
    demo_tgt = best_ang;
    if (cannon_angle_deg<demo_tgt) cannon_angle_deg++;
    else if (cannon_angle_deg>demo_tgt) cannon_angle_deg--;
    if (--demo_fcd<=0 && absi(cannon_angle_deg-demo_tgt)<=2 && best_d<140*140) {
        cannon_rot_dir=0; player_fire(); demo_fcd=8;
    }
}

// ---------------------------------------------------------------------------
// Tick principal
// ---------------------------------------------------------------------------
static void pt_tick(void) {
    blink++;
    update_dt_scale();

    if (demo_mode) {
        bool any = controls_menu_select()
                || controls_get_raw_delta(0) != 0
                || controls_button_down(BTN_J1_B);
        if (any || ++demo_ticks >= TICKS_S*30) {
            g_done = true;
            return;
        }
    } else {
        // Girar el encoder (índice 4/5, ver BTN_IDX_ENC*_SW más arriba) sale
        // al menú en cualquier momento -- equivalente a "SW1: salir".
        if (controls_button_pressed(BTN_IDX_ENC1_SW) || controls_button_pressed(BTN_IDX_ENC2_SW)) {
            g_done = true;
            return;
        }
    }

    bool btn = controls_button_pressed(BTN_J1_A) || controls_button_pressed(BTN_J1_B);

    switch (state) {

    case PT_TITLE:
        if (btn) game_start();
        {
            bool bon = (blink/20)%2==0;
            update_bottom_message(bon ? "PULSA A/B PARA JUGAR" : "", 1);
        }
        break;

    case PT_PLAYING:
    case PT_JET_PASS: {
        if (!demo_mode) {
            int enc = controls_get_raw_delta(0);
            cannon_enc_acc += enc;
            if (cannon_enc_acc>=4)       { cannon_enc_acc=0; cannon_rot_dir= 1; }
            else if (cannon_enc_acc<=-4) { cannon_enc_acc=0; cannon_rot_dir=-1; }
            if (cannon_rot_dir!=0) {
                cannon_angle_deg += cannon_rot_dir*CANNON_ROT_SPD;
                if (cannon_angle_deg<=ANGLE_MIN_DEG) { cannon_angle_deg=ANGLE_MIN_DEG; cannon_rot_dir=0; }
                if (cannon_angle_deg>=ANGLE_MAX_DEG) { cannon_angle_deg=ANGLE_MAX_DEG; cannon_rot_dir=0; }
            }
            if (btn) { cannon_rot_dir=0; cannon_enc_acc=0; if (!fire_held) player_fire(); fire_held=true; }
            else fire_held=false;
        } else {
            demo_ai();
        }

        if (cannon_inv>0) cannon_inv--;

        if (state==PT_PLAYING && !tower_active) {
            for (int lane=0;lane<2;lane++) {
                if (--lane_timer[lane] <= 0) {
                    lane_timer[lane] = HELI_GAP + rnd(TICKS_S);
                    if (wave_helis_spawned < wave_heli_budget) {
                        spawn_heli(lane);
                        wave_helis_spawned++;
                    }
                }
            }
        }

        update_helis();
        update_paras();
        update_tower();
        update_plane();
        update_bombs();
        update_bullets();
        upd_particles();
        check_collisions();

        if (state==PT_PLAYING && cannon_alive) {
            if (attack_mode) {
                if (--attack_timer<=0) {
                    attack_mode=false;
                    attack_cooldown=ATTACK_INTERVAL+rnd(TICKS_S*10);
                }
            } else {
                if (--attack_cooldown<=0) {
                    attack_mode=true;
                    attack_timer=ATTACK_DUR;
                }
            }
        }

        if (!cannon_alive) {
            for (int i=0;i<MAX_HELIS;i++) helis[i].active=false;
            state=PT_DEAD; pause_cnt=TICKS_S*3;
            sound_stop_paratrooper_music(); // por si el jingle de inicio seguía sonando
            draw_playing_frame();
            break;
        }

        if (state==PT_PLAYING) {
            if (!wave_done && !tower_active && wave_clear()) {
                wave_done = true;
                jet_delay = TICKS_S*2;
            }
            if (wave_done && !jet_launched) {
                if (--jet_delay <= 0) {
                    launch_jet();
                    jet_launched = true;
                    state = PT_JET_PASS;
                }
            }
        }
        if (state==PT_JET_PASS) {
            if (!plane.active && !bombs[0].active && !bombs[1].active) {
                wave++; score += PTS_WAVE_BONUS;
                reset_wave();
                for (int i=0;i<MAX_PARAS;i++) {
                    Para *p = &paras[i];
                    if (!p->active || !p->landed) continue;
                    slot_count[p->side][p->slot]++;
                    ground_total[p->side]++;
                    p->marching=false; p->climbing=false;
                    p->march_order=-1;
                    p->target_x=p->target_y=0;
                }
                state = PT_PLAYING;
                sound_effect_success();
            }
        }

        draw_playing_frame();
        break;
    }

    case PT_DEAD:
        upd_particles();
        if (--pause_cnt <= 0) {
            sound_siren_stop();
            sound_effect_game_over();
            draw_playing_frame();
            if (!demo_mode && highscores_is_top(PT_GAME_ID, (uint32_t)score)) {
                highscores_enter(PT_GAME_ID, (uint32_t)score); // bloqueante
            }
            pause_cnt = 0;
            state = PT_GAMEOVER;
            draw_gameover_screen();
        } else {
            draw_playing_frame();
        }
        break;

    case PT_GAMEOVER: {
        bool bon = (blink/20)%2==0;
        update_bottom_message(bon ? "PULSA PARA CONTINUAR" : "", 1);
        pause_cnt++;
        if (demo_mode) {
            if (pause_cnt > TICKS_S*2) g_done = true;
        } else if (btn || pause_cnt > TICKS_S*8) {
            pause_cnt = 0;
            state = PT_SCORES;
            draw_scores_screen();
        }
        break;
    }

    case PT_SCORES:
        if (btn || ++pause_cnt > TICKS_S*8) g_done = true;
        break;
    }
}

// ---------------------------------------------------------------------------
// API pública -- firma game_run_fn (game_common.h): el menú solo conoce
// void paratrooper_run(game_mode_t mode), sin valor de retorno y sin
// función de demo separada (demo = mode==GAME_MODE_DEMO), exactamente
// igual que game_asteroids_run() en asteroids.c. Paratrooper no tiene
// concepto de 2 jugadores -- GAME_MODE_2P se trata igual que 1P.
// ---------------------------------------------------------------------------
void game_paratrooper_run(game_mode_t mode) {
    srand(time_us_32());

    bool is_demo = (mode == GAME_MODE_DEMO);

    demo_mode = is_demo;
    demo_ticks = 0;
    blink = 0;
    pause_cnt = 0;
    fire_held = false;
    g_done = false;
    cannon_enc_acc = 0; cannon_rot_dir = 0;
    demo_fcd = 0; demo_tgt = ANGLE_UP_DEG;
    wave = 1;
    field_needs_redraw = true;

    memset(helis, 0, sizeof(helis));
    memset(paras, 0, sizeof(paras));
    memset(bullets, 0, sizeof(bullets));
    memset(bombs, 0, sizeof(bombs));
    memset(&plane, 0, sizeof(plane));
    memset(particles, 0, sizeof(particles));

    last_tick_time = get_absolute_time();
    reset_render_trace();

    if (is_demo) {
        /*
         * Vaciamos el acumulador de controls_get_raw_delta(0) antes
         * de arrancar -- es la señal que usa pt_tick() para saber si
         * un jugador de verdad ha tocado el encoder y así cortar la
         * demo. Sin este vaciado, cualquier ruido/rebote mecánico
         * acumulado desde la ÚLTIMA vez que alguien leyó ese
         * acumulador -- que puede ser rato antes, mientras el menú
         * estaba en el marcador o en otra pantalla que no lo consume
         * -- se lee como "el jugador ha tocado el mando" en el
         * primerísimo tick, y la demo se corta antes de llegar a
         * dibujar un solo fotograma.
         */
        controls_get_raw_delta(0);
        game_start();
    } else {
        state = PT_TITLE;
        draw_title_screen();
    }

    while (!g_done) {
        controls_update();
        pt_tick();
        sound_update();
        sleep_ms(8);
    }

    sound_siren_stop();
    sound_stop_paratrooper_music(); // por si se sale a mitad del jingle de inicio
    highscores_flush();
}