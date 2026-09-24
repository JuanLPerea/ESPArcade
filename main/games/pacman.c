/**
 * pacman.c — Pac-Man para ArcadeColor (Pi Pico + ST7789), 1-2 jugadores
 *
 * PORT NOTES (de la versión "pacman2p.c" original a pantalla grande, al
 * patrón que usa asteroids.c en ArcadeColor):
 *
 *  1) RESOLUCIÓN. El laberinto original es de 19×21 celdas y necesita más
 *     alto que ancho (19:22 ≈ 0.86), mientras que la pantalla física de
 *     ArcadeColor es 320×240 en horizontal (mucho más ancha que alta,
 *     4:3 apaisado). Ni con HUD arriba/abajo ni con celdas pequeñas cabe
 *     el mapa completo en 240px de alto en horizontal (22 filas × 11px
 *     mínimos ya son 242px, sin dejar nada para HUD).
 *
 *     Por eso este port ROTA LA PANTALLA A VERTICAL (240×320) solo
 *     mientras dura la partida, usando st7789_set_rotation() (documentada
 *     en st7789.h para exactamente este caso). Al salir del juego se
 *     restaura la rotación horizontal para el resto de ArcadeColor.
 *     Con pantalla 240×320: CELL=12px → mapa 228×264px, dejando 56px
 *     repartidos arriba/abajo para el HUD.
 *
 *  2) SPRITES. Los sprites originales eran mapas de bits de 32×32px,
 *     pensados para celdas de ~21px (el sprite es intencionalmente más
 *     grande que la celda). A CELL=12px esos sprites no caben de ninguna
 *     manera. Se sustituyen por dibujo vectorial sencillo (círculos por
 *     filas + cuña para la boca de Pac-Man, cúpula+cuerpo para los
 *     fantasmas), en la misma línea de "gráficos simples" que usa
 *     asteroids.c en vez de pixel-art detallado.
 *
 *  3) ENTRADA. La versión original leía variables globales volátiles
 *     rellenadas por una ISR (enc1_count, btn1_pressed, ...) y un menú de
 *     callbacks (set_draw_callback/set_tick_callback). Aquí se usa la
 *     API actual de controls.c (controls_update/controls_get_raw_delta/
 *     controls_button_pressed/controls_button_down/controls_menu_*) y un
 *     bucle propio game_pacman_run(), igual que game_asteroids_run().
 *
 *  4) RECORDS. La entrada de iniciales ya no es un estado de la máquina
 *     de estados del juego (ST_ENTER_NAME) alimentado por hs_input.h;
 *     se usa la highscores_enter() BLOQUEANTE de highscores.c, igual que
 *     hace asteroids.c, justo antes de pasar a la pantalla de GAME OVER.
 *
 *  5) TIEMPO REAL / DT. Igual que asteroids.c, el bucle mide el tiempo
 *     real transcurrido por vuelta y escala el movimiento en píxeles con
 *     él (g_dt_scale), para que la velocidad no dependa de cuánto tarde
 *     el SPI en cada flush. Los temporizadores en "ticks" (frightened,
 *     scatter/chase, pausas...) se mantienen como cuenta de iteraciones
 *     del bucle, igual que en el original y en asteroids.c — no son
 *     tiempo real exacto, pero es el mismo criterio ya usado en el resto
 *     de ArcadeColor.
 *
 *  6) JUGABILIDAD SIN CAMBIOS: mapa, IA de fantasmas (scatter/chase/
 *     frightened/dead, objetivos por fantasma, BFS de vuelta a casa),
 *     física de movimiento sub-píxel, túnel lateral, puntuaciones y
 *     reglas de 1/2 jugadores se han portado tal cual desde el original.
 *
 * CONTROLES:
 *   J1: encoder 1 = arriba/abajo · BTN_J1_A = izquierda · BTN_J1_B = derecha
 *   J2: encoder 2 = arriba/abajo · BTN_J2_A = izquierda · BTN_J2_B = derecha
 *   Mantener pulsados ambos SW de encoder = salir al menú
 *
 *  7) FANTASMAS GIRADOS. El cuerpo del fantasma (cúpula + patas
 *     dentadas) es una forma FIJA que no depende de hacia dónde se
 *     mueve -- igual que en el arcade original -- así que draw_ghost_
 *     body()/draw_ghost_eyes() dibujan esa silueta ya girada 90° en el
 *     sentido de las agujas del reloj (cúpula a la derecha, patas a la
 *     izquierda) para que se vea coherente en la pantalla vertical.
 *     La pupila (mirada) SÍ sigue la dirección real de movimiento, sin
 *     girar, igual que la boca de Pac-Man.
 *
 *  8) ROTACIÓN DE PANTALLA EN LAS PANTALLAS COMPARTIDAS. highscores_
 *     draw()/highscores_enter() son de highscores.c, comunes a todos
 *     los juegos, y están pensadas para la orientación apaisada
 *     estándar (rotation=1) con la que las llaman el resto de juegos.
 *     Como este archivo pone rotation=0 (vertical) al entrar y no lo
 *     restaura hasta el final de game_pacman_run(), esas pantallas
 *     compartidas se dibujaban con la rotación equivocada. Por eso se
 *     vuelve a rotation=1 justo antes de usarlas (ver pm_tick()).
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "pacman.h"
#include "renderer.h"
#include "controls.h"
#include "highscores.h"
#include "sound.h"

// ─────────────────────────────────────────────────────────────────────────────
// Pantalla: rotada a vertical durante la partida (ver notas de cabecera)
// ─────────────────────────────────────────────────────────────────────────────
#define SCREEN_W   240
#define SCREEN_H   320

// Dimensiones bajo rotation=1 (apaisada), NO las de Pac-Man -- solo se
// usan para la línea de ayuda de PM_SCORES, que se dibuja con esa
// rotación activa (ver nota 8) en la cabecera del archivo).
#define LANDSCAPE_W 320
#define LANDSCAPE_H 240
#define TICKS_S    62      // igual que el original: constante nominal para
                            // expresar duraciones en "ticks" (no es la tasa
                            // real del bucle, igual que en asteroids.c)

// ─────────────────────────────────────────────────────────────────────────────
// Geometría del mapa: 19 cols × 22 filas, celdas 12×12px
// ─────────────────────────────────────────────────────────────────────────────
#define MAP_COLS   19
#define MAP_ROWS   22
#define CELL       12

#define MAP_W      (MAP_COLS * CELL)              // 228
#define MAP_H      (MAP_ROWS * CELL)               // 264
#define MAP_X      ((SCREEN_W - MAP_W) / 2)        // 6
#define MAP_Y       34                             // deja hueco para HUD superior

#define HUD_TOP_Y   2
#define HUD_BOT_Y  (MAP_Y + MAP_H + 4)              // 302, quedan 18px hasta 320

// Centro en pantalla de la celda lógica (c, r)
#define CELL_CX(c)   (MAP_X + (c)*CELL + CELL/2)
#define CELL_CY(r)   (MAP_Y + (r)*CELL + CELL/2)

// ─────────────────────────────────────────────────────────────────────────────
// Tipos de celda
// ─────────────────────────────────────────────────────────────────────────────
#define C_WALL   1
#define C_PATH   0
#define C_DOOR   2
#define C_POWER  3
#define C_EMPTY  4

// ─────────────────────────────────────────────────────────────────────────────
// Laberinto 19×22 (idéntico al original)
// ─────────────────────────────────────────────────────────────────────────────
static const uint8_t MAP_ORIG[MAP_ROWS][MAP_COLS] = {
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,1},
    {1,C_POWER,1,1,1,1,0,1,0,1,0,1,0,1,1,1,1,C_POWER,1},
    {1,0,1,1,1,1,0,1,0,1,0,1,0,1,1,1,1,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,1,1,0,1,0,1,1,1,1,1,0,1,0,1,1,0,1},
    {1,0,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0,0,1},
    {1,1,1,1,0,1,1,1,0,1,0,1,1,1,0,1,1,1,1},
    {1,1,1,1,0,1,0,0,0,0,0,0,0,1,0,1,1,1,1},
    {1,1,1,1,0,1,0,1,1,2,1,1,0,1,0,1,1,1,1},
    {0,0,0,0,0,0,0,1,0,0,0,1,0,0,0,0,0,0,0},
    {1,1,1,1,0,1,0,1,1,1,1,1,0,1,0,1,1,1,1},
    {1,1,1,1,0,1,0,0,0,C_EMPTY,0,0,0,1,0,1,1,1,1},
    {1,1,1,1,0,1,1,1,0,1,0,1,1,1,0,1,1,1,1},
    {1,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,1},
    {1,C_POWER,1,1,1,1,0,1,1,1,1,1,0,1,1,1,1,C_POWER,1},
    {1,0,0,0,1,0,0,0,0,C_EMPTY,0,0,0,0,1,0,0,0,1},
    {1,1,1,0,1,0,1,1,0,1,0,1,1,0,1,0,1,1,1},
    {1,0,0,0,0,0,1,0,0,1,0,0,1,0,0,0,0,0,1},
    {1,0,1,1,1,0,1,0,1,1,1,0,1,0,1,1,1,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
};

// Tabla BFS precomputada: dirección hacia el interior de la casa (fila 10,
// col 9) para un fantasma muerto. 0=target,1=UP,2=DOWN,3=LEFT,4=RIGHT
static const uint8_t DEAD_DIR[MAP_ROWS][MAP_COLS] = {
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,4,4,4,4,4,2,3,3,0,4,4,2,3,3,3,3,3,0},
    {0,2,0,0,0,0,2,0,2,0,2,0,2,0,0,0,0,2,0},
    {0,2,0,0,0,0,2,0,2,0,2,0,2,0,0,0,0,2,0},
    {0,4,4,4,4,4,2,3,3,3,4,4,2,3,3,3,3,3,0},
    {0,1,0,0,1,0,2,0,0,0,0,0,2,0,1,0,0,1,0},
    {0,4,4,4,1,0,4,4,2,0,2,3,3,0,1,3,3,3,0},
    {0,0,0,0,2,0,0,0,2,0,2,0,0,0,2,0,0,0,0},
    {0,0,0,0,2,0,4,4,4,2,3,3,3,0,2,0,0,0,0},
    {0,0,0,0,2,0,1,0,0,2,0,0,1,0,2,0,0,0,0},
    {4,4,4,4,4,4,1,0,4,0,3,0,1,3,3,3,3,3,3},
    {0,0,0,0,1,0,1,0,0,0,0,0,1,0,1,0,0,0,0},
    {0,0,0,0,1,0,1,3,3,3,4,4,1,0,1,0,0,0,0},
    {0,0,0,0,1,0,0,0,1,0,1,0,0,0,1,0,0,0,0},
    {0,4,4,4,1,3,4,4,1,0,1,3,3,4,1,3,3,3,0},
    {0,1,0,0,0,0,1,0,0,0,0,0,1,0,0,0,0,1,0},
    {0,1,3,3,0,4,1,3,3,3,4,4,1,3,0,4,4,1,0},
    {0,0,0,1,0,1,0,0,1,0,1,0,0,1,0,1,0,0,0},
    {0,4,4,4,4,1,0,4,1,0,1,3,0,1,3,3,3,3,0},
    {0,1,0,0,0,1,0,1,0,0,0,1,0,1,0,0,0,1,0},
    {0,4,4,4,4,1,3,3,3,3,4,1,4,1,3,3,3,3,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
};

static uint8_t map[MAP_ROWS][MAP_COLS];
static int dots_left, total_dots;

static int spd_fright;
static int spd_tunnel;

static uint8_t wall_flags[MAP_ROWS][MAP_COLS];

static void precompute_wall_flags(void) {
    for (int r = 0; r < MAP_ROWS; r++) {
        for (int c = 0; c < MAP_COLS; c++) {
            if (MAP_ORIG[r][c] != C_WALL) { wall_flags[r][c] = 0; continue; }
            uint8_t f = 0;
            if (r > 0          && (MAP_ORIG[r-1][c] == C_WALL || MAP_ORIG[r-1][c] == C_DOOR)) f |= 8;
            if (r < MAP_ROWS-1 && (MAP_ORIG[r+1][c] == C_WALL || MAP_ORIG[r+1][c] == C_DOOR)) f |= 4;
            if (c < MAP_COLS-1 && (MAP_ORIG[r][c+1] == C_WALL || MAP_ORIG[r][c+1] == C_DOOR)) f |= 2;
            if (c > 0          && (MAP_ORIG[r][c-1] == C_WALL || MAP_ORIG[r][c-1] == C_DOOR)) f |= 1;
            wall_flags[r][c] = f;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Velocidades en 1/8 de píxel por tick (SPD_UNIT = 8 → 1.0 px/tick).
// Tabla y fórmulas idénticas al original.
// ─────────────────────────────────────────────────────────────────────────────
#define SPD_UNIT  8

static int level_speed(int lvl) {
    int s = SPD_UNIT + (lvl - 1);
    if (s > SPD_UNIT * 2 + 4) s = SPD_UNIT * 2 + 4;
    return s;
}
static int fright_speed(int lvl) {
    int base = level_speed(lvl);
    int fs = (base * 55) / 100;
    if (fs < 5) fs = 5;
    return fs;
}
static int tunnel_speed(int lvl) {
    int base = level_speed(lvl);
    int ts = (base * 40) / 100;
    if (ts < 4) ts = 4;
    return ts;
}

// ─────────────────────────────────────────────────────────────────────────────
// Direcciones
// ─────────────────────────────────────────────────────────────────────────────
typedef enum { DIR_NONE=0, DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT } Dir;
static const int8_t DX[5] = {0, 0, 0,-1, 1};
static const int8_t DY[5] = {0,-1, 1, 0, 0};

static Dir opp(Dir d) {
    switch(d){
    case DIR_UP:   return DIR_DOWN;  case DIR_DOWN:  return DIR_UP;
    case DIR_LEFT: return DIR_RIGHT; case DIR_RIGHT: return DIR_LEFT;
    default:       return DIR_NONE;
    }
}

typedef enum { GM_SCATTER,GM_CHASE,GM_FRIGHTENED,GM_DEAD,GM_HOME,GM_LEAVING } GhostMode;

typedef struct {
    int    px, py;
    int    tx, ty;
    Dir    dir;
    Dir    want;
    bool   moving;
    int8_t anim;
    int    anim_timer;
    bool   alive;
    int    sub_x, sub_y;
} Pacman;

typedef struct {
    int    px, py;
    int    tx, ty;
    Dir    dir;
    GhostMode mode;
    int    home_timer;
    bool   visible;
    int8_t anim;
    int    anim_timer;
    int    sub_x, sub_y;
} Ghost;

// ─────────────────────────────────────────────────────────────────────────────
// Estado del juego (sin ST_ENTER_NAME: se resuelve con highscores_enter()
// bloqueante, como en asteroids.c)
// ─────────────────────────────────────────────────────────────────────────────
typedef enum {
    PM_SELECT, PM_READY, PM_PLAYING,
    PM_DEAD, PM_LEVELUP, PM_GAMEOVER, PM_SCORES
} PmState;

static PmState state;
static int    blink;
static bool   demo_mode;
static int    demo_ticks;
static int    pause_cnt;
static int    score, lives, level;
static int    spd;
static Pacman pac;
static Pacman pac2;
static Ghost  gh[4];
static int    frighten_timer;
static int    ghost_combo;
static int    bonus_visible;
static int    bonus_spawn;
static int    bonus_type;
static int    scatter_timer;
static bool   scatter_phase;
static int    enc_acc;
static int    enc2_acc;
static int    waa_phase;

static bool   two_player;
static int    score2;
static int    lives2;
static bool   p2_dead_anim;
static int    p2_dead_cnt;
static int    bonus_winner;

static int8_t demo_nearest_c;
static int8_t demo_nearest_r;

static bool   g_done;             // sustituye a g_game_done
static uint8_t g_prev_rotation;   // rotación a restaurar al salir

#define T_FRIGHTEN  (7*TICKS_S)
#define T_SCATTER   (7*TICKS_S)
#define T_CHASE     (20*TICKS_S)
#define T_BONUS_VIS  (12*TICKS_S)
#define T_BONUS_LO   (8*TICKS_S)
#define T_BONUS_HI   (25*TICKS_S)
#define T_DEAD_W    (1*TICKS_S)
#define T_READY     (1*TICKS_S)
#define T_LEVELUP   (1*TICKS_S)
#define T_GAMEOVER  (1*TICKS_S)

#define PTS_DOT    10
#define PTS_POWER  50
#define PTS_FRUIT 100
#define PTS_BONUS 500

// ─────────────────────────────────────────────────────────────────────────────
// Escala por tiempo real (dt), igual criterio que asteroids.c: el bucle mide
// cuánto ha tardado realmente la vuelta anterior y escala el movimiento en
// píxeles para que no dependa de la duración variable del flush SPI.
// DT_UNIT=256 representa "1 vuelta nominal de 16ms" (Q8.8-ish, solo para esto).
// ─────────────────────────────────────────────────────────────────────────────
#define DT_UNIT       256
#define DT_NOMINAL_MS 16
static int g_dt_scale = DT_UNIT;

static void update_dt_scale(uint32_t elapsed_ms) {
    if (elapsed_ms < 1)  elapsed_ms = 1;
    if (elapsed_ms > 64) elapsed_ms = 64;  // evita saltos enormes tras una pausa larga
    g_dt_scale = (int)((elapsed_ms * DT_UNIT) / DT_NOMINAL_MS);
    if (g_dt_scale < DT_UNIT / 4) g_dt_scale = DT_UNIT / 4;
}

#define TUNNEL_ROW  10

static void tunnel_wrap(int8_t *c, int8_t r) {
    if (r != TUNNEL_ROW) return;
    if (*c < 0)         *c = MAP_COLS-1;
    if (*c >= MAP_COLS) *c = 0;
}
static bool pac_ok(int8_t c, int8_t r) {
    tunnel_wrap(&c, r);
    if (c<0||c>=MAP_COLS||r<0||r>=MAP_ROWS) return false;
    return map[r][c]!=C_WALL && map[r][c]!=C_DOOR;
}
static bool ghost_ok(int8_t c, int8_t r, bool door) {
    tunnel_wrap(&c, r);
    if (c<0||c>=MAP_COLS||r<0||r>=MAP_ROWS) return false;
    if (map[r][c]==C_WALL) return false;
    if (map[r][c]==C_DOOR) return door;
    return true;
}

static bool in_tunnel_zone(int px_pos, int py_pos) {
    int row = (py_pos - MAP_Y) / CELL;
    if (row != TUNNEL_ROW) return false;
    int col = (px_pos - MAP_X) / CELL;
    if (px_pos < MAP_X || px_pos >= MAP_X + MAP_W) return true;
    return (col <= 3) || (col >= MAP_COLS - 4);
}

static bool tunnel_teleport(int *px_pos, int *py_pos, int *tx, int *ty, Dir dir) {
    int row = (*py_pos - MAP_Y) / CELL;
    if (row != TUNNEL_ROW) return false;
    int col = (*px_pos - MAP_X) / CELL;

    if (col == 0 && dir == DIR_LEFT) {
        int dest = MAP_X + (MAP_COLS-1)*CELL + CELL/2;
        *px_pos = dest; *tx = dest;
        return true;
    }
    if (col == MAP_COLS-1 && dir == DIR_RIGHT) {
        int dest = MAP_X + 0*CELL + CELL/2;
        *px_pos = dest; *tx = dest;
        return true;
    }
    return false;
}

// Movimiento suave con velocidad en 1/8 px/tick, ESCALADA por g_dt_scale
// (mismo patrón que la física de asteroids.c). Devuelve true si llegó.
static bool move_toward_fp(int *pos, int target, int spd8, int *sub) {
    int d = target - *pos;
    if (d == 0 && *sub == 0) return true;

    int scaled = (spd8 * g_dt_scale) / DT_UNIT;
    if (scaled < 1) scaled = 1;

    *sub += scaled;
    int steps = *sub / SPD_UNIT;
    *sub %= SPD_UNIT;

    if (steps <= 0) return (d == 0);

    if (d > 0) {
        if (steps > d) steps = d;
        *pos += steps;
    } else if (d < 0) {
        int nd = -d;
        if (steps > nd) steps = nd;
        *pos -= steps;
    }
    return *pos == target;
}

static int col_to_px(int c) { return MAP_X + c*CELL + CELL/2; }
static int row_to_py(int r) { return MAP_Y + r*CELL + CELL/2; }
static int px_to_col(int px) { return (px - MAP_X) / CELL; }
static int px_to_row(int py) { return (py - MAP_Y) / CELL; }

#define BONUS_COL  9
#define BONUS_ROW  12

static void map_reset(void) {
    memcpy(map, MAP_ORIG, sizeof(map));

    for (int c=0;c<MAP_COLS;c++)
        if (map[TUNNEL_ROW][c]==C_PATH) map[TUNNEL_ROW][c]=C_EMPTY;

    static const uint8_t HOUSE[][2] = {
        {7,7},{7,8},{7,9},{7,10},{7,11},
        {9,8},{9,9},{9,10},
        {11,7},{11,8},{11,9},{11,10},{11,11}
    };
    for (int i=0;i<(int)(sizeof(HOUSE)/2);i++)
        if (map[HOUSE[i][0]][HOUSE[i][1]]==C_PATH)
            map[HOUSE[i][0]][HOUSE[i][1]]=C_EMPTY;

    static const uint8_t DEAD[][2] = {
        {7,1},{7,2},{7,16},{7,17},
        {11,1},{11,2},{11,16},{11,17}
    };
    for (int i=0;i<(int)(sizeof(DEAD)/2);i++)
        if (map[DEAD[i][0]][DEAD[i][1]]==C_PATH)
            map[DEAD[i][0]][DEAD[i][1]]=C_EMPTY;

    dots_left = 0;
    for (int r=0;r<MAP_ROWS;r++)
        for (int c=0;c<MAP_COLS;c++)
            if (map[r][c]==C_PATH||map[r][c]==C_POWER) dots_left++;
    total_dots = dots_left;
}

static void pac_reset(void) {
    int c0=8, r0=16;
    pac.px = col_to_px(c0);  pac.py = row_to_py(r0);
    pac.tx = pac.px;          pac.ty = pac.py;
    pac.dir = DIR_LEFT;       pac.want = DIR_LEFT;
    pac.moving = false;
    pac.anim = 0;             pac.anim_timer = 0;
    pac.alive = true;
    pac.sub_x = 0;            pac.sub_y = 0;
}

static void pac2_reset(void) {
    int c0=10, r0=16;
    pac2.px = col_to_px(c0); pac2.py = row_to_py(r0);
    pac2.tx = pac2.px;        pac2.ty = pac2.py;
    pac2.dir = DIR_RIGHT;     pac2.want = DIR_RIGHT;
    pac2.moving = false;
    pac2.anim = 0;            pac2.anim_timer = 0;
    pac2.alive = true;
    pac2.sub_x = 0;           pac2.sub_y = 0;
}

static void ghost_reset(int i) {
    int c, r;
    switch(i) {
    case 0: c=9;  r=10; gh[i].mode=GM_HOME; gh[i].home_timer=TICKS_S/2;  break;
    case 1: c=8;  r=10; gh[i].mode=GM_HOME; gh[i].home_timer=TICKS_S*2;  break;
    case 2: c=9;  r=10; gh[i].mode=GM_HOME; gh[i].home_timer=TICKS_S*4;  break;
    case 3: c=10; r=10; gh[i].mode=GM_HOME; gh[i].home_timer=TICKS_S*6;  break;
    }
    gh[i].px = col_to_px(c);  gh[i].py = row_to_py(r);
    gh[i].tx = gh[i].px;      gh[i].ty = gh[i].py;
    gh[i].dir = DIR_LEFT;
    gh[i].visible = true;     gh[i].anim = 0;  gh[i].anim_timer = 0;
    gh[i].sub_x = 0;          gh[i].sub_y = 0;
}

static void demo_update_nearest(void);

static void level_init(void) {
    map_reset();
    pac_reset();
    if (two_player) pac2_reset();
    for (int i=0;i<4;i++) ghost_reset(i);
    spd = level_speed(level);
    spd_fright  = fright_speed(level);
    spd_tunnel  = tunnel_speed(level);
    frighten_timer=0; ghost_combo=0; waa_phase=0;
    bonus_visible=0; bonus_type = (level-1) % 6;
    bonus_spawn = T_BONUS_LO + rand()%(T_BONUS_HI - T_BONUS_LO);
    scatter_timer=T_SCATTER; scatter_phase=true; enc_acc=0; enc2_acc=0;
    bonus_winner=0;
    p2_dead_anim=false; p2_dead_cnt=0;
    demo_nearest_c = 0; demo_nearest_r = 0;
    if (demo_mode) demo_update_nearest();
}

// ─────────────────────────────────────────────────────────────────────────────
// IA de fantasmas
// ─────────────────────────────────────────────────────────────────────────────
#define absi(x)       ((x) < 0 ? -(x) : (x))
#define manh(ac,ar,bc,br) (absi((ac)-(bc)) + absi((ar)-(br)))

// Distancia REAL por el laberinto (BFS) desde el objetivo (tc,tr) a cada
// celda alcanzable, respetando paredes/puerta -- best_towards() usaba
// antes solo la distancia en línea recta (Manhattan) al candidato, que
// ignora las paredes por completo salvo el paso inmediato. Eso es fiel
// al comportamiento del arcade original (que se diseñó SOBRE ese laberinto
// concreto sabiendo que el atajo funcionaría), pero en un laberinto propio
// hace que el fantasma elija a menudo un paso que "en línea recta" parece
// acercarlo pero en realidad lleva a un callejón sin salida o a un rodeo
// más largo -- se ve como si evitase a Pac-Man en vez de perseguirlo. Con
// BFS el fantasma siempre elige el paso que de verdad acorta el camino.
static int16_t bfs_dist[MAP_ROWS][MAP_COLS];
static void bfs_from(int tc, int tr, bool door) {
    for (int r=0;r<MAP_ROWS;r++)
        for (int c=0;c<MAP_COLS;c++)
            bfs_dist[r][c] = -1;
    if (tc<0||tc>=MAP_COLS||tr<0||tr>=MAP_ROWS) return;

    static int8_t qc[MAP_ROWS*MAP_COLS], qr[MAP_ROWS*MAP_COLS];
    int head=0, tail=0;
    bfs_dist[tr][tc]=0;
    qc[tail]=(int8_t)tc; qr[tail]=(int8_t)tr; tail++;

    static const Dir P4[4]={DIR_UP,DIR_DOWN,DIR_LEFT,DIR_RIGHT};
    while (head<tail) {
        int8_t c=qc[head], r=qr[head]; head++;
        int16_t d = bfs_dist[r][c];
        for (int k=0;k<4;k++) {
            int8_t nc=(int8_t)(c+DX[P4[k]]), nr=(int8_t)(r+DY[P4[k]]);
            tunnel_wrap(&nc,nr);
            if (nc<0||nc>=MAP_COLS||nr<0||nr>=MAP_ROWS) continue;
            if (!ghost_ok(nc,nr,door)) continue;
            if (bfs_dist[nr][nc] != -1) continue;
            bfs_dist[nr][nc] = (int16_t)(d+1);
            qc[tail]=nc; qr[tail]=nr; tail++;
        }
    }
}

static Dir best_towards(int8_t fc,int8_t fr,int tc,int tr,Dir from,bool door) {
    static const Dir P[4]={DIR_UP,DIR_LEFT,DIR_DOWN,DIR_RIGHT};
    bfs_from(tc, tr, door);
    Dir best=DIR_NONE; int bd=99999;
    for (int i=0;i<4;i++) {
        Dir d=P[i]; if(d==opp(from)) continue;
        int8_t nc=fc+DX[d], nr=fr+DY[d]; tunnel_wrap(&nc,nr);
        if (!ghost_ok(nc,nr,door)) continue;
        int dist = bfs_dist[nr][nc];
        // El objetivo puede quedar temporalmente inalcanzable por este
        // lado de una puerta (p.ej. en GM_LEAVING antes de cruzarla) --
        // en ese caso, línea recta como último recurso en vez de tratar
        // "inalcanzable" como si fuera la mejor opción.
        if (dist<0) dist = manh(nc,nr,tc,tr) + 1000;
        if (dist<bd){bd=dist;best=d;}
    }
    return best==DIR_NONE?opp(from):best;
}

static Dir rnd_dir(int8_t fc,int8_t fr,Dir from) {
    static const Dir P[4]={DIR_UP,DIR_LEFT,DIR_DOWN,DIR_RIGHT};
    Dir opts[4]; int n=0;
    for (int i=0;i<4;i++) {
        Dir d=P[i]; if(d==opp(from)) continue;
        int8_t nc=fc+DX[d],nr=fr+DY[d]; tunnel_wrap(&nc,nr);
        if (ghost_ok(nc,nr,false)) opts[n++]=d;
    }
    return n?opts[rand()%n]:opp(from);
}

static void scatter_tgt(int i,int*tc,int*tr){
    switch(i){case 0:*tc=18;*tr=0;break; case 1:*tc=0;*tr=0;break;
              case 2:*tc=18;*tr=MAP_ROWS-1;break; default:*tc=0;*tr=MAP_ROWS-1;break;}
}
static void chase_tgt(int i,int*tc,int*tr){
    if (!two_player) {
        int pc=px_to_col(pac.px), pr=px_to_row(pac.py);
        switch(i){
        case 0: *tc=pc;*tr=pr; break;
        case 1: *tc=pc+DX[pac.dir]*4;*tr=pr+DY[pac.dir]*4; break;
        case 2:{int vx=(pc+DX[pac.dir]*2)-px_to_col(gh[0].px);
                int vy=(pr+DY[pac.dir]*2)-px_to_row(gh[0].py);
                *tc=px_to_col(gh[0].px)+vx*2; *tr=px_to_row(gh[0].py)+vy*2; break;}
        case 3: if(manh(px_to_col(gh[3].px),px_to_row(gh[3].py),pc,pr)>6)
                    {*tc=pc;*tr=pr;}
                else scatter_tgt(3,tc,tr); break;
        }
    } else {
        Pacman *p1 = pac.alive  ? &pac  : (pac2.alive ? &pac2 : &pac);
        Pacman *p2 = pac2.alive ? &pac2 : (pac.alive  ? &pac  : &pac2);
        switch(i){
        case 0: *tc=px_to_col(p1->px); *tr=px_to_row(p1->py); break;
        case 1: *tc=px_to_col(p1->px)+DX[p1->dir]*4; *tr=px_to_row(p1->py)+DY[p1->dir]*4; break;
        case 2: *tc=px_to_col(p2->px); *tr=px_to_row(p2->py); break;
        case 3: *tc=px_to_col(p2->px)+DX[p2->dir]*4; *tr=px_to_row(p2->py)+DY[p2->dir]*4; break;
        }
    }
    if(*tc<0)*tc=0; if(*tc>=MAP_COLS)*tc=MAP_COLS-1;
    if(*tr<0)*tr=0; if(*tr>=MAP_ROWS)*tr=MAP_ROWS-1;
}

static void ghost_step(int i) {
    Ghost *g = &gh[i];

    int gspd;
    if (in_tunnel_zone(g->px, g->py)) {
        gspd = spd_tunnel;
    } else if (g->mode == GM_FRIGHTENED) {
        gspd = spd_fright;
    } else if (g->mode == GM_DEAD) {
        gspd = spd * 2;
    } else {
        gspd = spd;
    }

    if (++g->anim_timer >= 12) { g->anim_timer=0; g->anim^=1; }

    bool at_tx = move_toward_fp(&g->px, g->tx, gspd, &g->sub_x);
    bool at_ty = move_toward_fp(&g->py, g->ty, gspd, &g->sub_y);

    if (!at_tx || !at_ty) return;

    if (tunnel_teleport(&g->px, &g->py, &g->tx, &g->ty, g->dir)) return;

    int col_raw = (g->px - MAP_X) / CELL;
    int row_raw = (g->py - MAP_Y) / CELL;
    int8_t col = (int8_t)col_raw;
    int8_t row = (int8_t)row_raw;
    if (col < 0 || col >= MAP_COLS || row < 0 || row >= MAP_ROWS) {
        g->px = col_to_px(9); g->py = row_to_py(TUNNEL_ROW);
        g->tx = g->px;        g->ty = g->py;
        g->dir = DIR_LEFT;
        return;
    }

    if (g->mode == GM_HOME) {
        if (row != 10) {
            Dir vert = (row < 10) ? DIR_DOWN : DIR_UP;
            int8_t nr2 = row + DY[vert];
            g->dir = vert;
            g->tx = col_to_px(col); g->ty = row_to_py(nr2);
            return;
        }
        int8_t nc = col + DX[g->dir];
        if (g->dir != DIR_LEFT && g->dir != DIR_RIGHT) g->dir = DIR_LEFT;
        nc = col + DX[g->dir];
        if (nc < 8 || nc > 10 || !ghost_ok(nc, row, false)) {
            g->dir = opp(g->dir);
            nc = col + DX[g->dir];
        }
        if (nc >= 8 && nc <= 10 && ghost_ok(nc, row, false)) {
            g->tx = col_to_px(nc);
        } else {
            g->tx = col_to_px(col);
        }
        g->ty = row_to_py(row);
        return;
    }
    if (g->mode == GM_LEAVING) {
        int tc=9, tr=8;
        if (col==tc && row<=tr){
            g->mode = scatter_phase ? GM_SCATTER : GM_CHASE;
            g->dir=DIR_LEFT;
            g->tx=col_to_px(col); g->ty=row_to_py(row);
            return;
        }
        g->dir=best_towards(col,row,tc,tr,g->dir,true);
        int8_t nc=col+DX[g->dir], nr=row+DY[g->dir]; tunnel_wrap(&nc,nr);
        g->tx=col_to_px(nc); g->ty=row_to_py(nr);
        return;
    }
    if (g->mode == GM_DEAD) {
        if (row == 10 && col >= 8 && col <= 10) {
            g->mode = GM_HOME;
            static const int respawn_delay[4] = {
                TICKS_S*3, TICKS_S*5, TICKS_S*7, TICKS_S*9
            };
            g->home_timer = respawn_delay[i];
            g->dir = DIR_LEFT;
            g->tx = col_to_px(col); g->ty = row_to_py(row);
            return;
        }
        if (col == 9 && row == 9) {
            g->dir = DIR_DOWN;
            g->tx = col_to_px(9); g->ty = row_to_py(10);
            return;
        }
        uint8_t d = DEAD_DIR[row][col];
        static const Dir DEAD_DIR_MAP[5] = {DIR_NONE, DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT};
        if (d == 0) {
            g->px = col_to_px(9); g->py = row_to_py(9);
            g->tx = col_to_px(9); g->ty = row_to_py(10);
            g->dir = DIR_DOWN;
            return;
        }
        g->dir = DEAD_DIR_MAP[d];
        int8_t nc2 = (int8_t)(col + DX[g->dir]);
        int8_t nr2 = (int8_t)(row + DY[g->dir]);
        if (!ghost_ok(nc2, nr2, true)) {
            g->px = col_to_px(9); g->py = row_to_py(9);
            g->tx = col_to_px(9); g->ty = row_to_py(10);
            g->dir = DIR_DOWN;
            return;
        }
        g->tx = col_to_px(nc2); g->ty = row_to_py(nr2);
        return;
    }

    Dir next;
    if (g->mode==GM_FRIGHTENED) {
        next=rnd_dir(col,row,g->dir);
    } else {
        int tc,tr;
        if (scatter_phase) scatter_tgt(i,&tc,&tr);
        else               chase_tgt(i,&tc,&tr);
        next=best_towards(col,row,tc,tr,g->dir,false);
    }
    g->dir=next;
    int8_t nc=col+DX[g->dir], nr=row+DY[g->dir]; tunnel_wrap(&nc,nr);
    if (!ghost_ok(nc,nr,false)) {
        g->dir=rnd_dir(col,row,DIR_NONE);
        nc=col+DX[g->dir]; nr=row+DY[g->dir]; tunnel_wrap(&nc,nr);
    }
    if (ghost_ok(nc,nr,false)) {
        g->tx=col_to_px(nc); g->ty=row_to_py(nr);
    }
}

static void demo_update_nearest(void) {
    int8_t pc = (int8_t)px_to_col(pac.px), pr = (int8_t)px_to_row(pac.py);
    int best = 99999;
    int8_t bc = 0, br = 0;
    for (int r = 0; r < MAP_ROWS; r++)
        for (int c = 0; c < MAP_COLS; c++)
            if (map[r][c] == C_PATH || map[r][c] == C_POWER) {
                int d = manh(pc, pr, c, r);
                if (d < best) { best = d; bc = (int8_t)c; br = (int8_t)r; }
            }
    demo_nearest_c = bc;
    demo_nearest_r = br;
}

static void pac_step_generic(Pacman *p, int *score_ptr, int bonus_winner_val,
                             int8_t *anim_dir_ptr) {
    if (!p->alive) return;

    bool at_tx = move_toward_fp(&p->px, p->tx, spd, &p->sub_x);
    bool at_ty = move_toward_fp(&p->py, p->ty, spd, &p->sub_y);

    if (++p->anim_timer >= 4) {
        p->anim_timer = 0;
        p->anim += *anim_dir_ptr;
        if (p->anim >= 2) { p->anim = 2; *anim_dir_ptr = -1; }
        if (p->anim <= 0) { p->anim = 0; *anim_dir_ptr =  1; }
    }

    {
        int8_t col=(int8_t)px_to_col(p->px), row=(int8_t)px_to_row(p->py);
        if (col>=0&&col<MAP_COLS&&row>=0&&row<MAP_ROWS) {
            uint8_t *cell=&map[row][col];
            if (*cell==C_PATH) {
                *cell=C_EMPTY; dots_left--; *score_ptr+=PTS_DOT;
                sound_effect_pacman_chomp(waa_phase != 0);
                waa_phase ^= 1;
                if (demo_mode) demo_update_nearest();
            } else if (*cell==C_POWER) {
                *cell=C_EMPTY; dots_left--; *score_ptr+=PTS_POWER;
                frighten_timer=T_FRIGHTEN; ghost_combo=0;
                for (int i=0;i<4;i++)
                    if (gh[i].mode==GM_SCATTER||gh[i].mode==GM_CHASE||gh[i].mode==GM_LEAVING)
                        gh[i].mode=GM_FRIGHTENED;
                sound_effect_pacman_power();
                if (demo_mode) demo_update_nearest();
            }
            if (col==BONUS_COL&&row==BONUS_ROW&&bonus_visible>0){
                bonus_visible=0; *score_ptr+=PTS_BONUS;
                bonus_winner=bonus_winner_val;
                sound_effect_pacman_fruit();
            }
        }
    }

    if (!at_tx || !at_ty) return;

    tunnel_teleport(&p->px, &p->py, &p->tx, &p->ty, p->dir);

    int8_t col=(int8_t)px_to_col(p->px), row=(int8_t)px_to_row(p->py);

    if (!in_tunnel_zone(p->px, p->py)) {
        Dir try_dir = p->want;
        if (try_dir != DIR_NONE) {
            int8_t nc=col+DX[try_dir], nr=row+DY[try_dir]; tunnel_wrap(&nc,nr);
            if (pac_ok(nc,nr)) p->dir = try_dir;
        }
    }

    {
        int8_t nc=col+DX[p->dir], nr=row+DY[p->dir]; tunnel_wrap(&nc,nr);
        if (pac_ok(nc,nr)) {
            p->tx=col_to_px(nc); p->ty=row_to_py(nr);
            p->moving=true;
        } else {
            p->moving=false;
        }
    }
}

static void pac_step(void) {
    static int8_t ad = 1;
    pac_step_generic(&pac,  &score,  1, &ad);
}
static void pac2_step(void) {
    static int8_t ad = 1;
    pac_step_generic(&pac2, &score2, 2, &ad);
}

static void check_col(void) {
    int thresh = CELL / 2;
    for (int i=0;i<4;i++) {
        Ghost *g=&gh[i];
        if (g->mode==GM_DEAD||g->mode==GM_HOME) continue;

        if (pac.alive && absi(g->px-pac.px)<=thresh && absi(g->py-pac.py)<=thresh) {
            if (g->mode==GM_FRIGHTENED) {
                ghost_combo++;
                int pts=200<<(ghost_combo-1); if(pts>1600)pts=1600;
                score+=pts; g->mode=GM_DEAD;
                g->tx=g->px; g->ty=g->py;
                sound_effect_pacman_eat_ghost();
            } else {
                pac.alive=false;
                sound_effect_pacman_death();
                state=PM_DEAD; pause_cnt=T_DEAD_W;
            }
        }

        if (two_player && pac2.alive && !p2_dead_anim &&
            absi(g->px-pac2.px)<=thresh && absi(g->py-pac2.py)<=thresh) {
            if (g->mode==GM_FRIGHTENED) {
                ghost_combo++;
                int pts=200<<(ghost_combo-1); if(pts>1600)pts=1600;
                score2+=pts; g->mode=GM_DEAD;
                g->tx=g->px; g->ty=g->py;
                sound_effect_pacman_eat_ghost();
            } else {
                pac2.alive=false;
                sound_effect_pacman_death();
                if (!pac.alive || state==PM_DEAD) {
                    state=PM_DEAD; pause_cnt=T_DEAD_W;
                } else {
                    p2_dead_anim=true; p2_dead_cnt=T_DEAD_W;
                }
            }
        }
    }
}

static void demo_ai(void) {
    static const Dir dirs[4]={DIR_UP,DIR_LEFT,DIR_DOWN,DIR_RIGHT};
    Dir best=DIR_NONE; int bval=-99999;
    int8_t col=(int8_t)px_to_col(pac.px), row=(int8_t)px_to_row(pac.py);
    int tc = demo_nearest_c, tr = demo_nearest_r;
    for (int i=0;i<4;i++) {
        Dir d=dirs[i]; if(d==opp(pac.dir)) continue;
        int8_t nc=col+DX[d], nr=row+DY[d]; tunnel_wrap(&nc,nr);
        if (!pac_ok(nc,nr)) continue;
        int val = -manh(nc, nr, tc, tr) * 2;
        for (int g=0;g<4;g++)
            if (gh[g].mode!=GM_FRIGHTENED&&gh[g].mode!=GM_DEAD){
                int gd=manh(nc,nr,px_to_col(gh[g].px),px_to_row(gh[g].py));
                if(gd<5)val-=(5-gd)*60;}
        if(val>bval){bval=val;best=d;}
    }
    if(best!=DIR_NONE) pac.want=best;
}

// ─────────────────────────────────────────────────────────────────────────────
// Dibujo: helpers geométricos simplificados (sustituyen a los sprites de
// mapa de bits 32×32 del original — ver notas de cabecera, punto 2).
// ─────────────────────────────────────────────────────────────────────────────

// Raíz cuadrada entera, solo para radios pequeños (<=8) -- suficiente aquí.
static int isqrt_small(int n) {
    int v = 0;
    while ((v+1)*(v+1) <= n) v++;
    return v;
}

// Círculo relleno por filas horizontales (pocas llamadas a fill_rect).
static void fill_circle(int cx, int cy, int r, uint16_t color) {
    for (int dy = -r; dy <= r; dy++) {
        int dx = isqrt_small(r*r - dy*dy);
        renderer_fill_rect(cx - dx, cy + dy, 2*dx + 1, 1, color);
    }
}

// Pac-Man: círculo con cuña de boca abierta hacia 'dir' (dirección real
// de movimiento, sin girar -- la boca tiene que apuntar hacia donde se
// mueve). frame: 0=abierta del todo, 1=medio abierta, 2=cerrada (círculo
// completo).
static void draw_pac_sprite(int cx, int cy, Dir dir, int frame, uint16_t color) {
    int r = CELL/2 - 1; if (r < 3) r = 3;
    if (dir == DIR_NONE) dir = DIR_RIGHT;
    int8_t ddx = DX[dir], ddy = DY[dir];

    for (int dy = -r; dy <= r; dy++) {
        int half = isqrt_small(r*r - dy*dy);
        for (int dx = -half; dx <= half; dx++) {
            if (frame < 2) {
                int along = dx*ddx + dy*ddy;
                if (along > 0) {
                    int perp = dx*ddy - dy*ddx; if (perp < 0) perp = -perp;
                    bool cut = (frame == 0) ? (perp <= along)        // boca ancha (~45°)
                                             : (perp*2 <= along);     // boca media (~26°)
                    if (cut) continue;
                }
            }
            renderer_fill_rect(cx+dx, cy+dy, 1, 1, color);
        }
    }
}

// Fantasma: "cúpula" + cuerpo rectangular + 3 patas dentadas, pero toda
// la silueta va girada 90° en el sentido de las agujas del reloj
// respecto al dibujo "clásico" (cúpula arriba, patas abajo). Esta forma
// es FIJA -- no depende de hacia dónde se mueve el fantasma, igual que
// en el arcade original -- así que si no se gira aquí sigue apuntando
// "hacia arriba" del viejo layout horizontal en vez de hacia donde
// corresponde en esta pantalla vertical. Tras el giro: la cúpula queda
// hacia la derecha ("este") y las patas dentadas hacia la izquierda
// ("oeste").
static void draw_ghost_body(int cx, int cy, uint16_t body_color) {
    int r = CELL/2 - 1; if (r < 3) r = 3;

    // Cúpula: semicírculo hacia la derecha (antes era hacia arriba)
    for (int dx = 0; dx <= r; dx++) {
        int dy = isqrt_small(r*r - dx*dx);
        renderer_fill_rect(cx + dx, cy - dy, 1, 2*dy + 1, body_color);
    }
    // Cuerpo recto, pegado a la izquierda de la cúpula
    int body_w = r - 1; if (body_w < 1) body_w = 1;
    renderer_fill_rect(cx - body_w, cy - r, body_w, 2*r + 1, body_color);

    // Borde dentado (3 dientes con huecos entre ellos), ahora en el
    // lado izquierdo/oeste en vez de abajo
    int bx = cx - body_w - 1;
    int h = 2*r + 1;
    int tooth = h / 3; if (tooth < 2) tooth = 2;
    for (int i = 0; i < 3; i++) {
        int ty0 = cy - r + i*tooth;
        int th = tooth - 1; if (th < 1) th = 1;
        renderer_fill_rect(bx, ty0, 1, th, body_color);
    }
}

static void draw_ghost_eyes(int cx, int cy, Dir dir, uint16_t pupil_color) {
    // Las cuencas (parte blanca) son una forma fija pegada al cuerpo,
    // así que van giradas igual que draw_ghost_body(): apiladas en
    // vertical junto al lado "este" (donde ahora está la cúpula), en
    // vez de una al lado de la otra como en el dibujo horizontal
    // original.
    int ex = cx;
    int ey0 = cy - 2, ey1 = cy + 1;
    renderer_fill_rect(ex, ey0, 2, 2, COLOR_WHITE);
    renderer_fill_rect(ex, ey1, 2, 2, COLOR_WHITE);

    // La pupila SÍ sigue la dirección real de movimiento, SIN girar --
    // igual que la boca de Pac-Man: así el jugador ve hacia dónde va
    // el fantasma de verdad, no una dirección girada sin sentido para
    // el jugador.
    int pdx = (dir==DIR_LEFT) ? -1 : (dir==DIR_RIGHT) ? 1 : 0;
    int pdy = (dir==DIR_UP)   ? -1 : (dir==DIR_DOWN)  ? 1 : 0;
    renderer_fill_rect(ex+pdx, ey0+pdy, 1, 1, pupil_color);
    renderer_fill_rect(ex+pdx, ey1+pdy, 1, 1, pupil_color);
}

// Colores clásicos de los 4 fantasmas (aprox en RGB565; naranja no está
// entre las constantes de st7789.h así que se define aquí).
#define COLOR_ORANGE  0xFD20
#define COLOR_PINK    0xFC1F
#define GHOST_FRIGHT_COLOR  COLOR_BLUE

static uint16_t ghost_color(int i) {
    switch (i) {
    case 0: return COLOR_RED;     // Blinky
    case 1: return COLOR_PINK;    // Pinky
    case 2: return COLOR_CYAN;    // Inky
    default: return COLOR_ORANGE; // Clyde
    }
}

// Bonus: un único diamante relleno, coloreado según el tipo (0-5), en vez
// de las 6 frutas de pixel-art detallado del original (no caben a CELL=12).
static void draw_bonus_sprite(int cx, int cy, int type) {
    static const uint16_t cols[6] = {
        COLOR_RED, COLOR_GREEN, COLOR_MAGENTA, COLOR_YELLOW, COLOR_ORANGE, COLOR_CYAN
    };
    uint16_t color = cols[type % 6];
    int r = CELL/2 - 2; if (r < 2) r = 2;
    for (int dy = -r; dy <= r; dy++) {
        int dx = r - absi(dy);
        renderer_fill_rect(cx - dx, cy + dy, 2*dx + 1, 1, color);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Mapa
// ─────────────────────────────────────────────────────────────────────────────
static bool is_wall_cell(int r, int c) {
    if (r<0||r>=MAP_ROWS||c<0||c>=MAP_COLS) return true;
    return MAP_ORIG[r][c] == C_WALL || MAP_ORIG[r][c] == C_DOOR;
}

static void draw_wall_tile(int px, int py, bool N, bool S, bool E, bool W) {
    (void)is_wall_cell;
    int t = (N?8:0)|(S?4:0)|(E?2:0)|(W?1:0);
    uint16_t color = COLOR_BLUE;

    enum { mid   = CELL / 2,
           thick = (CELL / 3 < 2) ? 2 : CELL / 3,
           len   = CELL };

    switch(t) {
    case 0:
        renderer_fill_rect(px + mid - thick/2, py + mid - thick/2, thick, thick, color);
        break;
    case 1:
        renderer_fill_rect(px, py + mid - thick/2, mid, thick, color);
        break;
    case 2:
        renderer_fill_rect(px + mid, py + mid - thick/2, mid, thick, color);
        break;
    case 3:
        renderer_fill_rect(px, py + mid - thick/2, len, thick, color);
        break;
    case 4:
        renderer_fill_rect(px + mid - thick/2, py + mid, thick, mid, color);
        break;
    case 8:
        renderer_fill_rect(px + mid - thick/2, py, thick, mid, color);
        break;
    case 12:
        renderer_fill_rect(px + mid - thick/2, py, thick, len, color);
        break;
    case 7:
        renderer_fill_rect(px, py + mid - thick/2, len, thick, color);
        renderer_fill_rect(px + mid - thick/2, py + mid, thick, mid, color);
        break;
    case 11:
        renderer_fill_rect(px, py + mid - thick/2, len, thick, color);
        renderer_fill_rect(px + mid - thick/2, py, thick, mid, color);
        break;
    case 13:
        renderer_fill_rect(px + mid - thick/2, py, thick, len, color);
        renderer_fill_rect(px, py + mid - thick/2, mid, thick, color);
        break;
    case 14:
        renderer_fill_rect(px + mid - thick/2, py, thick, len, color);
        renderer_fill_rect(px + mid, py + mid - thick/2, mid, thick, color);
        break;
    case 15:
        renderer_fill_rect(px, py + mid - thick/2, len, thick, color);
        renderer_fill_rect(px + mid - thick/2, py, thick, len, color);
        break;
    default:
        if (N) renderer_fill_rect(px + mid - thick/2, py, thick, mid, color);
        if (S) renderer_fill_rect(px + mid - thick/2, py + mid, thick, mid, color);
        if (E) renderer_fill_rect(px + mid, py + mid - thick/2, mid, thick, color);
        if (W) renderer_fill_rect(px, py + mid - thick/2, mid, thick, color);
        break;
    }
}

static void draw_map(void) {
    for (int r=0;r<MAP_ROWS;r++) {
        for (int c=0;c<MAP_COLS;c++) {
            int px=MAP_X+c*CELL, py=MAP_Y+r*CELL;
            uint8_t cell = map[r][c];
            if (cell == C_WALL) {
                uint8_t f = wall_flags[r][c];
                draw_wall_tile(px, py, f>>3&1, f>>2&1, f>>1&1, f&1);
            } else if (cell == C_PATH) {
                renderer_fill_rect(px+CELL/2-1, py+CELL/2-1, 2, 2, COLOR_YELLOW);
            } else if (cell == C_POWER) {
                if ((blink/8)%2==0)
                    renderer_fill_rect(px+CELL/2-2, py+CELL/2-2, 4, 4, COLOR_YELLOW);
            } else if (cell == C_DOOR) {
                renderer_fill_rect(px+1, py+CELL/2-1, CELL-2, 1, COLOR_MAGENTA);
            }
        }
    }
    if (bonus_visible > 0) {
        bool show = (bonus_visible > 3*TICKS_S) || ((blink/4)%2==0);
        if (show) draw_bonus_sprite(CELL_CX(BONUS_COL), CELL_CY(BONUS_ROW), bonus_type);
    }
}

static void draw_pacman(void) {
    if (pac.alive) {
        int frame = pac.anim;
        draw_pac_sprite(pac.px, pac.py, pac.dir, frame, COLOR_YELLOW);
    }
    if (two_player && pac2.alive) {
        int frame = pac2.anim;
        // J2 en un tono distinto (verde) para diferenciarlo visualmente de J1
        draw_pac_sprite(pac2.px, pac2.py, pac2.dir, frame, COLOR_GREEN);
    }
    if (two_player && !pac2.alive && p2_dead_anim && ((blink/4)%2==0)) {
        renderer_fill_rect(pac2.px-3, pac2.py-3, 6, 6, COLOR_GREEN);
    }
}

static void draw_ghosts(void) {
    for (int i=0;i<4;i++) {
        Ghost *g=&gh[i];
        if (g->mode==GM_DEAD) {
            draw_ghost_eyes(g->px, g->py, g->dir, COLOR_WHITE);
        } else if (g->mode==GM_FRIGHTENED) {
            bool show = (frighten_timer >= TICKS_S*2) || ((blink/4)%2==0);
            if (show) {
                draw_ghost_body(g->px, g->py, GHOST_FRIGHT_COLOR);
                draw_ghost_eyes(g->px, g->py, g->dir, COLOR_WHITE);
            }
        } else {
            draw_ghost_body(g->px, g->py, ghost_color(i));
            draw_ghost_eyes(g->px, g->py, g->dir, COLOR_BLUE);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// HUD compacto: franja superior (título/puntuaciones/nivel) y franja
// inferior (vidas / aviso frightened), a ambos lados del mapa vertical.
// ─────────────────────────────────────────────────────────────────────────────
static int centered_x(const char *text, int scale) {
    int w = (int)st7789_text_width(text, (uint8_t)scale);
    int x = (SCREEN_W - w) / 2;
    return (x < 0) ? 0 : x;
}

// Igual que centered_x(), pero para texto dibujado con rotation=1
// (horizontal, LANDSCAPE_W de ancho real) en vez de con la vertical
// propia de Pac-Man -- ver nota 8) en la cabecera del archivo.
static int centered_x_lw(const char *text, int scale) {
    int w = (int)st7789_text_width(text, (uint8_t)scale);
    int x = (LANDSCAPE_W - w) / 2;
    return (x < 0) ? 0 : x;
}

static void draw_hud(void) {
    char buf[32];

    // Fila 1: J1 score (izq) — NIVEL (centro) — J2 score (der, si 2P)
    snprintf(buf, sizeof(buf), "J1 %06d", score);
    renderer_draw_text(2, HUD_TOP_Y, buf, COLOR_YELLOW, COLOR_BLACK, 1);

    snprintf(buf, sizeof(buf), "LV%02d", level);
    renderer_draw_text(centered_x(buf, 1), HUD_TOP_Y, buf, COLOR_WHITE, COLOR_BLACK, 1);

    if (two_player) {
        snprintf(buf, sizeof(buf), "J2 %06d", score2);
        int w = (int)st7789_text_width(buf, 1);
        renderer_draw_text(SCREEN_W - 2 - w, HUD_TOP_Y, buf, COLOR_GREEN, COLOR_BLACK, 1);
    }

    // Fila 2: vidas de cada jugador (mini-icono) + estado FRIGHT
    int y2 = HUD_TOP_Y + 10;
    int lx = 2;
    {
        int n = lives; if (n>5) n=5; if (n<0) n=0;
        for (int i=0;i<n;i++) {
            draw_pac_sprite(lx+4, y2+4, DIR_RIGHT, 0, COLOR_YELLOW);
            lx += 10;
        }
    }
    if (two_player) {
        int rx = SCREEN_W - 2;
        int n = lives2; if (n>5) n=5; if (n<0) n=0;
        rx -= n*10;
        for (int i=0;i<n;i++) {
            draw_pac_sprite(rx+4, y2+4, DIR_RIGHT, 0, COLOR_GREEN);
            rx += 10;
        }
    }
    if (frighten_timer > 0) {
        renderer_draw_text(centered_x("FRIGHT!", 1), y2, "FRIGHT!", COLOR_CYAN, COLOR_BLACK, 1);
    }

    // Franja inferior: mensajes contextuales según estado (ver draw_frame)
}

// ─────────────────────────────────────────────────────────────────────────────
// Dibujo por estado (equivalente a pac_draw() del original)
// ─────────────────────────────────────────────────────────────────────────────
static void draw_frame(void) {
    renderer_clear(COLOR_BLACK);
    blink++;
    bool bon = (blink/20)%2==0;

    switch (state) {
    case PM_SELECT: {
        // Esta pantalla se muestra con rotation=1 (horizontal, ver
        // game_pacman_run()), así que usa LANDSCAPE_W/H y
        // centered_x_lw() en vez de las medidas verticales propias de
        // Pac-Man -- y el mismo layout relativo a CY que
        // draw_select_screen() en pong.c.
        int lcx = LANDSCAPE_W/2, lcy = LANDSCAPE_H/2;
        renderer_draw_text(centered_x_lw("PAC-MAN", 3), lcy-60, "PAC-MAN", COLOR_YELLOW, COLOR_BLACK, 3);
        draw_pac_sprite(lcx-30, lcy-25, DIR_RIGHT, 0, COLOR_YELLOW);
        draw_ghost_body(lcx+10, lcy-25, COLOR_RED);
        draw_ghost_eyes(lcx+10, lcy-25, DIR_RIGHT, COLOR_BLUE);

        // Mismo formato de líneas (guiones a los lados de la opción
        // activa, ancho fijo para que el texto no "salte" al cambiar)
        // y mismo texto de ayuda que draw_select_screen() en pong.c.
        const char *l1 = two_player  ? "- 2 JUGADORES -" : "  2 JUGADORES  ";
        const char *l2 = !two_player ? "- 1 JUGADOR   -" : "  1 JUGADOR    ";
        renderer_draw_text(centered_x_lw(l1,2), lcy-10, l1, COLOR_WHITE, COLOR_BLACK, 2);
        renderer_draw_text(centered_x_lw(l2,2), lcy+16, l2, COLOR_WHITE, COLOR_BLACK, 2);

        renderer_draw_text(centered_x_lw("GIRA PARA CAMBIAR - PULSA PARA JUGAR", 1), lcy+60,
                            "GIRA PARA CAMBIAR - PULSA PARA JUGAR", COLOR_WHITE, COLOR_BLACK, 1);
        return;
    }

    case PM_READY:
        draw_map(); draw_pacman(); draw_ghosts(); draw_hud();
        if (bon)
            renderer_draw_text(centered_x("READY!",2), CELL_CY(11)-7, "READY!", COLOR_YELLOW, COLOR_BLACK, 2);
        return;

    case PM_DEAD:
        draw_map(); draw_ghosts(); draw_hud();
        if (pac.alive) draw_pacman();
        if (!pac.alive && bon)
            renderer_fill_rect(pac.px-3, pac.py-3, 6, 6, COLOR_YELLOW);
        if (two_player && !pac2.alive && bon)
            renderer_fill_rect(pac2.px-3, pac2.py-3, 6, 6, COLOR_GREEN);
        return;

    case PM_LEVELUP: {
        draw_map(); draw_pacman(); draw_ghosts(); draw_hud();
        if (bon) {
            char b[16]; snprintf(b, sizeof(b), "NIVEL %d", level);
            renderer_draw_text(centered_x(b,2), CELL_CY(11)-7, b, COLOR_YELLOW, COLOR_BLACK, 2);
            if (two_player && bonus_winner > 0) {
                char bw[20]; snprintf(bw, sizeof(bw), "BONUS -> J%d!", bonus_winner);
                renderer_draw_text(centered_x(bw,1), CELL_CY(11)+14, bw, COLOR_YELLOW, COLOR_BLACK, 1);
            }
        }
        return;
    }

    case PM_GAMEOVER:
        draw_map(); draw_ghosts(); draw_hud();
        if (bon) {
            renderer_draw_text(centered_x("GAME OVER",2), CELL_CY(11)-7, "GAME OVER", COLOR_RED, COLOR_BLACK, 2);
            if (two_player) {
                const char *w = (score>score2) ? "P1 WIN!" : (score2>score) ? "P2 WIN!" : "EMPATE!";
                renderer_draw_text(centered_x(w,1), CELL_CY(11)+16, w, COLOR_WHITE, COLOR_BLACK, 1);
            }
        }
        return;

    case PM_SCORES: {
        highscores_draw(PM_GAME_ID, "PAC-MAN", 20);
        if (bon)
            renderer_draw_text(centered_x_lw("Pulsa para continuar",1), LANDSCAPE_H-20,
                                "Pulsa para continuar", COLOR_WHITE, COLOR_BLACK, 1);
        return;
    }

    default: break;
    }

    // PM_PLAYING
    draw_map();
    draw_ghosts();
    draw_pacman();
    draw_hud();
}

// ─────────────────────────────────────────────────────────────────────────────
// Entrada: J1 = encoder 0 (izq/der) + BTN_J1_A (arriba) + BTN_J1_B
// (abajo). J2 = encoder 1 + BTN_J2_A/BTN_J2_B. Salir al menú:
// mantener pulsados ambos SW de encoder a la vez (BTN_ENC1_SW/BTN_ENC2_SW).
// ─────────────────────────────────────────────────────────────────────────────
#define ENC_DETENT 4   // 4 transiciones de cuadratura = 1 detent físico

static void read_player_turn(int encoder_index, int *acc, Dir *want) {
    int32_t d = controls_get_raw_delta(encoder_index);
    if (d == 0) return;
    *acc += (int)d;
    if (*acc >= ENC_DETENT)  { *want = DIR_DOWN; *acc = 0; }
    if (*acc <= -ENC_DETENT) { *want = DIR_UP;   *acc = 0; }
}

static void pm_tick(void) {
    if (demo_mode) {
        bool any = controls_menu_select()
                || controls_get_raw_delta(0) != 0
                || controls_get_raw_delta(1) != 0;
        if (any || ++demo_ticks >= TICKS_S*30) { g_done = true; return; }
    }

    bool exit_combo = controls_button_down(BTN_ENC1_SW) && controls_button_down(BTN_ENC2_SW);
    if (exit_combo && (state==PM_PLAYING||state==PM_READY||state==PM_DEAD)) {
        g_done = true; return;
    }

    switch (state) {

    case PM_SELECT: {
        if (controls_menu_up() || controls_menu_down()) two_player = !two_player;
        if (controls_menu_select()) {
            score=0; lives=3; level=1;
            score2=0; lives2=3;
            p2_dead_anim=false; p2_dead_cnt=0;
            level_init();
            sound_start_pacman_intro();
            pause_cnt = (1000 * TICKS_S) / 1000 + 10;
            state=PM_READY;
            // El menú se mostró en horizontal (rotation=1); el juego en
            // sí necesita vertical para encajar el laberinto -- ver
            // nota sobre rotación en game_pacman_run().
            st7789_set_rotation(0);
            renderer_clear(COLOR_BLACK);
            renderer_flush();
        }
        break;
    }

    case PM_READY:
        if(--pause_cnt<=0) state=PM_PLAYING;
        break;

    case PM_PLAYING: {
        if (demo_mode) {
            demo_ai();
        } else {
            read_player_turn(0, &enc_acc, &pac.want);
            if (controls_button_pressed(BTN_J1_A)) pac.want = DIR_LEFT;
            if (controls_button_pressed(BTN_J1_B)) pac.want = DIR_RIGHT;

            if (two_player) {
                read_player_turn(1, &enc2_acc, &pac2.want);
                if (controls_button_pressed(BTN_J2_A)) pac2.want = DIR_LEFT;
                if (controls_button_pressed(BTN_J2_B)) pac2.want = DIR_RIGHT;
            }
        }

        if(frighten_timer>0){
            if(--frighten_timer==0)
                for(int i=0;i<4;i++)
                    if(gh[i].mode==GM_FRIGHTENED)
                        gh[i].mode=scatter_phase?GM_SCATTER:GM_CHASE;
        } else {
            if(--scatter_timer<=0){
                scatter_phase=!scatter_phase;
                scatter_timer=scatter_phase?T_SCATTER:T_CHASE;
                for(int i=0;i<4;i++)
                    if(gh[i].mode==GM_SCATTER||gh[i].mode==GM_CHASE)
                        gh[i].dir=opp(gh[i].dir);
            }
        }

        for(int i=0;i<4;i++) {
            if(gh[i].mode==GM_HOME&&--gh[i].home_timer<=0) {
                gh[i].mode=GM_LEAVING;
                gh[i].tx=gh[i].px; gh[i].ty=gh[i].py;
            }
        }

        if (bonus_visible > 0) {
            bonus_visible--;
        } else if (bonus_spawn > 0) {
            bool can_spawn = !two_player || (dots_left == 0);
            if (can_spawn && --bonus_spawn == 0)
                bonus_visible = T_BONUS_VIS;
        }

        if (p2_dead_anim) {
            if (--p2_dead_cnt <= 0) {
                p2_dead_anim = false;
                lives2--;
                if (lives2 > 0) {
                    pac2_reset();
                    for(int i=0;i<4;i++) ghost_reset(i);
                } else {
                    if (!pac.alive && lives <= 0) {
                        sound_stop_pacman_intro();
                        state=PM_GAMEOVER; pause_cnt=T_GAMEOVER;
                    }
                }
            }
        }

        pac_step();
        if (two_player && !p2_dead_anim) pac2_step();
        for(int i=0;i<4;i++) ghost_step(i);
        check_col();

        if (two_player) {
            if (dots_left==0 && bonus_winner>0) {
                level++; state=PM_LEVELUP; pause_cnt=T_LEVELUP;
                sound_effect_pacman_levelup();
            }
        } else {
            if (dots_left==0) {
                level++; state=PM_LEVELUP; pause_cnt=T_LEVELUP;
                sound_effect_pacman_levelup();
            }
        }
        break;
    }

    case PM_DEAD:
        if(--pause_cnt<=0){
            bool j1_died = !pac.alive;
            bool j2_died = two_player && !pac2.alive && !p2_dead_anim;

            if (j1_died) lives--;
            if (j2_died) lives2--;

            bool j1_out = (lives <= 0);
            bool j2_out = !two_player || (lives2 <= 0);

            if (j1_out && j2_out) {
                sound_stop_pacman_intro();
                state=PM_GAMEOVER; pause_cnt=T_GAMEOVER;
            } else {
                if (j1_died) {
                    if (lives > 0) pac_reset();
                    else           pac.alive = false;
                }
                if (two_player && j2_died) {
                    if (lives2 > 0) pac2_reset();
                    else            pac2.alive = false;
                }
                for(int i=0;i<4;i++) ghost_reset(i);
                state=PM_READY; pause_cnt=T_READY;
            }
        }
        break;

    case PM_LEVELUP:
        if(--pause_cnt<=0){
            spd=level_speed(level);
            spd_fright = fright_speed(level);
            spd_tunnel = tunnel_speed(level);
            level_init(); state=PM_READY; pause_cnt=T_READY;
        }
        break;

    case PM_GAMEOVER:
        if (controls_menu_select() || --pause_cnt<=0) {
            // Se pasa a horizontal aquí: tanto la introducción de
            // iniciales como la tabla de récords están pensadas para
            // la rotación estándar del resto de juegos, no para el
            // rotation=0 vertical de Pac-Man -- ver nota 8) en la
            // cabecera. No hace falta volver a rotation=0 después: es
            // la última pantalla antes de salir, y game_pacman_run()
            // ya restaura rotation=1 al terminar el bucle.
            st7789_set_rotation(1);
            renderer_clear(COLOR_BLACK);
            renderer_flush();
            if (!demo_mode) {
                if (highscores_is_top(PM_GAME_ID,(uint32_t)score))  highscores_enter(PM_GAME_ID,(uint32_t)score);
                if (two_player && highscores_is_top(PM_GAME_ID,(uint32_t)score2)) highscores_enter(PM_GAME_ID,(uint32_t)score2);
            }
            state=PM_SCORES; pause_cnt=0;
        }
        if (demo_mode && pause_cnt<=0) g_done=true;
        break;

    case PM_SCORES:
        if (controls_menu_select() || ++pause_cnt>TICKS_S*8) { highscores_flush(); g_done=true; }
        break;

    default: break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// API pública — bucle propio, igual patrón que game_asteroids_run()
// ─────────────────────────────────────────────────────────────────────────────
void game_pacman_run(game_mode_t mode) {
    precompute_wall_flags();

    demo_mode  = (mode == GAME_MODE_DEMO);
    two_player = (mode == GAME_MODE_2P);
    state = demo_mode ? PM_PLAYING : PM_SELECT;

    blink=0; pause_cnt=0; g_done=false; demo_ticks=0;
    enc_acc=0; enc2_acc=0;

    if (demo_mode) {
        score=0; lives=3; level=1; score2=0; lives2=3;
        two_player=false;
        p2_dead_anim=false; p2_dead_cnt=0;
        level_init();
    }

    // Rotación inicial. El menú de 1/2 jugadores (PM_SELECT) se dibuja
    // en horizontal (rotation=1), igual que el resto de menús
    // compartidos (records, selector de juego) -- se pasa a vertical
    // (rotation=0, para encajar el laberinto) justo al confirmar la
    // partida, ver el case PM_SELECT en pm_tick(). En modo demo no hay
    // pantalla de selección, así que se entra directo en vertical.
    // rotation=0 -> 240x320; si en tu panel sale al revés, prueba rotation=2
    // (misma resolución, 180° girada) -- ver comentario de st7789_set_rotation
    // en st7789.h.
    st7789_set_rotation(demo_mode ? 0 : 1);
    renderer_clear(COLOR_BLACK);
    renderer_flush();

    absolute_time_t last = get_absolute_time();

    while (!g_done) {
        absolute_time_t now = get_absolute_time();
        uint32_t elapsed_ms = (uint32_t)(absolute_time_diff_us(last, now) / 1000);
        last = now;
        update_dt_scale(elapsed_ms);

        controls_update();
        pm_tick();
        sound_update();

        draw_frame();
        renderer_flush();

        sleep_ms(16);
    }

    highscores_flush();

    // Restaurar horizontal para el resto de ArcadeColor (rotation=1, el
    // valor por defecto que usa st7789_init()).
    st7789_set_rotation(1);
    renderer_clear(COLOR_BLACK);
    renderer_flush();
}
