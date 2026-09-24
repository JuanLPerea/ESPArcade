/**
 * asteroids.c -- portado de ArcadePi (https://github.com/JuanLPerea/ArcadePi),
 * mismo concepto (física de inercia Q8.8, 2 jugadores, hyperdrive,
 * OVNI, partículas), adaptado a ArcadeColor. Ver los mismos
 * principios de adaptación que en pong.c/space_invaders.c:
 *
 *  - Resolución: campo a pantalla casi completa (320x240 apaisada)
 *    en vez de 768x576. Radios y velocidades reescalados a mano
 *    (no proporcionalmente exactos, ajustados para que se vean bien
 *    y jueguen bien en una pantalla mucho más pequeña).
 *  - FÍSICA CON TIEMPO DELTA REAL: esto es nuevo respecto a los
 *    juegos anteriores, y importante. El original asume un tick de
 *    ~16ms fijo (posición += velocidad, una vez por tick, a 62Hz).
 *    Aquí NINGÚN tick dura lo mismo -- el coste de refrescar la
 *    pantalla por SPI varía según cuánto haya que dibujar ese tick
 *    (ver el mismo problema, más limitado, que ya nos dio la
 *    velocidad inconstante de la bola en Pong y el ritmo irregular
 *    de Space Invaders). Con TANTOS objetos moviéndose a la vez,
 *    aquí afectaría a toda la física, no solo a un temporizador.
 *    La solución: medir el tiempo real transcurrido entre ticks
 *    (dt_ms) y escalar todo el movimiento por él, en vez de asumir
 *    que cada tick avanza "una unidad de tiempo fija". Ver
 *    g_dt_scale más abajo.
 *  - Controles: encaja perfecto con nuestros 6 botones --
 *    disparo=BTN_J*_A (flanco), thrust=BTN_J*_B (mantenido),
 *    hyperdrive=BTN_ENC*_SW (flanco), giro=controls_get_raw_delta().
 *  - Menos objetos simultáneos: MAX_AST 24->14, MAX_BULLETS 8->6,
 *    MAX_PARTICLES 64->32 (menos carga de render incremental).
 *  - Render incremental: naves/asteroides/OVNI con su propio
 *    borrado+flush; balas y partículas agrupadas en un flush
 *    compartido cada una (son muchas y diminutas, moviéndose casi
 *    siempre juntas en el tiempo/espacio de una explosión o ráfaga).
 *  - Sonido: reutiliza sound_siren_start()/stop() (la sirena del
 *    platillo de Space Invaders) para el OVNI -- incluso más
 *    apropiado aquí, ya que el original también usaba una sirena en
 *    bucle mientras vuela. El resto de SFX (SFX_FIRE, SFX_AST_*,
 *    SFX_TICTAC_*...) no existen tal cual en nuestro sound.c, se
 *    mapean a los efectos que sí tenemos.
 *  - Sin hs_input/AS_ENTER_NAME: highscores_enter() bloqueante,
 *    como en los otros dos juegos.
 *  - Pantalla de seleccion rediseñada: mismo estilo de cursor que
 *    tetris.c (opcion resaltada con flechas ">  <" en amarillo), y un
 *    grafico decorativo bajo el titulo -- una nave con su llama de
 *    thrust y dos asteroides, dibujados con las mismas funciones
 *    vectoriales draw_ship()/draw_ast() de la partida real (ver
 *    draw_menu_deco()), no un dibujo aparte.
 *  - Bucle propio: game_asteroids_run(mode) con su propio bucle,
 *    nada de callbacks de dibujo/tick registrados aparte.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "asteroids.h"
#include "renderer.h"
#include "controls.h"
#include "highscores.h"
#include "sound.h"

// ---------------------------------------------------------------------------
// Área de juego -- literales fijos (no TFT_WIDTH/TFT_HEIGHT, que son
// variables en tiempo de ejecución -- ver el mismo comentario, más
// largo, en pong.c/space_invaders.c). Rotación fija a 320x240.
// ---------------------------------------------------------------------------
#define SCREEN_W 320
#define SCREEN_H 240

/*
 * OJO -- bug en controls.c: controls_button_pressed()/controls_button_down()
 * tratan su argumento como INDICE dentro de button_pins[] (0..5), no como
 * el numero de pin GPIO. PIN_BTN_J1_A/J1_B/J2_A/J2_B (0,1,2,3) coinciden
 * por casualidad con su indice, pero PIN_ENC1_SW/PIN_ENC2_SW (8 y 12) NO --
 * por eso el hyperdrive (mas abajo) nunca se disparaba: se le pasaba el
 * pin en vez del indice, y la comprobacion de rango de controls.c lo
 * descartaba siempre. button_pins[] en controls.c tiene este orden:
 * J1_A, J1_B, J2_A, J2_B, ENC1_SW, ENC2_SW -- de ahi el 4 y el 5.
 */
#define BTN_IDX_ENC1_SW  4
#define BTN_IDX_ENC2_SW  5

#define PLAY_X   4
#define PLAY_Y   3
#define PLAY_W   (SCREEN_W - 2 * PLAY_X)   // 312
#define PLAY_H   (SCREEN_H - 2 * PLAY_Y)   // 234
#define CX       (PLAY_X + PLAY_W / 2)
#define CY       (PLAY_Y + PLAY_H / 2)

#define TICKS_S  60   // referencia nominal SOLO para calcular ms (ver g_dt_scale) -- no se cuentan ticks reales

// ---------------------------------------------------------------------------
// Física: punto fijo Q8.8 (igual que el original)
// ---------------------------------------------------------------------------
#define FP          256
#define PX2FP(px)   ((int32_t)(px) << 8)
#define FP2PX(fp)   ((int)((fp) >> 8))

#define SHIP_MAX_SPD    (2 * FP)
#define SHIP_THRUST     (FP / 10)
#define FRIC_NUM        254
#define FRIC_DEN        256

// 32 pasos de ángulo (11.25° cada uno) -- idéntica tabla al original
#define ANGLE_STEPS  32
static const int16_t SIN_TAB[32] = {
      0,  50,  98, 142, 181, 213, 237, 251,
    256, 251, 237, 213, 181, 142,  98,  50,
      0, -50, -98,-142,-181,-213,-237,-251,
   -256,-251,-237,-213,-181,-142, -98, -50
};
#define SINV(a)  SIN_TAB[(a) & 31]
#define COSV(a)  SIN_TAB[((a) + 8) & 31]

// ---------------------------------------------------------------------------
// Tiempo delta real -- ver cabecera del archivo. g_dt_scale es un
// factor Q8.8 (256 = "un tick nominal de 16ms exactos"); todo
// movimiento basado en velocidad se multiplica por él y se divide
// por FP, en vez de sumarse tal cual. Se recalcula una vez al
// principio de cada as_tick().
// ---------------------------------------------------------------------------
static absolute_time_t last_tick_time;
static int32_t g_dt_scale = FP;   // arranca en "un tick nominal"

static void update_dt_scale(void) {
    int64_t elapsed_us = absolute_time_diff_us(last_tick_time, get_absolute_time());
    last_tick_time = get_absolute_time();

    int32_t elapsed_ms = (int32_t)(elapsed_us / 1000);
    if (elapsed_ms < 1)  elapsed_ms = 1;
    if (elapsed_ms > 50) elapsed_ms = 50; // limita saltos tras una pausa larga (p.ej. highscores_enter())

    g_dt_scale = elapsed_ms * FP / 16;   // 16ms = referencia "1 tick nominal" del original
}

// ---------------------------------------------------------------------------
// Nave -- radios/velocidades reescalados para el campo más pequeño
// ---------------------------------------------------------------------------
#define SHIP_R          7
#define HYPER_CD        (5 * TICKS_S)
#define HYPER_DEATH_PCT 15
#define RESPAWN_INV     (2 * TICKS_S)
#define BLINK_HALF      15

typedef struct {
    int32_t  x, y, vx, vy;
    int      angle;
    int      lives;
    uint32_t score;
    bool     alive;
    bool     thrusting;
    int      hyper_cd;
    int      inv_ticks;
    bool     fire_held;
} Ship;

// ---------------------------------------------------------------------------
// Partículas de explosión
// ---------------------------------------------------------------------------
#define MAX_PARTICLES  32          // reducido de 64 (ver cabecera)
#define PART_LIFE_AST  28
#define PART_LIFE_SHIP 55
#define SHIP_DEATH_PAUSE  (TICKS_S * 2)

static int rnd(int n) { return n > 0 ? rand() % n : 0; }
static int blink;
static int menu_enc_acc = 0;

typedef struct {
    int32_t x, y, vx, vy;
    int     life, life_max;
    bool    active;
} Particle;

static Particle parts[MAX_PARTICLES];

static void spawn_explosion(int32_t x, int32_t y, int n, int speed_fp, int life) {
    int spawned = 0;
    for (int i = 0; i < MAX_PARTICLES && spawned < n; i++) {
        if (parts[i].active) continue;
        parts[i].active   = true;
        parts[i].x        = x;
        parts[i].y        = y;
        parts[i].life     = life + rnd(life/3);
        parts[i].life_max = parts[i].life;
        int dir = rnd(32);
        int spd = speed_fp/2 + rnd(speed_fp/2);
        parts[i].vx = spd * COSV(dir) / FP;
        parts[i].vy = spd * SINV(dir) / FP;
        spawned++;
    }
}

static void update_particles(void) {
    for (int i = 0; i < MAX_PARTICLES; i++) {
        if (!parts[i].active) continue;
        parts[i].x += parts[i].vx * g_dt_scale / FP;
        parts[i].y += parts[i].vy * g_dt_scale / FP;
        parts[i].vx = parts[i].vx * 252 / 256;
        parts[i].vy = parts[i].vy * 252 / 256;
        if (--parts[i].life <= 0) parts[i].active = false;
    }
}

#define MAX_BULLETS     6    // reducido de 8 (ver cabecera)
#define BULLET_SPD      (5 * FP)
#define BULLET_LIFE     (TICKS_S * 3 / 2)

typedef struct {
    int32_t x, y, vx, vy;
    int     life;
    bool    active;
    int     owner;   // 0=J1, 1=J2, 2=OVNI
} Bullet;

static Bullet bullets[MAX_BULLETS];

// ---------------------------------------------------------------------------
// Asteroides -- radios reescalados
// ---------------------------------------------------------------------------
#define MAX_AST     14   // reducido de 24 (ver cabecera)
#define R_LARGE     14
#define R_MED        9
#define R_SMALL      5

typedef enum { SZ_LARGE=0, SZ_MED=1, SZ_SMALL=2 } AstSz;
static const int  AST_R[3]     = { R_LARGE, R_MED, R_SMALL };
static const int  AST_PTS[3]   = { 20, 50, 100 };
static const int  AST_SPD[3]   = { FP*3/4, FP*5/4, FP*2 };

// 4 variantes de forma (offsets radiales para 12 vértices) -- idéntico al original
static const int8_t AST_SHAPE[4][12] = {
    {  0, -4,  2, -3,  0,  4, -2,  3,  1, -3,  2,  0 },
    {  3,  0, -2,  4,  1, -3,  3,  0, -4,  2,  0, -2 },
    { -2,  3,  0, -4,  4,  0, -1,  3, -3,  0,  2, -3 },
    {  2, -2,  4,  0, -3,  3,  0, -4,  3,  2, -2,  4 },
};

typedef struct {
    int32_t x, y, vx, vy;
    AstSz   size;
    int     shape;
    bool    active;
} Asteroid;

static Asteroid asts[MAX_AST];

// ---------------------------------------------------------------------------
// OVNI -- radio reescalado
// ---------------------------------------------------------------------------
#define SAUCER_R        8
#define SAUCER_PTS      200
#define SAUCER_LIFE     (12 * TICKS_S)
#define SAUCER_FIRE_CD  (2 * TICKS_S)
#define SAUCER_SPD      (FP + FP/2)
#define SAUCER_DIR_CD_MIN  (TICKS_S * 2)
#define SAUCER_DIR_CD_MAX  (TICKS_S * 4)
#define SAUCER_VY_MAX   (FP + FP/4)

typedef struct {
    int32_t x, y, vx, vy;
    int     life, fire_cd, dir_change_cd;
    bool    active;
} Saucer;

static Saucer saucer;
static absolute_time_t next_saucer_spawn; // tiempo real -- ver comentario largo sobre esto en space_invaders.c

// ---------------------------------------------------------------------------
// Estados
// ---------------------------------------------------------------------------
typedef enum {
    AS_SELECT,
    AS_PLAYING,
    AS_SHIP_DEAD,
    AS_LEVEL_CLEAR,
    AS_GAME_OVER,
    AS_SCORES
} AsSt;

static AsSt state;
static Ship ships[2];
static int  level;
static int  pause_ticks;
static int  tictac_cd;
static int  thrust_cd;
static int  num_players;
static bool demo;
static int  demo_ticks;
static bool g_done;

static int clamp(int v, int lo, int hi) { return v<lo?lo:v>hi?hi:v; }

// ---------------------------------------------------------------------------
// Utilidades -- idénticas al original salvo el envoltorio de tipos
// ---------------------------------------------------------------------------
static void wrap(int32_t *x, int32_t *y) {
    int32_t x0 = PX2FP(PLAY_X), x1 = PX2FP(PLAY_X + PLAY_W);
    int32_t y0 = PX2FP(PLAY_Y), y1 = PX2FP(PLAY_Y + PLAY_H);
    if (*x <  x0) *x += PX2FP(PLAY_W);
    if (*x >= x1) *x -= PX2FP(PLAY_W);
    if (*y <  y0) *y += PX2FP(PLAY_H);
    if (*y >= y1) *y -= PX2FP(PLAY_H);
}

static bool hit(int32_t ax, int32_t ay, int ra,
                int32_t bx, int32_t by, int rb) {
    int dx = FP2PX(ax) - FP2PX(bx);
    int dy = FP2PX(ay) - FP2PX(by);
    int r  = ra + rb;
    return dx*dx + dy*dy < r*r;
}

// Rotar punto relativo (rx,ry) según ángulo a y trasladar a (cx,cy)
static void rot(int cx, int cy, int rx, int ry, int a, int *ox, int *oy) {
    *ox = cx + (rx * COSV(a) - ry * SINV(a)) / FP;
    *oy = cy + (rx * SINV(a) + ry * COSV(a)) / FP;
}

// ---------------------------------------------------------------------------
// Dibujo vectorial -- líneas por Bresenham (renderer_fill_rect 2x2 por
// paso, igual que el renderer_draw_rect(x,y,2,2) del original)
// ---------------------------------------------------------------------------
static void line(int x0, int y0, int x1, int y1, uint16_t color) {
    int dx = x1-x0; if (dx<0) dx=-dx;
    int dy = y1-y0; if (dy<0) dy=-dy;
    int sx = x0<x1 ? 1:-1, sy = y0<y1 ? 1:-1;
    int err = dx - dy;
    for (;;) {
        if (x0>=PLAY_X && x0<PLAY_X+PLAY_W && y0>=PLAY_Y && y0<PLAY_Y+PLAY_H)
            renderer_fill_rect(x0, y0, 2, 2, color);
        if (x0==x1 && y0==y1) break;
        int e2 = 2*err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

static int centered_x(const char *text, int scale) {
    int w = (int)st7789_text_width(text, (uint8_t)scale);
    int x = (TFT_WIDTH - w) / 2;
    return (x < 0) ? 0 : x;
}

// ---------------------------------------------------------------------------
// Dibujo -- nave (idéntico trazado vectorial al original)
// ---------------------------------------------------------------------------
static void draw_ship(const Ship *s, int p, uint16_t color) {
    if (!s->alive) return;
    if (s->inv_ticks > 0 && (blink / BLINK_HALF) % 2 == 1) return;

    int cx = FP2PX(s->x), cy = FP2PX(s->y), a = s->angle;

    int px,py, lx,ly, rx,ry, tx,ty;
    rot(cx,cy,  0,-9, a, &px,&py);
    rot(cx,cy, -6, +6, a, &lx,&ly);
    rot(cx,cy, +6, +6, a, &rx,&ry);
    rot(cx,cy,  0, +4, a, &tx,&ty);

    line(px,py, lx,ly, color);
    line(px,py, rx,ry, color);
    line(lx,ly, rx,ry, color);
    line(lx,ly, tx,ty, color);
    line(rx,ry, tx,ty, color);

    // Marca J2: barra horizontal en tobera
    if (p == 1) {
        int m1x,m1y,m2x,m2y;
        rot(cx,cy,-3,+6,a,&m1x,&m1y);
        rot(cx,cy,+3,+6,a,&m2x,&m2y);
        line(m1x,m1y,m2x,m2y, color);
    }

    // Llama thrust
    if (s->thrusting && (blink%4)<3) {
        int f1x,f1y,f2x,f2y,fmx,fmy;
        int fl = 5 + rnd(4);
        rot(cx,cy,-2,+6,a,&f1x,&f1y);
        rot(cx,cy,+2,+6,a,&f2x,&f2y);
        rot(cx,cy, 0,+6+fl,a,&fmx,&fmy);
        line(f1x,f1y,fmx,fmy, COLOR_YELLOW);
        line(f2x,f2y,fmx,fmy, COLOR_YELLOW);
    }
}

// Radio de la caja que hay que borrar/redibujar para cubrir la nave
// entera (incluida la llama de thrust en su tamaño máximo)
#define SHIP_BBOX_R 18

// ---------------------------------------------------------------------------
// Dibujo -- asteroide (idéntico trazado vectorial al original)
// ---------------------------------------------------------------------------
static void draw_ast(const Asteroid *a, uint16_t color) {
    if (!a->active) return;
    int cx = FP2PX(a->x), cy = FP2PX(a->y);
    int br = AST_R[a->size];
    int vx[12], vy[12];
    for (int i=0;i<12;i++) {
        int ai = (i*32/12) & 31;
        int r  = br + AST_SHAPE[a->shape&3][i] / 2; // formas reescaladas al radio menor
        vx[i] = cx + r * COSV(ai) / FP;
        vy[i] = cy + r * SINV(ai) / FP;
    }
    for (int i=0;i<12;i++)
        line(vx[i],vy[i],vx[(i+1)%12],vy[(i+1)%12], color);
}

// ---------------------------------------------------------------------------
// Dibujo -- OVNI (idéntico trazado vectorial al original)
// ---------------------------------------------------------------------------
static void draw_saucer(const Saucer *s, uint16_t color) {
    if (!s->active) return;
    int cx = FP2PX(s->x), cy = FP2PX(s->y);
    int ep[8][2];
    for (int i=0;i<8;i++) {
        ep[i][0] = cx + 8 * COSV(i*4) / FP;
        ep[i][1] = cy + 3 * SINV(i*4) / FP;
    }
    for (int i=0;i<8;i++)
        line(ep[i][0],ep[i][1],ep[(i+1)%8][0],ep[(i+1)%8][1], color);
    int dp[5][2];
    for (int i=0;i<5;i++) {
        int ai = i * 4;
        dp[i][0] = cx + 3 * COSV(ai) / FP;
        dp[i][1] = cy - 1 + 3 * SINV(ai) / FP;
    }
    for (int i=0;i<4;i++)
        line(dp[i][0],dp[i][1],dp[i+1][0],dp[i+1][1], color);
    line(dp[0][0],dp[0][1],dp[4][0],dp[4][1], color);
}

// ---------------------------------------------------------------------------
// Render incremental -- borrado por caja delimitadora (bounding box)
// en vez de redibujar todo cada frame (imposible por coste de SPI a
// este tamaño de pantalla, ver cabecera del archivo y el mismo
// patrón en pong.c/space_invaders.c). Como estas formas son
// vectoriales (líneas, no rectángulos sólidos), se borra el
// rectángulo que las envuelve y se redibuja encima.
// ---------------------------------------------------------------------------
#define COLOR_SHIP0  COLOR_CYAN
#define COLOR_SHIP1  COLOR_YELLOW
#define COLOR_AST    COLOR_WHITE
#define COLOR_SAUCER COLOR_RED

typedef struct { int x, y; bool alive; } PrevPos;

static PrevPos prev_ship[2];
static PrevPos prev_ast[MAX_AST];
static PrevPos prev_saucer_pos = { .alive = false };
static bool    field_needs_redraw = true;
static int     prev_score[2] = { -1, -1 };
static int     prev_lives[2] = { -1, -1 };
static int     prev_level_hud = -1;
static char    prev_center_msg[24] = "";
static char    prev_bottom_msg[40] = "";

static void reset_render_trace(void) {
    prev_ship[0].alive = false;
    prev_ship[1].alive = false;
    for (int i = 0; i < MAX_AST; i++) prev_ast[i].alive = false;
    prev_saucer_pos.alive = false;
    prev_score[0] = prev_score[1] = -1;
    prev_lives[0] = prev_lives[1] = -1;
    prev_level_hud = -1;
    prev_center_msg[0] = '\0';
    prev_bottom_msg[0] = '\0';
}

/*
 * Borra una caja, recortándola primero a coordenadas no negativas.
 *
 * renderer_fill_rect() recibe x/y como uint16_t. Las cajas de borrado
 * de nave/asteroide/OVNI se calculan como "posición - radio", que
 * sale NEGATIVO en cuanto el objeto está a menos del radio del borde
 * izquierdo o superior. Un int negativo convertido a uint16_t da la
 * vuelta a un número enorme (65529 en vez de -7, por ejemplo), y la
 * comprobación de límites de st7789_fill_rect lo descarta sin avisar
 * -- el borrado sencillamente no ocurre, y el objeto se queda
 * "fantasma" dibujado para siempre en esa posición. Por eso se veían
 * rastros concentrados en el borde izquierdo (donde SHIP_BBOX_R/
 * radios de asteroide superan la coordenada X con facilidad).
 */
static void erase_box(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    renderer_fill_rect(x, y, w, h, COLOR_BLACK);
}

static void draw_field_static(void) {
    renderer_clear(COLOR_BLACK);
    renderer_fill_rect(PLAY_X, PLAY_Y,          PLAY_W, 1, COLOR_WHITE);
    renderer_fill_rect(PLAY_X, PLAY_Y+PLAY_H-1, PLAY_W, 1, COLOR_WHITE);
    reset_render_trace();
    field_needs_redraw = false;
    renderer_flush();
}

static void draw_ship_if_moved(int p) {
    const Ship *s = &ships[p];
    int cx = FP2PX(s->x), cy = FP2PX(s->y);
    bool show = s->alive;

    if (!show && !prev_ship[p].alive) return;

    if (prev_ship[p].alive)
        erase_box(prev_ship[p].x - SHIP_BBOX_R, prev_ship[p].y - SHIP_BBOX_R,
                  SHIP_BBOX_R*2, SHIP_BBOX_R*2);
    if (show)
        draw_ship(s, p, p==0 ? COLOR_SHIP0 : COLOR_SHIP1);

    prev_ship[p].x = cx; prev_ship[p].y = cy; prev_ship[p].alive = show;
    renderer_flush();
}

static void draw_asteroids_if_moved(void) {
    for (int i = 0; i < MAX_AST; i++) {
        const Asteroid *a = &asts[i];
        int cx = FP2PX(a->x), cy = FP2PX(a->y);
        bool show = a->active;

        if (!show && !prev_ast[i].alive) continue;
        int r = AST_R[a->active ? a->size : SZ_LARGE] + 4;

        if (prev_ast[i].alive) {
            int pr = r; // usamos el mismo margen; el radio real no cambia mientras está vivo
            erase_box(prev_ast[i].x - pr, prev_ast[i].y - pr, pr*2, pr*2);
        }
        if (show)
            draw_ast(a, COLOR_AST);

        prev_ast[i].x = cx; prev_ast[i].y = cy; prev_ast[i].alive = show;
        renderer_flush();
    }
}

static void draw_saucer_if_moved(void) {
    int cx = FP2PX(saucer.x), cy = FP2PX(saucer.y);
    bool show = saucer.active;

    if (!show && !prev_saucer_pos.alive) return;

    if (prev_saucer_pos.alive)
        erase_box(prev_saucer_pos.x - SAUCER_R - 4, prev_saucer_pos.y - SAUCER_R - 4,
                  (SAUCER_R+4)*2, (SAUCER_R+4)*2);
    if (show)
        draw_saucer(&saucer, COLOR_SAUCER);

    prev_saucer_pos.x = cx; prev_saucer_pos.y = cy; prev_saucer_pos.alive = show;
    renderer_flush();
}

// Balas y partículas: se agrupan en un único flush cada una (son
// muchas y diminutas, y suelen estar juntas en el tiempo/espacio de
// una ráfaga o una explosión -- un flush por unidad sería demasiada
// sobrecarga de transacciones SPI para lo poco que aportaría).
static int prev_bullet_x[MAX_BULLETS], prev_bullet_y[MAX_BULLETS];
static bool prev_bullet_active[MAX_BULLETS];
static int prev_part_x[MAX_PARTICLES], prev_part_y[MAX_PARTICLES];
static bool prev_part_active[MAX_PARTICLES];

static void draw_bullets_if_moved(void) {
    bool any = false;
    for (int i = 0; i < MAX_BULLETS; i++) {
        int x = FP2PX(bullets[i].x) - 1, y = FP2PX(bullets[i].y) - 1;
        bool show = bullets[i].active;
        if (!show && !prev_bullet_active[i]) continue;

        if (prev_bullet_active[i])
            renderer_fill_rect(prev_bullet_x[i], prev_bullet_y[i], 3, 3, COLOR_BLACK);
        
        if (show) {
            // Seleccionar color según el propietario del disparo
            uint16_t bullet_color = COLOR_WHITE;
            if (bullets[i].owner == 0)      bullet_color = COLOR_SHIP0; // Cian para J1
            else if (bullets[i].owner == 1) bullet_color = COLOR_SHIP1; // Amarillo para J2
            else if (bullets[i].owner == 2) bullet_color = COLOR_SAUCER; // Rojo para el OVNI

            renderer_fill_rect(x, y, 3, 3, bullet_color);
        }

        prev_bullet_x[i] = x; prev_bullet_y[i] = y; prev_bullet_active[i] = show;
        any = true;
    }
    if (any) renderer_flush();
}

static void draw_particles_if_moved(void) {
    bool bon_blink = (blink % 2) == 0;
    bool any = false;
    for (int i = 0; i < MAX_PARTICLES; i++) {
        int x = FP2PX(parts[i].x), y = FP2PX(parts[i].y);
        int age_pct = parts[i].active ? (parts[i].life * 100 / parts[i].life_max) : 0;
        bool show = parts[i].active && (age_pct >= 30 || bon_blink);

        if (!show && !prev_part_active[i]) continue;

        if (prev_part_active[i])
            renderer_fill_rect(prev_part_x[i], prev_part_y[i], 2, 2, COLOR_BLACK);
        if (show &&
            x>=PLAY_X && x<PLAY_X+PLAY_W && y>=PLAY_Y && y<PLAY_Y+PLAY_H)
            renderer_fill_rect(x, y, 2, 2, COLOR_YELLOW);

        prev_part_x[i] = x; prev_part_y[i] = y; prev_part_active[i] = show;
        any = true;
    }
    if (any) renderer_flush();
}

// ---------------------------------------------------------------------------
// HUD -- solo redibuja lo que cambia, como en pong.c/space_invaders.c
// ---------------------------------------------------------------------------
static void draw_ship_icon(int lx, int ly, uint16_t color) {
    line(lx+3,ly,   lx,  ly+6, color);
    line(lx+3,ly,   lx+6,ly+6, color);
    line(lx+1,ly+5, lx+5,ly+5, color);
}

static absolute_time_t next_hud_refresh;

static void draw_hud_if_changed(void) {
    char buf[16];
    bool changed = false;

    // Los asteroides/nave pasan por encima de la zona del HUD (no hay
    // hueco reservado para ellos, a diferencia de Space Invaders) y su
    // borrado incremental puede pisar el texto del HUD sin que ningún
    // valor haya cambiado. Forzamos un redibujado completo cada ~700ms
    // para "curar" eso, además del redibujado normal por cambio.
    bool force = time_reached(next_hud_refresh);
    if (force) next_hud_refresh = make_timeout_time_ms(700);

    if ((int)ships[0].score != prev_score[0] || force) {
        renderer_fill_rect(PLAY_X+2, PLAY_Y+3, 70, 14, COLOR_BLACK);
        snprintf(buf, sizeof(buf), "%u", ships[0].score);
        renderer_draw_text(PLAY_X+2, PLAY_Y+3, buf, COLOR_SHIP0, COLOR_BLACK, 2);
        prev_score[0] = (int)ships[0].score;
        changed = true;
    }
    if (ships[0].lives != prev_lives[0] || force) {
        renderer_fill_rect(PLAY_X+2, PLAY_Y+20, 70, 10, COLOR_BLACK);
        int lives = clamp(ships[0].lives, 0, 5);
        for (int i = 0; i < lives; i++)
            draw_ship_icon(PLAY_X+4+i*11, PLAY_Y+21, COLOR_SHIP0);
        prev_lives[0] = ships[0].lives;
        changed = true;
    }
    if (level != prev_level_hud || force) {
        renderer_fill_rect(CX-30, PLAY_Y+3, 60, 14, COLOR_BLACK);
        snprintf(buf, sizeof(buf), "NIV %d", level);
        renderer_draw_text(centered_x(buf, 2), PLAY_Y+3, buf, COLOR_WHITE, COLOR_BLACK, 2);
        prev_level_hud = level;
        changed = true;
    }
    if (num_players == 2) {
        if ((int)ships[1].score != prev_score[1] || force) {
            renderer_fill_rect(PLAY_X+PLAY_W-72, PLAY_Y+3, 70, 14, COLOR_BLACK);
            snprintf(buf, sizeof(buf), "%u", ships[1].score);
            int x = PLAY_X+PLAY_W-2-(int)st7789_text_width(buf, 2);
            renderer_draw_text(x, PLAY_Y+3, buf, COLOR_SHIP1, COLOR_BLACK, 2);
            prev_score[1] = (int)ships[1].score;
            changed = true;
        }
        if (ships[1].lives != prev_lives[1] || force) {
            renderer_fill_rect(PLAY_X+PLAY_W-72, PLAY_Y+20, 70, 10, COLOR_BLACK);
            int lives = clamp(ships[1].lives, 0, 5);
            for (int i = 0; i < lives; i++)
                draw_ship_icon(PLAY_X+PLAY_W-15-i*11, PLAY_Y+21, COLOR_SHIP1);
            prev_lives[1] = ships[1].lives;
            changed = true;
        }
    }
    if (changed) renderer_flush();
}

// Mensaje central grande (READY / NIVEL SUPERADO / GAME OVER), solo
// redibuja si el texto que toca mostrar cambia -- mismo patrón que
// el mensaje inferior de pong.c/space_invaders.c.
static void update_center_message(const char *target, uint16_t color, int scale) {
    if (strcmp(target, prev_center_msg) == 0) return;

    renderer_fill_rect(0, CY-16, TFT_WIDTH, 32, COLOR_BLACK);
    if (target[0]) {
        renderer_draw_text(centered_x(target, scale), CY-10, target, color, COLOR_BLACK, scale);
    }
    strncpy(prev_center_msg, target, sizeof(prev_center_msg) - 1);
    prev_center_msg[sizeof(prev_center_msg) - 1] = '\0';
    renderer_flush();
}

static void update_bottom_message(const char *target, int scale) {
    if (strcmp(target, prev_bottom_msg) == 0) return;

    renderer_fill_rect(0, PLAY_Y+PLAY_H-18, TFT_WIDTH, 16, COLOR_BLACK);
    if (target[0]) {
        renderer_draw_text(centered_x(target, scale), PLAY_Y+PLAY_H-16, target, COLOR_WHITE, COLOR_BLACK, scale);
    }
    strncpy(prev_bottom_msg, target, sizeof(prev_bottom_msg) - 1);
    prev_bottom_msg[sizeof(prev_bottom_msg) - 1] = '\0';
    renderer_flush();
}

static void draw_playing_frame(void) {
    if (field_needs_redraw) draw_field_static();

    draw_asteroids_if_moved();
    draw_bullets_if_moved();
    draw_particles_if_moved();
    draw_saucer_if_moved();
    draw_ship_if_moved(0);
    if (num_players == 2) draw_ship_if_moved(1);

    draw_hud_if_changed();

    bool bon = (blink / BLINK_HALF) % 2 == 0;

    const char *center = "";
    uint16_t center_color = COLOR_YELLOW;
    int center_scale = 2;
    if (state == AS_SHIP_DEAD && bon)        { center = "READY"; center_scale = 3; }
    else if (state == AS_LEVEL_CLEAR && bon) { center = "NIVEL SUPERADO"; }
    else if (state == AS_GAME_OVER)          { center = "GAME OVER"; center_scale = 3; }
    update_center_message(center, center_color, center_scale);

    const char *bottom = "";
    if (state == AS_GAME_OVER && bon) bottom = "PULSA PARA CONTINUAR";
    else if (demo && bon)              bottom = "DEMO - PULSA PARA JUGAR";
    update_bottom_message(bottom, 1);
}

// ---------------------------------------------------------------------------
// Pantallas "estáticas" -- se redibujan enteras solo al entrar en el
// estado, como en pong.c/space_invaders.c
// ---------------------------------------------------------------------------

// Gráfico decorativo del menú: una nave con su llama de thrust y un
// par de asteroides, dibujados con las MISMAS funciones vectoriales
// draw_ship()/draw_ast() que usa la partida real (mismo trazado por
// líneas, no un dibujo aparte) -- structs locales mínimas que no
// forman parte de la partida ni interactúan con ella.
static void draw_menu_deco(int cy) {
    Ship deco_ship = {0};
    deco_ship.x         = PX2FP(CX);
    deco_ship.y         = PX2FP(cy);
    deco_ship.angle     = 0;      // apunta hacia arriba (ver draw_ship: vertice en 0,-9)
    deco_ship.alive     = true;
    deco_ship.thrusting = true;   // llama siempre encendida, puramente decorativo
    draw_ship(&deco_ship, 0, COLOR_SHIP0);

    Asteroid deco_ast1 = {0};
    deco_ast1.x      = PX2FP(CX - 75);
    deco_ast1.y      = PX2FP(cy + 6);
    deco_ast1.size   = SZ_MED;
    deco_ast1.shape  = 1;
    deco_ast1.active = true;
    draw_ast(&deco_ast1, COLOR_AST);

    Asteroid deco_ast2 = {0};
    deco_ast2.x      = PX2FP(CX + 75);
    deco_ast2.y      = PX2FP(cy - 6);
    deco_ast2.size   = SZ_SMALL;
    deco_ast2.shape  = 3;
    deco_ast2.active = true;
    draw_ast(&deco_ast2, COLOR_AST);
}

// Línea del menú de selección, resaltada con flechas ">  <" y en
// amarillo si es la opción actualmente elegida.
static void draw_menu_item(const char *text, int y, bool selected) {
    char buf[32];
    if (selected) snprintf(buf, sizeof(buf), "> %s <", text);
    else          snprintf(buf, sizeof(buf), "  %s  ", text);
    uint16_t color = selected ? COLOR_YELLOW : COLOR_WHITE;
    renderer_draw_text(centered_x(buf, 2), y, buf, color, COLOR_BLACK, 2);
}

static void draw_select_screen(void) {
    renderer_clear(COLOR_BLACK);

    renderer_draw_text(centered_x("ASTEROIDS", 3), PLAY_Y + 6, "ASTEROIDS", COLOR_CYAN, COLOR_BLACK, 3);

    draw_menu_deco(PLAY_Y + 60);

    draw_menu_item("1 JUGADOR",   PLAY_Y + 104, num_players == 1);
    draw_menu_item("2 JUGADORES", PLAY_Y + 130, num_players == 2);

    renderer_draw_text(centered_x("GIRA PARA CAMBIAR - PULSA PARA JUGAR", 1), PLAY_Y + 160,
                        "GIRA PARA CAMBIAR - PULSA PARA JUGAR", COLOR_WHITE, COLOR_BLACK, 1);
    renderer_draw_text(centered_x("DISPARO / THRUST / HYPER = BOTON A/B/GIRO", 1), PLAY_Y + 177,
                        "DISPARO / THRUST / HYPER = BOTON A/B/GIRO", COLOR_WHITE, COLOR_BLACK, 1);
    prev_bottom_msg[0] = '\0';
    prev_center_msg[0] = '\0';
    renderer_flush();
}

static void draw_scores_screen(void) {
    renderer_clear(COLOR_BLACK);
    highscores_draw(AS_GAME_ID, "ASTEROIDS", 20);
    renderer_flush();
}

// ---------------------------------------------------------------------------
// Spawn nave / asteroide / nivel
// ---------------------------------------------------------------------------
static void spawn_ship(int p) {
    Ship *s = &ships[p];
    s->x  = PX2FP(p==0 ? CX-40 : CX+40);
    s->y  = PX2FP(CY);
    s->vx = s->vy   = 0;
    s->angle        = 0;
    s->alive        = true;
    s->thrusting    = false;
    s->hyper_cd     = 0;
    s->inv_ticks    = RESPAWN_INV;
    s->fire_held    = true;
}

static void spawn_ast(AstSz sz, int32_t x, int32_t y) {
    for (int i=0;i<MAX_AST;i++) {
        if (asts[i].active) continue;
        asts[i].active = true;
        asts[i].size   = sz;
        asts[i].shape  = rnd(4);
        asts[i].x = x; asts[i].y = y;
        int spd = AST_SPD[sz] + (level-1)*FP/8;
        spd = spd * (2 + rnd(3)) / 3;
        int dir = rnd(32);
        asts[i].vx = spd * COSV(dir) / FP;
        asts[i].vy = spd * SINV(dir) / FP;
        return;
    }
}

static void level_init(void) {
    memset(asts,    0, sizeof(asts));
    memset(bullets, 0, sizeof(bullets));
    memset(parts,   0, sizeof(parts));
    saucer.active = false;
    next_saucer_spawn = make_timeout_time_ms(1000 * (12 + rand() % 13)); // 12-24s reales
    tictac_cd     = TICKS_S;
    thrust_cd     = 0;
    int n = 2 + level; if (n>6) n=6; // tope reducido (campo más pequeño, MAX_AST=14)
    for (int i=0;i<n;i++) {
        int side = rnd(4);
        int32_t ax, ay;
        switch (side) {
            case 0: ax=PX2FP(PLAY_X+rnd(PLAY_W)); ay=PX2FP(PLAY_Y);         break;
            case 1: ax=PX2FP(PLAY_X+rnd(PLAY_W)); ay=PX2FP(PLAY_Y+PLAY_H);  break;
            case 2: ax=PX2FP(PLAY_X);              ay=PX2FP(PLAY_Y+rnd(PLAY_H)); break;
            default:ax=PX2FP(PLAY_X+PLAY_W);       ay=PX2FP(PLAY_Y+rnd(PLAY_H)); break;
        }
        spawn_ast(SZ_LARGE, ax, ay);
    }
    field_needs_redraw = true;
}

static int count_asts(void) {
    int n=0; for (int i=0;i<MAX_AST;i++) if (asts[i].active) n++; return n;
}

// ---------------------------------------------------------------------------
// Disparar / hyperdrive / romper asteroide
// ---------------------------------------------------------------------------
static void fire(int owner) {
    const Ship *s = &ships[owner];
    if (!s->alive) return;
    for (int i=0;i<MAX_BULLETS;i++) {
        if (bullets[i].active) continue;
        Bullet *b = &bullets[i];
        b->active = true; b->owner = owner; b->life = BULLET_LIFE;
        int bx, by;
        rot(FP2PX(s->x), FP2PX(s->y), 0, -9, s->angle, &bx, &by);
        b->x = PX2FP(bx); b->y = PX2FP(by);
        b->vx = s->vx + BULLET_SPD * SINV(s->angle) / FP;
        b->vy = s->vy - BULLET_SPD * COSV(s->angle) / FP;
        sound_effect_shoot();
        return;
    }
}

static bool hyperdrive(int p) {
    Ship *s = &ships[p];
    if (!s->alive || s->hyper_cd > 0) return false;
    s->hyper_cd = HYPER_CD;
    if (rnd(100) < HYPER_DEATH_PCT) {
        s->alive = false; s->lives--;
        sound_effect_explosion();
        return true;
    }
    s->x = PX2FP(PLAY_X + R_LARGE + rnd(PLAY_W - 2*R_LARGE));
    s->y = PX2FP(PLAY_Y + R_LARGE + rnd(PLAY_H - 2*R_LARGE));
    s->vx = s->vy = 0;
    s->inv_ticks = RESPAWN_INV / 2;
    sound_effect_select();
    return false;
}

static void break_ast(int idx) {
    Asteroid *a = &asts[idx];
    int32_t sx = a->x, sy = a->y;

    int nparts = (a->size==SZ_LARGE) ? 12 : (a->size==SZ_MED) ? 8 : 5;
    int pspd   = (a->size==SZ_LARGE) ? FP*2 : (a->size==SZ_MED) ? FP*3 : FP*4;
    spawn_explosion(a->x, a->y, nparts, pspd, PART_LIFE_AST);

    sound_effect_explosion();
    a->active = false;

    if (a->size != SZ_SMALL) {
        AstSz next = (a->size==SZ_LARGE) ? SZ_MED : SZ_SMALL;
        spawn_ast(next, sx, sy);
        spawn_ast(next, sx, sy);
    }
}

// ---------------------------------------------------------------------------
// OVNI -- temporizador de aparición en tiempo real (ver cabecera del
// archivo), sirena en bucle mientras vuela (reutiliza
// sound_siren_start/stop del platillo de Space Invaders).
// ---------------------------------------------------------------------------
static void update_saucer(void) {
    if (!saucer.active) {
        if (time_reached(next_saucer_spawn)) {
            saucer.active       = true;
            saucer.life         = SAUCER_LIFE;
            saucer.fire_cd      = SAUCER_FIRE_CD;
            saucer.dir_change_cd = SAUCER_DIR_CD_MIN + rnd(SAUCER_DIR_CD_MAX - SAUCER_DIR_CD_MIN);
            int spd = SAUCER_SPD + (level-1)*FP/4;
            if (rnd(2)==0) { saucer.x=PX2FP(PLAY_X);       saucer.vx= spd; }
            else            { saucer.x=PX2FP(PLAY_X+PLAY_W); saucer.vx=-spd; }
            saucer.y  = PX2FP(PLAY_Y + PLAY_H/4 + rnd(PLAY_H/2));
            saucer.vy = 0;
            sound_siren_start();
        }
        return;
    }
    saucer.x += saucer.vx * g_dt_scale / FP;
    saucer.y += saucer.vy * g_dt_scale / FP;

    if (FP2PX(saucer.y) < PLAY_Y + SAUCER_R) {
        saucer.y  = PX2FP(PLAY_Y + SAUCER_R);
        saucer.vy = (saucer.vy < 0) ? -saucer.vy : saucer.vy;
    }
    if (FP2PX(saucer.y) > PLAY_Y + PLAY_H - SAUCER_R) {
        saucer.y  = PX2FP(PLAY_Y + PLAY_H - SAUCER_R);
        saucer.vy = (saucer.vy > 0) ? -saucer.vy : saucer.vy;
    }

    if (--saucer.dir_change_cd <= 0) {
        saucer.dir_change_cd = SAUCER_DIR_CD_MIN + rnd(SAUCER_DIR_CD_MAX - SAUCER_DIR_CD_MIN);
        int vy_options[5] = { -SAUCER_VY_MAX, -SAUCER_VY_MAX/2, 0, SAUCER_VY_MAX/2, SAUCER_VY_MAX };
        saucer.vy = vy_options[rnd(5)];
    }

    if (--saucer.life <= 0 ||
        FP2PX(saucer.x) < PLAY_X-30 ||
        FP2PX(saucer.x) > PLAY_X+PLAY_W+30) {
        saucer.active = false;
        next_saucer_spawn = make_timeout_time_ms(1000 * (12 + rand() % 13));
        sound_siren_stop();
        return;
    }
    if (--saucer.fire_cd <= 0) {
        saucer.fire_cd = SAUCER_FIRE_CD;

        int alive_list[2], alive_count = 0;
        for (int p = 0; p < num_players; p++)
            if (ships[p].alive) alive_list[alive_count++] = p;

        if (alive_count > 0) {
            int target = alive_list[rnd(alive_count)];
            int tx = FP2PX(ships[target].x) - FP2PX(saucer.x);
            int ty = FP2PX(ships[target].y) - FP2PX(saucer.y);
            int ax = tx < 0 ? -tx : tx;
            int ay = ty < 0 ? -ty : ty;
            int dist = (ax > ay) ? (ax + ay * 106 / 256) : (ay + ax * 106 / 256);
            if (dist < 1) dist = 1;
            int spd = BULLET_SPD / 3 + (level - 1) * FP / 8; // antes 2/3 + FP/4 -- imposible de esquivar
            int bvx = (int32_t)tx * spd / dist;
            int bvy = (int32_t)ty * spd / dist;

            for (int i = 0; i < MAX_BULLETS; i++) {
                if (bullets[i].active) continue;
                bullets[i].active = true;
                bullets[i].owner  = 2;
                bullets[i].life   = BULLET_LIFE;
                bullets[i].x      = saucer.x;
                bullets[i].y      = saucer.y;
                bullets[i].vx     = bvx;
                bullets[i].vy     = bvy;
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Colisiones -- idéntico al original salvo los nombres de sonido
// ---------------------------------------------------------------------------
static bool collisions(void) {
    bool died = false;

    for (int bi=0;bi<MAX_BULLETS;bi++) {
        Bullet *b=&bullets[bi];
        if (!b->active || b->owner==2) continue;
        for (int ai=0;ai<MAX_AST;ai++) {
            if (!asts[ai].active) continue;
            if (hit(b->x,b->y,2, asts[ai].x,asts[ai].y,AST_R[asts[ai].size])) {
                ships[b->owner].score += AST_PTS[asts[ai].size];
                b->active=false;
                break_ast(ai);
                break;
            }
        }
    }
    if (saucer.active) {
        for (int bi=0;bi<MAX_BULLETS;bi++) {
            Bullet *b=&bullets[bi];
            if (!b->active || b->owner>=2) continue;
            if (hit(b->x,b->y,2, saucer.x,saucer.y,SAUCER_R)) {
                b->active=false;
                spawn_explosion(saucer.x, saucer.y, 10, FP*3, PART_LIFE_AST);
                saucer.active=false;
                next_saucer_spawn = make_timeout_time_ms(1000 * (12 + rand() % 13));
                sound_siren_stop();
                sound_effect_success();
                ships[b->owner].score += SAUCER_PTS;
            }
        }
    }
    for (int p=0;p<num_players;p++) {
        Ship *s=&ships[p];
        if (!s->alive || s->inv_ticks>0) continue;
        for (int ai=0;ai<MAX_AST;ai++) {
            if (!asts[ai].active) continue;
            if (hit(s->x,s->y,SHIP_R, asts[ai].x,asts[ai].y,AST_R[asts[ai].size])) {
                spawn_explosion(s->x, s->y, 24, FP*6, PART_LIFE_SHIP);
                s->alive=false; s->lives--; died=true;
                sound_effect_explosion();
                break_ast(ai);
                break;
            }
        }
    }
    for (int p=0;p<num_players;p++) {
        Ship *s=&ships[p];
        if (!s->alive || s->inv_ticks>0) continue;
        if (saucer.active && hit(s->x,s->y,SHIP_R, saucer.x,saucer.y,SAUCER_R)) {
            spawn_explosion(s->x, s->y, 24, FP*6, PART_LIFE_SHIP);
            s->alive=false; s->lives--; died=true;
            saucer.active=false;
            next_saucer_spawn = make_timeout_time_ms(1000 * (12 + rand() % 13));
            sound_siren_stop();
            sound_effect_explosion();
        }
        for (int bi=0;bi<MAX_BULLETS;bi++) {
            Bullet *b=&bullets[bi];
            if (!b->active || b->owner!=2) continue;
            if (hit(b->x,b->y,2, s->x,s->y,SHIP_R)) {
                b->active=false;
                spawn_explosion(s->x, s->y, 24, FP*6, PART_LIFE_SHIP);
                s->alive=false; s->lives--; died=true;
                sound_effect_explosion();
            }
        }
    }
    return died;
}

// ---------------------------------------------------------------------------
// Física de nave -- CON TIEMPO DELTA (ver cabecera del archivo):
// s->x/s->y avanzan según velocidad * g_dt_scale, no velocidad tal
// cual, así que el movimiento real no depende de cuánto tarde este
// tick en concreto en dibujarse.
// ---------------------------------------------------------------------------
static bool update_ship(int p, int enc, bool thrust, bool fire_btn, bool hyper_btn) {
    Ship *s = &ships[p];
    if (!s->alive) return false;
    if (s->hyper_cd>0)     s->hyper_cd--;
    if (s->inv_ticks>0)    s->inv_ticks--;

    if (enc) s->angle = (s->angle - (enc>0?1:-1) + ANGLE_STEPS) % ANGLE_STEPS;

    s->thrusting = thrust;
    if (thrust) {
        s->vx += SHIP_THRUST * SINV(s->angle) / FP * g_dt_scale / FP;
        s->vy -= SHIP_THRUST * COSV(s->angle) / FP * g_dt_scale / FP;
        int svx = s->vx/FP, svy = s->vy/FP;
        int spd2 = svx*svx + svy*svy;
        int maxv = SHIP_MAX_SPD/FP;
        if (spd2 > maxv*maxv) {
            int spd = 1; while (spd*spd < spd2) spd++;
            s->vx = s->vx * maxv / spd;
            s->vy = s->vy * maxv / spd;
        }
    }
    s->vx = s->vx * FRIC_NUM / FRIC_DEN;
    s->vy = s->vy * FRIC_NUM / FRIC_DEN;
    s->x += s->vx * g_dt_scale / FP;
    s->y += s->vy * g_dt_scale / FP;
    wrap(&s->x, &s->y);

    if (fire_btn) {
        if (!s->fire_held) {
            fire(p);
            s->fire_held = true;
        }
    } else {
        s->fire_held = false;
    }

    if (hyper_btn) return hyperdrive(p);
    return false;
}

// ---------------------------------------------------------------------------
// IA de la demo -- idéntica al original
// ---------------------------------------------------------------------------
static void demo_ai(int p) {
    Ship *s = &ships[p];
    if (!s->alive) return;

    int best=-1, bd2=0x7fffffff;
    for (int i=0;i<MAX_AST;i++) {
        if (!asts[i].active) continue;
        int dx=FP2PX(asts[i].x)-FP2PX(s->x), dy=FP2PX(asts[i].y)-FP2PX(s->y);
        int d2=dx*dx+dy*dy;
        if (d2<bd2) { bd2=d2; best=i; }
    }
    if (best<0) return;
    int dx=FP2PX(asts[best].x)-FP2PX(s->x), dy=FP2PX(asts[best].y)-FP2PX(s->y);
    int ta=0, bdot=-0x7fff;
    for (int a=0;a<32;a++) {
        int dot=dx*COSV(a)/FP+dy*SINV(a)/FP;
        if (dot>bdot) { bdot=dot; ta=a; }
    }
    int diff=(ta-s->angle+ANGLE_STEPS)%ANGLE_STEPS;
    int enc_in = 0;
    if (diff>1  && diff<16) enc_in = -1;
    else if (diff>16 && diff<31) enc_in =  1;
    bool thrust   = (bd2 > 60*60);
    bool fire_btn = (diff<=2 || diff>=30);
    update_ship(p, enc_in, thrust, fire_btn, false);
}

// ---------------------------------------------------------------------------
// Balas -- con tiempo delta, igual que el resto de la física
// ---------------------------------------------------------------------------
static void update_bullets(void) {
    for (int i=0;i<MAX_BULLETS;i++) {
        Bullet *b=&bullets[i];
        if (!b->active) continue;
        b->x += b->vx * g_dt_scale / FP;
        b->y += b->vy * g_dt_scale / FP;
        wrap(&b->x, &b->y);
        if (--b->life <= 0) b->active = false;
    }
}

static void update_asteroids(void) {
    for (int i=0;i<MAX_AST;i++) {
        Asteroid *a=&asts[i];
        if (!a->active) continue;
        a->x += a->vx * g_dt_scale / FP;
        a->y += a->vy * g_dt_scale / FP;
        wrap(&a->x, &a->y);
    }
}

// ---------------------------------------------------------------------------
// Tick principal
// ---------------------------------------------------------------------------
static void as_tick(void) {
    blink++;
    update_dt_scale();

    if (demo) {
        bool any = controls_menu_select()
                || controls_get_raw_delta(0) != 0
                || controls_button_down(PIN_BTN_J1_B);
        if (any || ++demo_ticks >= TICKS_S * 40) {
            g_done = true;
            return;
        }
    }

    switch (state) {

    // ------------------------------------------------------------------
    case AS_SELECT: {
        int d = controls_get_raw_delta(0);
        if (d) {
            menu_enc_acc += d;
            if (menu_enc_acc >= 4)  { num_players = (num_players==1)?2:1; menu_enc_acc = 0; draw_select_screen(); }
            if (menu_enc_acc <= -4) { num_players = (num_players==1)?2:1; menu_enc_acc = 0; draw_select_screen(); }
        }
        if (controls_menu_select()) {
            level = 1;
            ships[0].lives = 3; ships[0].score = 0;
            ships[1].lives = 3; ships[1].score = 0;
            spawn_ship(0);
            if (num_players==2) spawn_ship(1); else ships[1].alive = false;
            level_init();
            reset_render_trace();
            sound_stop_menu_music();
            state = AS_PLAYING;
        }
        break;
    }

    // ------------------------------------------------------------------
    case AS_PLAYING: {
        bool hyper_died = false;
        if (demo) {
            demo_ai(0);
        } else {
            int d0 = -controls_get_raw_delta(0);
            if (update_ship(0, d0,
                        controls_button_down(BTN_J1_B),
                        controls_button_down(BTN_J1_A),
                        controls_button_pressed(BTN_IDX_ENC1_SW))) hyper_died = true;
            if (num_players==2) {
                int d1 = -controls_get_raw_delta(1);
                if (update_ship(1, d1,
                            controls_button_down(BTN_J2_B),
                            controls_button_down(BTN_J2_A),
                            controls_button_pressed(BTN_IDX_ENC2_SW))) hyper_died = true;
            }
        }

        update_bullets();
        update_asteroids();
        update_particles();
        update_saucer();

        // Tic-tac ambiental (referencia clásica) -- 2 notas alternas
        if (--tictac_cd <= 0) {
            int n = count_asts();
            int period = TICKS_S/2 + n*TICKS_S/8;
            if (period < TICKS_S/6) period = TICKS_S/6;
            tictac_cd = period;
            sound_play_tone((blink & 1) ? 220 : 180, 40);
        }

        bool died = collisions() || hyper_died;

        if (died) {
            bool any_alive = false, any_lives = false;
            for (int p=0;p<num_players;p++) {
                if (ships[p].alive) any_alive = true;
                if (ships[p].lives > 0) any_lives = true;
            }
            if (!any_alive) {
                if (any_lives) {
                    pause_ticks = SHIP_DEATH_PAUSE;
                    state = AS_SHIP_DEAD;
                } else {
                    sound_siren_stop(); // por si el OVNI seguía en pantalla
                    sound_effect_game_over();
                    draw_playing_frame();
                    for (int p=0;p<num_players;p++) {
                        if (!demo && highscores_is_top(AS_GAME_ID, ships[p].score)) {
                            highscores_enter(AS_GAME_ID, ships[p].score); // bloqueante
                        }
                    }
                    pause_ticks = 0;
                    state = AS_GAME_OVER;
                }
            }
        }

        if (count_asts() == 0 && !died) {
            sound_effect_success();
            pause_ticks = TICKS_S * 3;
            state = AS_LEVEL_CLEAR;
        }

        draw_playing_frame();
        break;
    }

    // ------------------------------------------------------------------
    case AS_SHIP_DEAD:
        if (--pause_ticks <= 0) {
            for (int p=0;p<num_players;p++)
                if (!ships[p].alive && ships[p].lives>0) spawn_ship(p);
            state = AS_PLAYING;
        }
        draw_playing_frame();
        break;

    // ------------------------------------------------------------------
    case AS_LEVEL_CLEAR:
        if (--pause_ticks <= 0) {
            level++;
            for (int p=0;p<num_players;p++) if (ships[p].lives>0) spawn_ship(p);
            level_init();
            state = AS_PLAYING;
        }
        draw_playing_frame();
        break;

    // ------------------------------------------------------------------
    case AS_GAME_OVER:
        if (++pause_ticks > TICKS_S) {
            if (controls_menu_select() || pause_ticks > TICKS_S*8) {
                pause_ticks = 0;
                state = AS_SCORES;
                draw_scores_screen();
            }
        }
        break;

    // ------------------------------------------------------------------
    case AS_SCORES:
        if (++pause_ticks > TICKS_S*8) g_done = true;
        if (controls_menu_select()) g_done = true;
        break;
    }
}

// ---------------------------------------------------------------------------
// API pública
// ---------------------------------------------------------------------------
void game_asteroids_run(game_mode_t mode) {
    srand(time_us_32());

    demo = (mode == GAME_MODE_DEMO);
    num_players = (mode == GAME_MODE_2P) ? 2 : 1;
    blink = 0;
    demo_ticks = 0;
    menu_enc_acc = 0;
    g_done = false;
    field_needs_redraw = true;
    memset(asts, 0, sizeof(asts));
    memset(bullets, 0, sizeof(bullets));
    memset(parts, 0, sizeof(parts));
    saucer.active = false;
    sound_siren_stop(); // por si veníamos de una partida con el OVNI sonando
    reset_render_trace();
    last_tick_time = get_absolute_time();

    if (demo) {
        level = 1;
        ships[0].lives = 1; ships[0].score = 0;
        ships[1].alive = false;
        spawn_ship(0);
        level_init();
        state = AS_PLAYING;
    } else {
        state = AS_SELECT;
        draw_select_screen();
        sound_start_menu_music();
    }

    while (!g_done) {
        controls_update();
        as_tick();
        sound_update();
        sleep_ms(8);
    }

    highscores_flush();
}