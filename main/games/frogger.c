/**
 * frogger.c -- remake genérico del clásico "cruza la carretera y el
 * río saltando", construido desde cero (mecánica de juego, no hay
 * arte ni código original que portar aquí) siguiendo el mismo
 * esquema que asteroids.c / lunar_lander.c.
 *
 * DIFERENCIA CLAVE con los otros juegos de ArcadeColor: aquí el
 * "campo" no es estático (terreno de lunar_lander) ni casi-vacío
 * (campo de asteroids) -- son ~8 carriles con tráfico/troncos en
 * movimiento CONTINUO simultáneo. Decisiones de adaptación:
 *
 *  - CUADRÍCULA, no física libre: la rana se mueve en saltos de una
 *    celda (fila/columna), como el original. Los objetos de cada
 *    carril SÍ se mueven en continuo (posición de punto fijo), y se
 *    dibujan/borran de forma incremental igual que el resto del
 *    proyecto: solo se toca la pantalla cuando la posición en
 *    píxeles de un objeto (redondeada) cambia respecto al frame
 *    anterior.
 *  - Fondos PLANOS por fila (asfalto/río/césped): a diferencia del
 *    terreno de lunar_lander (con altura variable por columna), aquí
 *    "borrar" un objeto es simplemente repintar el color de fondo
 *    de SU fila -- mucho más barato, no hace falta muestrear ningún
 *    perfil. La única fila con fondo no uniforme es la de META (fila
 *    0: setos entre huecos), así que tiene su propia función
 *    cell_bg_color().
 *  - Objetos en carril = "cinta sin fin": cada carril tiene
 *    OBJ_PER_LANE objetos igualmente espaciados en un ciclo de
 *    longitud CYCLE_LEN_PX (el ancho de la cuadrícula + un margen
 *    de sobra). La posición de cada objeto es
 *    (offset_relativo + desplazamiento_acumulado) módulo ese ciclo
 *    -- así entran y salen por los bordes sin "teletransportarse" ni
 *    necesitar lógica de aparición/desaparición.
 *  - Franja de HUD DEDICADA (filas 11 y 12, las dos últimas de la
 *    cuadrícula): a diferencia de asteroids/lunar_lander, aquí el
 *    tablero ocupa TODA el área de juego, así que no sobra sitio
 *    arriba para el marcador. Se reservan 2 filas de la propia
 *    cuadrícula (36px) exclusivamente para el HUD -- la rana nunca
 *    entra ahí.
 *  - Colisión con coches con caja ligeramente más pequeña que el
 *    sprite dibujado (ver FROG_HIT_INSET/VEH_HIT_INSET en
 *    road_hit_at), para que el choque case con lo que se ve en
 *    pantalla. Hubo una versión con "barrido" prev_x..nuevo_x para
 *    protegerse de que un coche muy rápido atravesara la celda sin
 *    detectarse en un tick largo; se quitó porque con las
 *    velocidades ya bajadas (ver init_lanes) sobraba, y agrandaba la
 *    caja de colisión más de lo que se veía.
 *  - 2 jugadores SIMULTÁNEOS: cada rana tiene su propio sub-estado
 *    (Frog.pstate) independiente -- un jugador puede estar
 *    parpadeando tras morir mientras el otro sigue saltando con
 *    total normalidad. El progreso de huecos de meta rellenados es
 *    COMPARTIDO entre ambos (avance conjunto hacia limpiar el
 *    nivel); cuando un jugador agota sus vidas pasa a espectador
 *    (Frog.pstate == PS_DONE) el resto de la partida.
 *  - Controles por jugador: encoder = paso IZQUIERDA/DERECHA (un
 *    detent = una celda, con acumulador+cooldown para que cada
 *    salto sea un único evento discreto); BTN_x_A = salto ARRIBA;
 *    BTN_x_B = salto ABAJO.
 *  - Sin sistema de partículas: al morir, la celda de la rana
 *    parpadea unos instantes (color según la causa) y se restaura
 *    -- mismo objetivo visual que el "boom" de lunar_lander con
 *    mucho menos código, ya que aquí no hace falta simular nada
 *    (fondo plano por fila).
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "frogger.h"
#include "renderer.h"
#include "controls.h"
#include "highscores.h"
#include "sound.h"

// ---------------------------------------------------------------------------
// Área de juego -- literales fijos, igual que el resto de juegos.
// ---------------------------------------------------------------------------
#define SCREEN_W 320
#define SCREEN_H 240
#define PLAY_X   4
#define PLAY_Y   3
#define PLAY_W   (SCREEN_W - 2 * PLAY_X)   // 312
#define PLAY_H   (SCREEN_H - 2 * PLAY_Y)   // 234
#define CX       (PLAY_X + PLAY_W / 2)
#define CY       (PLAY_Y + PLAY_H / 2)

#define FP        256
#define PX2FP(px) ((int32_t)(px) << 8)
#define FP2PX(fp) ((int)((fp) >> 8))

// ---------------------------------------------------------------------------
// Cuadrícula: 14 columnas x 13 filas, celda de 22x18 -- 18*13=234
// encaja EXACTO con PLAY_H. Las dos últimas filas (11 y 12) son la
// franja de HUD, no forman parte del tablero jugable.
// ---------------------------------------------------------------------------
#define COLS    14
#define CELL_W  22
#define ROWS    13
#define CELL_H  18
#define GRID_X0 (PLAY_X + (PLAY_W - COLS*CELL_W) / 2)   // 6, centra los 308px de rejilla en los 312 de juego
#define GRID_Y0 PLAY_Y
#define GRID_W  (COLS * CELL_W)                          // 308

#define ROW_HOME         0
#define ROW_RIVER_FIRST  1
#define ROW_RIVER_LAST   4
#define N_RIVER_LANES    (ROW_RIVER_LAST - ROW_RIVER_FIRST + 1)   // 4
#define ROW_MEDIAN       5
#define ROW_ROAD_FIRST   6
#define ROW_ROAD_LAST    9
#define N_ROAD_LANES     (ROW_ROAD_LAST - ROW_ROAD_FIRST + 1)     // 4
#define ROW_START        10
#define ROW_HUD1         11
#define ROW_HUD2         12
#define ROW_MAX_PLAYABLE ROW_START

static int row_y(int row) { return GRID_Y0 + row * CELL_H; }
static int col_x(int col) { return GRID_X0 + col * CELL_W; }

#define N_HOME_SLOTS 5
static const int HOME_SLOTS[N_HOME_SLOTS] = { 1, 4, 6, 9, 12 };

// ---------------------------------------------------------------------------
// Colores (RGB565) -- ajustables a ojo, no hay ninguna referencia que
// respetar aquí.
// ---------------------------------------------------------------------------
#define COLOR_ROAD    0x39C7   // gris asfalto
#define COLOR_RIVER   0x041D   // azul río
#define COLOR_GRASS   0x1D06   // verde césped
#define COLOR_HEDGE   0x0320   // verde seto (fila de meta, columnas sin hueco)
#define COLOR_LOG     0x7A22   // marrón tronco
#define COLOR_LOG_DARK  0x38E1 // veta oscura del tronco
#define COLOR_LOG_LIGHT 0xB4EB // remate claro en los extremos del tronco
#define COLOR_TURTLE  0x1D26   // verde oscuro caparazón de tortuga
#define COLOR_TRUCK   0xE3C2   // naranja caja de carga del camión
#define COLOR_CAB     0xE71C   // gris claro cabina/parabrisas
#define COLOR_CAR1    COLOR_RED
#define COLOR_CAR2    COLOR_YELLOW
#define COLOR_FROG1   COLOR_GREEN
#define COLOR_FROG2   COLOR_MAGENTA
static uint16_t frog_color(int player) { return player == 0 ? COLOR_FROG1 : COLOR_FROG2; }

static int   clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int   absi(int v) { return v < 0 ? -v : v; }
static int   rnd(int n) { return n > 0 ? rand() % n : 0; }
static int32_t pmod(int32_t v, int32_t m) { int32_t r = v % m; if (r < 0) r += m; return r; }

// ---------------------------------------------------------------------------
// Tiempo delta real -- idéntico mecanismo que asteroids.c/lunar_lander.c.
// ---------------------------------------------------------------------------
static absolute_time_t last_tick_time;
static int32_t g_dt_scale = FP;
static int32_t g_elapsed_ms = 16;   // ms reales del último tick, derivado de g_dt_scale

static void update_dt_scale(void) {
    int64_t elapsed_us = absolute_time_diff_us(last_tick_time, get_absolute_time());
    last_tick_time = get_absolute_time();
    int32_t elapsed_ms = (int32_t)(elapsed_us / 1000);
    if (elapsed_ms < 1)  elapsed_ms = 1;
    if (elapsed_ms > 50) elapsed_ms = 50;
    g_elapsed_ms = elapsed_ms;
    g_dt_scale = elapsed_ms * FP / 16;
}

static int centered_x(const char *text, int scale) {
    int w = (int)st7789_text_width(text, (uint8_t)scale);
    int x = (TFT_WIDTH - w) / 2;
    return (x < 0) ? 0 : x;
}

static void fill_clipped_h(int x, int y, int w, int h, uint16_t color) {
    int x1 = x + w;
    if (x  < GRID_X0)          x  = GRID_X0;
    if (x1 > GRID_X0 + GRID_W) x1 = GRID_X0 + GRID_W;
    if (x1 > x) renderer_fill_rect(x, y, x1 - x, h, color);
}
static void fill_clipped(int x, int y, int w, uint16_t color) {
    fill_clipped_h(x, y, w, CELL_H, color);
}

// ---------------------------------------------------------------------------
// Nivel actual -- declarado aquí (antes de las tablas de carriles,
// que lo necesitan) para evitar el problema de orden de declaración
// que tuvo lunar_lander.c con fuel_start()/level.
// ---------------------------------------------------------------------------
static int level_g = 1;

// ---------------------------------------------------------------------------
// Carriles -- ver cabecera del archivo. Cada carril es una "cinta sin
// fin" de OBJ_PER_LANE objetos igualmente espaciados.
// ---------------------------------------------------------------------------
#define OBJ_PER_LANE     3
#define CYCLE_LEN_PX     (GRID_W + 80)                    // 388: margen de sobra para que entren/salgan fuera de vista
#define LANE_SPACING_PX  (CYCLE_LEN_PX / OBJ_PER_LANE)

#define DIVE_CYCLE_MS 6000
static int32_t dive_submerged_ms(int level) {
    int32_t v = 1600 + level * 80;
    return clampi(v, 1600, 2400);
}

typedef enum { SHAPE_CAR, SHAPE_TRUCK, SHAPE_LOG, SHAPE_TURTLE } LaneShape;

typedef struct {
    int8_t   dir;          // +1 derecha, -1 izquierda
    int32_t  speed_fp;     // magnitud, px/tick en FP
    int32_t  scroll_fp;    // acumulado con signo (dir ya aplicado)
    int16_t  width_px;
    uint16_t color;
    LaneShape shape;
    bool     is_turtle;    // solo carriles de río
    int32_t  dive_phase_ms;
} Lane;

static Lane river_lanes[N_RIVER_LANES];
static Lane road_lanes[N_ROAD_LANES];

// [0]=río [1]=carretera ; [carril][objeto]
static int  obj_x[2][4][OBJ_PER_LANE];
static int  obj_prev_x[2][4][OBJ_PER_LANE];
static bool obj_visible[2][4][OBJ_PER_LANE];
static bool obj_prev_visible[2][4][OBJ_PER_LANE];

static void reset_obj_trace(void) {
    for (int t = 0; t < 2; t++)
        for (int L = 0; L < 4; L++)
            for (int o = 0; o < OBJ_PER_LANE; o++) {
                obj_prev_x[t][L][o] = -9999;
                obj_prev_visible[t][L][o] = false;
            }
}

// Velocidades: valores pequeños "a ojo" (no proporcionales a nada),
// pensados para que el tráfico se pueda leer y cronometrar bien a
// simple vista; ver dive_submerged_ms() para la dificultad de las
// tortugas. Reescaladas hacia abajo dos veces tras probarlo en
// hardware real (historial: base carretera 1.5 -> 1.0 -> 0.75
// px/tick; base río 1.0 -> 0.75 -> 0.5 px/tick). Si sigue yendo
// rápido, toca directamente estos dos speed_fp -- son los únicos
// puntos que fijan la velocidad del tráfico.
static void init_lanes(int level) {
    for (int i = 0; i < N_RIVER_LANES; i++) {
        Lane *ln = &river_lanes[i];
        ln->dir = (i % 2 == 0) ? +1 : -1;
        ln->speed_fp = FP/2 + i * (FP/10) + (level-1) * (FP/12);
        if (ln->speed_fp > FP) ln->speed_fp = FP;
        ln->scroll_fp = 0;
        ln->is_turtle = (i % 2 == 1);
        ln->dive_phase_ms = i * 1500;   // desfasa los ciclos de inmersión entre carriles
        if (ln->is_turtle) { ln->width_px = CELL_W*2; ln->color = COLOR_TURTLE; ln->shape = SHAPE_TURTLE; }
        else               { ln->width_px = (i==0) ? CELL_W*3 : CELL_W*2; ln->color = COLOR_LOG; ln->shape = SHAPE_LOG; }
    }
    for (int i = 0; i < N_ROAD_LANES; i++) {
        Lane *ln = &road_lanes[i];
        ln->dir = (i % 2 == 0) ? -1 : +1;
        ln->speed_fp = (FP*3)/4 + i * (FP/8) + (level-1) * (FP/10);
        if (ln->speed_fp > (FP*3)/2) ln->speed_fp = (FP*3)/2;
        ln->scroll_fp = 0;
        ln->is_turtle = false;
        ln->dive_phase_ms = 0;
        if (i % 2 == 0) { ln->width_px = CELL_W;   ln->color = (i==0)?COLOR_CAR1:COLOR_CAR2; ln->shape = SHAPE_CAR; }
        else            { ln->width_px = CELL_W*2; ln->color = COLOR_TRUCK; ln->shape = SHAPE_TRUCK; }
    }
    reset_obj_trace();
    level_g = level;
}

static int lane_obj_x(const Lane *ln, int idx) {
    int32_t rel = (int32_t)idx * LANE_SPACING_PX;
    int32_t pos = pmod(rel * FP + ln->scroll_fp, (int32_t)CYCLE_LEN_PX * FP) / FP;
    return GRID_X0 - 40 + pos;
}

static void update_lanes(void) {
    for (int t = 0; t < 2; t++) {
        int n = (t == 0) ? N_RIVER_LANES : N_ROAD_LANES;
        Lane *lanes = (t == 0) ? river_lanes : road_lanes;
        for (int L = 0; L < n; L++) {
            Lane *ln = &lanes[L];
            ln->scroll_fp += (int32_t)ln->dir * ln->speed_fp * g_dt_scale / FP;
            if (ln->is_turtle) {
                ln->dive_phase_ms += g_elapsed_ms;
                if (ln->dive_phase_ms > DIVE_CYCLE_MS) ln->dive_phase_ms -= DIVE_CYCLE_MS;
            }
            bool submerged = ln->is_turtle &&
                (ln->dive_phase_ms > DIVE_CYCLE_MS - dive_submerged_ms(level_g));
            for (int o = 0; o < OBJ_PER_LANE; o++) {
                obj_prev_x[t][L][o] = obj_x[t][L][o];
                obj_prev_visible[t][L][o] = obj_visible[t][L][o];
                obj_x[t][L][o] = lane_obj_x(ln, o);
                obj_visible[t][L][o] = !submerged;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Fondo de fila -- plano excepto la fila de META (setos entre huecos,
// o la rana ya aparcada si el hueco está ocupado). cell_bg_color()
// es la función de "restaurar fondo" que usan tanto el borrado de
// objetos en movimiento como el de la rana y el parpadeo de muerte.
// ---------------------------------------------------------------------------
static bool home_filled[N_HOME_SLOTS];
static int  home_filled_by[N_HOME_SLOTS];

static int home_slot_index(int col) {
    for (int k = 0; k < N_HOME_SLOTS; k++) if (HOME_SLOTS[k] == col) return k;
    return -1;
}

static uint16_t row_bg_color(int row) {
    if (row == ROW_HOME) return COLOR_RIVER;
    if (row >= ROW_RIVER_FIRST && row <= ROW_RIVER_LAST) return COLOR_RIVER;
    if (row == ROW_MEDIAN) return COLOR_GRASS;
    if (row >= ROW_ROAD_FIRST && row <= ROW_ROAD_LAST) return COLOR_ROAD;
    if (row == ROW_START) return COLOR_GRASS;
    return COLOR_BLACK; // filas de HUD
}

static uint16_t cell_bg_color(int row, int col) {
    if (row == ROW_HOME) {
        int k = home_slot_index(col);
        if (k < 0) return COLOR_HEDGE;
        return home_filled[k] ? frog_color(home_filled_by[k]) : COLOR_RIVER;
    }
    return row_bg_color(row);
}

// ---------------------------------------------------------------------------
// Sprites detallados de los objetos de carril -- unos pocos
// rectángulos pequeños en vez de un bloque plano. El BORRADO sigue
// siendo un único fill_clipped() con el color de fondo (barato); el
// coste extra de estos detalles solo se paga al DIBUJAR, y ronda
// unas pocas llamadas más por objeto, asumible dado el margen que
// ya tenía el juego (ver cabecera del archivo).
// ---------------------------------------------------------------------------
static void draw_car(int x, int y, int w, uint16_t body, int dir) {
    fill_clipped_h(x+1, y+5, w-2, 8, body);                              // carrocería
    fill_clipped_h(x+w/2-4, y+2, 8, 4, body);                            // techo
    int wx = (dir > 0) ? x+w-8 : x+2;
    fill_clipped_h(wx, y+3, 5, 4, COLOR_CAB);                            // parabrisas
    fill_clipped_h(x+2, y+13, 4, 3, COLOR_BLACK);                        // rueda izq
    fill_clipped_h(x+w-6, y+13, 4, 3, COLOR_BLACK);                      // rueda der
}

static void draw_truck(int x, int y, int w, uint16_t body, int dir) {
    int cabw = 13;
    int cab_x   = (dir > 0) ? (x+w-cabw-1) : (x+1);
    int cargo_x = (dir > 0) ? (x+1)        : (x+cabw+2);
    int cargo_w = w - cabw - 3;
    fill_clipped_h(cargo_x, y+2, cargo_w, 11, body);                     // caja de carga
    fill_clipped_h(cab_x, y+1, cabw, 13, COLOR_CAB);                     // cabina
    int wx = (dir > 0) ? cab_x+cabw-6 : cab_x+2;
    fill_clipped_h(wx, y+3, 4, 4, COLOR_ROAD);                           // parabrisas (hueco oscuro sobre la cabina clara)
    fill_clipped_h(x+3, y+14, 4, 3, COLOR_BLACK);
    fill_clipped_h(x+w/2-2, y+14, 4, 3, COLOR_BLACK);
    fill_clipped_h(x+w-7, y+14, 4, 3, COLOR_BLACK);
}

static void draw_log(int x, int y, int w, uint16_t body) {
    fill_clipped_h(x, y+4, w, 10, body);                                 // cuerpo
    fill_clipped_h(x+2, y+7, w-4, 1, COLOR_LOG_DARK);                    // veta
    fill_clipped_h(x+2, y+10, w-4, 1, COLOR_LOG_DARK);                   // veta
    fill_clipped_h(x, y+4, 3, 10, COLOR_LOG_LIGHT);                      // remate izquierdo
    fill_clipped_h(x+w-3, y+4, 3, 10, COLOR_LOG_LIGHT);                  // remate derecho
}

static void draw_turtles(int x, int y, int w, uint16_t body) {
    int n = w / CELL_W; if (n < 1) n = 1;
    int tw = w / n;
    for (int i = 0; i < n; i++) {
        int tx = x + i*tw;
        fill_clipped_h(tx+3, y+5, tw-6, 9, body);                        // caparazón
        fill_clipped_h(tx+tw/2-2, y+3, 4, 3, COLOR_GREEN);               // cabeza
    }
}

static void draw_lane_object(LaneShape shape, int x, int y, int w, uint16_t color, int dir) {
    switch (shape) {
        case SHAPE_CAR:    draw_car(x, y, w, color, dir); break;
        case SHAPE_TRUCK:  draw_truck(x, y, w, color, dir); break;
        case SHAPE_LOG:    draw_log(x, y, w, color); break;
        case SHAPE_TURTLE: draw_turtles(x, y, w, color); break;
    }
}

// Dibuja/borra los objetos de carril cuya posición en pantalla (o
// visibilidad, para las tortugas) cambió desde el frame anterior.
static void draw_lanes_if_moved(void) {
    bool any = false;
    for (int t = 0; t < 2; t++) {
        int n = (t == 0) ? N_RIVER_LANES : N_ROAD_LANES;
        Lane *lanes = (t == 0) ? river_lanes : road_lanes;
        int row0 = (t == 0) ? ROW_RIVER_FIRST : ROW_ROAD_FIRST;
        for (int L = 0; L < n; L++) {
            Lane *ln = &lanes[L];
            int y = row_y(row0 + L);
            uint16_t bg = row_bg_color(row0 + L);
            for (int o = 0; o < OBJ_PER_LANE; o++) {
                int nx = obj_x[t][L][o], px = obj_prev_x[t][L][o];
                bool nv = obj_visible[t][L][o], pv = obj_prev_visible[t][L][o];
                if (nx == px && nv == pv) continue;
                if (pv) fill_clipped(px, y, ln->width_px, bg);
                if (nv) draw_lane_object(ln->shape, nx, y, ln->width_px, ln->color, ln->dir);
                any = true;
            }
        }
    }
    if (any) renderer_flush();
}

// Colisión con el coche/camión -- caja algo más pequeña que la
// celda/sprite dibujado en ambos lados (rana y vehículo), para que
// la colisión case con lo que se ve en pantalla y no "toque" un
// par de píxeles antes de solapar visualmente.
//
// Ya NO usa barrido prev->nuevo: esa protección contra "teleporte"
// tenía sentido con las velocidades originales, pero con el tráfico
// ya bastante más lento (ver init_lanes) apenas hace falta, y en
// cambio agrandaba la caja de colisión más de lo que se ve --
// exactamente la sensación de "choca antes de tiempo" que se
// reportó. Si en el futuro se suben mucho las velocidades, puede
// hacer falta reintroducirlo.
#define FROG_HIT_INSET 4   // píxeles de margen a cada lado de la celda de la rana que NO cuentan como colisión
#define VEH_HIT_INSET  3   // ídem para el coche/camión

static bool road_hit_at(int lane_idx, int player_x) {
    int p0 = player_x + FROG_HIT_INSET;
    int p1 = player_x + CELL_W - FROG_HIT_INSET;
    for (int o = 0; o < OBJ_PER_LANE; o++) {
        int x0 = obj_x[1][lane_idx][o] + VEH_HIT_INSET;
        int w  = road_lanes[lane_idx].width_px - 2*VEH_HIT_INSET;
        if (!(x0 + w <= p0 || x0 >= p1)) return true;
    }
    return false;
}

// ¿Hay tronco/tortuga (visible) bajo esta X en este carril de río?
// Si lo hay, además devuelve la velocidad con signo del carril (para
// que la rana se desplace exactamente igual que el objeto que la
// lleva).
static bool river_ride_at(int lane_idx, int player_x, int32_t *out_speed_signed_fp) {
    for (int o = 0; o < OBJ_PER_LANE; o++) {
        if (!obj_visible[0][lane_idx][o]) continue;
        int x = obj_x[0][lane_idx][o];
        int w = river_lanes[lane_idx].width_px;
        if (player_x + CELL_W > x && player_x < x + w) {
            *out_speed_signed_fp = (int32_t)river_lanes[lane_idx].dir * river_lanes[lane_idx].speed_fp;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Rana -- posición X continua (FP) porque puede ir a la deriva
// mientras monta un tronco; fila discreta.
//
// pstate es el sub-estado INDIVIDUAL de cada rana: con 2 jugadores
// SIMULTÁNEOS ya no hay un único "turno" -- un jugador puede estar
// muriendo (parpadeo) mientras el otro sigue saltando con total
// normalidad, así que cada Frog lleva su propio temporizador de
// pausa y su propia información de parpadeo de muerte, en vez de
// las variables globales que bastaban cuando solo había una rana en
// pantalla a la vez.
// ---------------------------------------------------------------------------
typedef enum { DEATH_SQUASHED, DEATH_DROWNED, DEATH_TIMEOUT, DEATH_HEDGE, DEATH_EDGE } DeathCause;
typedef enum { HOP_NONE, HOP_UP, HOP_DOWN, HOP_LEFT, HOP_RIGHT } HopDir;
typedef enum { PS_PLAYING, PS_DYING, PS_HOME_PAUSE, PS_DONE } PlayerState;

typedef struct {
    int32_t       x_fp;
    int           row;
    int           best_row;     // fila más alta (número más bajo) alcanzada esta vida
    int           lives;
    int32_t       score;
    int32_t       timer_ms;
    PlayerState   pstate;
    absolute_time_t pause_until;    // fin de la pausa, si pstate es PS_DYING o PS_HOME_PAUSE
    int           death_row, death_col;
    uint16_t      death_flash_color;
} Frog;

static Frog frogs[2];
static int  n_players;
static int  menu_enc_acc;   // acumulador del encoder de J1 para el selector 1P/2P en FR_READY (ver pong.c)
static bool demo;

static int32_t time_limit_ms(int level) {
    int32_t v = 25000 - (level - 1) * 1000;
    return clampi(v, 15000, 25000);
}

static void spawn_frog(int player) {
    Frog *f = &frogs[player];
    f->row = ROW_START;
    // Con 2 jugadores, cada uno aparece desplazado del centro (uno a
    // la izquierda, otro a la derecha) para no solaparse en la
    // salida; en 1P, el jugador 0 sigue apareciendo centrado.
    int col = COLS/2;
    if (n_players == 2) col = (player == 0) ? COLS/2 - 3 : COLS/2 + 3;
    f->x_fp = PX2FP(col_x(col));
    f->best_row = ROW_START;
    f->timer_ms = time_limit_ms(level_g);
    f->pstate = PS_PLAYING;
}

// ---------------------------------------------------------------------------
// Render de la rana -- cuerpo simple (cuadrado + ojos), fácil de
// reconocer a 22x18px. El borrado usa cell_bg_color() de la fila
// anterior (funciona igual en carretera/río/césped/meta).
// ---------------------------------------------------------------------------
static void draw_frog_shape(int x, int y, uint16_t color) {
    renderer_fill_rect(x+1, y+9, 4, 6, color);                 // pata izq
    renderer_fill_rect(x+CELL_W-5, y+9, 4, 6, color);          // pata der
    renderer_fill_rect(x+4, y+5, CELL_W-8, CELL_H-8, color);   // cuerpo
    renderer_fill_rect(x+5, y+3, 4, 4, COLOR_WHITE);           // ojo izq
    renderer_fill_rect(x+CELL_W-9, y+3, 4, 4, COLOR_WHITE);    // ojo der
    renderer_fill_rect(x+6, y+4, 2, 2, COLOR_BLACK);           // pupila izq
    renderer_fill_rect(x+CELL_W-8, y+4, 2, 2, COLOR_BLACK);    // pupila der
}

static int  prev_frog_x[2]   = { -9999, -9999 };
static int  prev_frog_row[2] = { -1, -1 };
static bool prev_frog_shown[2] = { false, false };

static void draw_frog_if_moved(int player, bool shown) {
    Frog *f = &frogs[player];
    int x = FP2PX(f->x_fp), y = row_y(f->row);
    if (!shown && !prev_frog_shown[player]) return;
    if (x == prev_frog_x[player] && f->row == prev_frog_row[player] && shown == prev_frog_shown[player]) return;

    if (prev_frog_shown[player]) {
        int pcol = (prev_frog_x[player] - GRID_X0) / CELL_W;
        fill_clipped(prev_frog_x[player], row_y(prev_frog_row[player]), CELL_W,
                     cell_bg_color(prev_frog_row[player], pcol));
    }
    if (shown) draw_frog_shape(x, y, frog_color(player));

    prev_frog_x[player] = x; prev_frog_row[player] = f->row; prev_frog_shown[player] = shown;
    renderer_flush();
}

// ---------------------------------------------------------------------------
// Parpadeo de muerte -- sustituye al sistema de partículas de
// lunar_lander.c (ver cabecera del archivo): no hace falta simular
// nada, solo alternar el color de la celda unos instantes. Por
// jugador (Frog.death_row/death_col/death_flash_color) porque con 2
// jugadores simultáneos ambos pueden estar parpadeando a la vez en
// celdas distintas.
// ---------------------------------------------------------------------------
static uint16_t death_cause_color(DeathCause c) {
    switch (c) {
        case DEATH_SQUASHED: return COLOR_RED;
        case DEATH_DROWNED:  return COLOR_CYAN;
        case DEATH_TIMEOUT:  return COLOR_YELLOW;
        case DEATH_HEDGE:    return COLOR_WHITE;
        case DEATH_EDGE:     return COLOR_CYAN;
    }
    return COLOR_RED;
}

static void draw_death_flash_player(int player, int blink_val) {
    Frog *f = &frogs[player];
    int x = col_x(f->death_col), y = row_y(f->death_row);
    uint16_t c = (blink_val % 6 < 3) ? f->death_flash_color : cell_bg_color(f->death_row, f->death_col);
    fill_clipped(x, y, CELL_W, c);
    renderer_flush();
}

static void clear_death_flash_player(int player) {
    Frog *f = &frogs[player];
    fill_clipped(col_x(f->death_col), row_y(f->death_row), CELL_W, cell_bg_color(f->death_row, f->death_col));
    renderer_flush();
}

// ---------------------------------------------------------------------------
// Tablero estático -- se dibuja una vez por nivel. Los carriles en
// movimiento y la rana se pintan encima cada tick (ver más arriba).
// ---------------------------------------------------------------------------
static bool field_needs_redraw = true;
static char prev_msg[40] = "";   // declarado aquí (antes de draw_board_static, que lo usa); update_message() más abajo lo reutiliza

static void draw_home_row(void) {
    int y = row_y(ROW_HOME);
    for (int c = 0; c < COLS; c++) {
        int k = home_slot_index(c);
        uint16_t col = (k < 0) ? COLOR_HEDGE : (home_filled[k] ? frog_color(home_filled_by[k]) : COLOR_RIVER);
        renderer_fill_rect(col_x(c), y, CELL_W, CELL_H, col);
    }
}

static void draw_board_static(void) {
    renderer_clear(COLOR_BLACK);
    draw_home_row();
    for (int r = ROW_RIVER_FIRST; r <= ROW_RIVER_LAST; r++)
        renderer_fill_rect(GRID_X0, row_y(r), GRID_W, CELL_H, COLOR_RIVER);
    renderer_fill_rect(GRID_X0, row_y(ROW_MEDIAN), GRID_W, CELL_H, COLOR_GRASS);
    for (int r = ROW_ROAD_FIRST; r <= ROW_ROAD_LAST; r++)
        renderer_fill_rect(GRID_X0, row_y(r), GRID_W, CELL_H, COLOR_ROAD);
    renderer_fill_rect(GRID_X0, row_y(ROW_START), GRID_W, CELL_H, COLOR_GRASS);
    renderer_fill_rect(PLAY_X, row_y(ROW_HUD1), PLAY_W, CELL_H*2, COLOR_BLACK);

    reset_obj_trace();
    prev_frog_shown[0] = prev_frog_shown[1] = false;
    prev_msg[0] = '\0';
    field_needs_redraw = false;
    renderer_flush();
}

// ---------------------------------------------------------------------------
// HUD -- franja dedicada (filas 11-12, ver cabecera del archivo).
// ---------------------------------------------------------------------------
static int prev_hud_score[2] = { -1, -1 };
static int prev_hud_lives[2] = { -1, -1 };
static int prev_hud_timer_s[2] = { -1, -1 };
static int prev_hud_level = -1;
static absolute_time_t next_hud_refresh;

static void draw_hud_if_changed(bool force_all) {
    char buf[24];
    bool changed = false;
    bool force = force_all || time_reached(next_hud_refresh);
    if (time_reached(next_hud_refresh)) next_hud_refresh = make_timeout_time_ms(500);

    int y1 = row_y(ROW_HUD1), y2 = row_y(ROW_HUD2);

    if (level_g != prev_hud_level || force) {
        renderer_fill_rect(CX-40, y1, 80, CELL_H, COLOR_BLACK);
        snprintf(buf, sizeof(buf), "NIV %d", level_g);
        renderer_draw_text(centered_x(buf, 2), y1+2, buf, COLOR_WHITE, COLOR_BLACK, 2);
        prev_hud_level = level_g; changed = true;
    }

    // Jugadores simultáneos: cada uno tiene su propio marcador Y su
    // propio contador de segundos restantes (antes había una única
    // barra de tiempo para el "jugador activo", que ya no existe
    // como concepto). El contador en texto ("VIDAS n  Ts") ocupa
    // mucho menos que una barra gráfica por jugador, necesario para
    // que quepan los dos lados en la fila 2.
    for (int p = 0; p < n_players; p++) {
        if ((int)frogs[p].score != prev_hud_score[p] || force) {
            int x = (p == 0) ? PLAY_X+2 : PLAY_X+PLAY_W-100;
            renderer_fill_rect(x, y1, 98, CELL_H, COLOR_BLACK);
            if (n_players == 2) snprintf(buf, sizeof(buf), "P%d %ld", p+1, (long)frogs[p].score);
            else                snprintf(buf, sizeof(buf), "%ld", (long)frogs[p].score);
            int tx = (p == 0) ? x : x+98-(int)st7789_text_width(buf, 2);
            renderer_draw_text(tx, y1+2, buf, frog_color(p), COLOR_BLACK, 2);
            prev_hud_score[p] = (int)frogs[p].score; changed = true;
        }
        int secs = (int)(frogs[p].timer_ms / 1000); if (secs < 0) secs = 0;
        if (frogs[p].lives != prev_hud_lives[p] || secs != prev_hud_timer_s[p] || force) {
            int x = (p == 0) ? PLAY_X+2 : PLAY_X+PLAY_W-100;
            renderer_fill_rect(x, y2, 98, CELL_H, COLOR_BLACK);
            if (frogs[p].pstate == PS_DONE) snprintf(buf, sizeof(buf), "FUERA");
            else                            snprintf(buf, sizeof(buf), "VIDAS %d  %ds", frogs[p].lives, secs);
            uint16_t col = (frogs[p].pstate != PS_DONE && secs <= 5) ? COLOR_RED : COLOR_WHITE;
            int tx = (p == 0) ? x : x+98-(int)st7789_text_width(buf, 1);
            renderer_draw_text(tx, y2+3, buf, col, COLOR_BLACK, 1);
            prev_hud_lives[p] = frogs[p].lives; prev_hud_timer_s[p] = secs; changed = true;
        }
    }

    if (changed) renderer_flush();
}

// Mensajes centrados sobre la mediana (fila 5: césped, sin peligros,
// el único sitio "libre" del tablero para superponer texto).
static void update_message(const char *target, uint16_t color, int scale) {
    if (strcmp(target, prev_msg) == 0) return;
    int y = row_y(ROW_MEDIAN);
    renderer_fill_rect(GRID_X0, y, GRID_W, CELL_H, COLOR_GRASS);
    if (target[0]) renderer_draw_text(centered_x(target, scale), y+2, target, color, COLOR_GRASS, scale);
    strncpy(prev_msg, target, sizeof(prev_msg)-1);
    prev_msg[sizeof(prev_msg)-1] = '\0';
    renderer_flush();
}

// ---------------------------------------------------------------------------
// Estado de partida y máquina de estados.
// ---------------------------------------------------------------------------
// FR_DYING y FR_HOME ya no son estados globales: con 2 jugadores
// simultáneos cada rana tiene su propio sub-estado (Frog.pstate,
// ver más abajo) para eso, independiente de la otra.
typedef enum { FR_READY, FR_PLAYING, FR_LEVEL_CLEAR, FR_GAME_OVER, FR_SCORES } FRState;

static FRState state;
static int blink;
static bool g_done;
static int demo_ticks;
static absolute_time_t pause_until;
static absolute_time_t game_over_deadline;
static absolute_time_t ready_input_ok_time;
static int32_t enc_acc[2];
static absolute_time_t next_hop_ok[2];

#define HOP_COOLDOWN_MS     110
#define HOP_POINTS          10
#define HOME_POINTS         50
#define LEVEL_CLEAR_BONUS   200
#define TIME_BONUS_PER_SEC  2

static void draw_ready_screen(void);
static void draw_scores_screen(void);

static void trigger_death(int player, DeathCause cause) {
    Frog *f = &frogs[player];
    f->death_row = f->row;
    f->death_col = clampi((FP2PX(f->x_fp) - GRID_X0 + CELL_W/2) / CELL_W, 0, COLS-1);
    f->death_flash_color = death_cause_color(cause);
    f->lives--;
    sound_effect_explosion();
    f->pause_until = make_timeout_time_ms(1200);
    f->pstate = PS_DYING;
}

/*
 * Ejecuta un salto de una celda. La rana solo se desplaza en firme al
 * FINAL de la función (si no acaba muriendo o llegando a meta) --
 * eso evita dejar al jugador "a medio camino" si el destino resulta
 * ser un seto o una plataforma ya ocupada.
 */
static void perform_hop(int player, HopDir dir) {
    Frog *f = &frogs[player];
    int new_row = f->row;
    int32_t new_x = f->x_fp;

    switch (dir) {
        case HOP_UP:    new_row = f->row - 1;sound_effect_jump(); break;
        case HOP_DOWN:  new_row = f->row + 1; sound_effect_powerdown(); break;
        case HOP_LEFT:  new_x -= PX2FP(CELL_W); sound_effect_move(); break;
        case HOP_RIGHT: new_x += PX2FP(CELL_W); sound_effect_move(); break;
        default: return;
    }
    new_row = clampi(new_row, 0, ROW_MAX_PLAYABLE);
    new_x = clampi(new_x, PX2FP(GRID_X0), PX2FP(GRID_X0 + (COLS-1)*CELL_W));

    if (new_row == f->row && new_x == f->x_fp) return;   // bloqueado contra un borde

    

    if (new_row == ROW_HOME) {
        // Borra el sprite de la rana en su última posición dibujada
        // ANTES de decidir qué pasa -- si no, al llegar a meta se ve
        // duplicada (el hueco relleno de color Y la rana vieja sin
        // borrar), y si choca contra un seto la rana sigue "flotando"
        // en su fila anterior mientras el parpadeo de muerte aparece
        // en la fila de meta.
        draw_frog_if_moved(player, false);

        int col = clampi((FP2PX(new_x) - GRID_X0 + CELL_W/2) / CELL_W, 0, COLS-1);
        int k = home_slot_index(col);
        if (k >= 0 && !home_filled[k]) {
            home_filled[k] = true; home_filled_by[k] = player;
            int32_t bonus = (f->timer_ms / 1000) * TIME_BONUS_PER_SEC;
            f->score += HOME_POINTS + bonus;
            sound_effect_coin();
            draw_home_row();
            renderer_flush();
            f->pause_until = make_timeout_time_ms(900);
            f->pstate = PS_HOME_PAUSE;
        } else {
            // Confirma la posición fallida ANTES de disparar la
            // muerte, para que el parpadeo salga en la celda correcta
            // (la del seto contra el que chocó), no en la fila de la
            // que venía.
            f->row = new_row; f->x_fp = new_x;
            trigger_death(player, DEATH_HEDGE);
        }
        return;
    }

    f->row = new_row; f->x_fp = new_x;
    if (new_row < f->best_row) { f->score += HOP_POINTS; f->best_row = new_row; }
}

// Física continua del jugador activo: colisión con tráfico, arrastre
// sobre troncos/tortugas (o ahogo si no hay ninguno debajo), y
// cuenta atrás del temporizador. Se llama una vez por tick mientras
// FR_PLAYING.
static void frog_physics_tick(int player) {
    Frog *f = &frogs[player];
    int row = f->row;
    int px = FP2PX(f->x_fp);

    if (row >= ROW_ROAD_FIRST && row <= ROW_ROAD_LAST) {
        int lane = row - ROW_ROAD_FIRST;
        if (road_hit_at(lane, px)) { trigger_death(player, DEATH_SQUASHED); return; }
    } else if (row >= ROW_RIVER_FIRST && row <= ROW_RIVER_LAST) {
        int lane = row - ROW_RIVER_FIRST;
        int32_t spd;
        if (river_ride_at(lane, px, &spd)) {
            f->x_fp += spd * g_dt_scale / FP;
            int nx = FP2PX(f->x_fp);
            if (nx + CELL_W <= GRID_X0 || nx >= GRID_X0 + GRID_W) {
                trigger_death(player, DEATH_EDGE); return;
            }
        } else {
            trigger_death(player, DEATH_DROWNED); return;
        }
    }

    f->timer_ms -= g_elapsed_ms;
    if (f->timer_ms <= 0) trigger_death(player, DEATH_TIMEOUT);
}

// IA sencilla para el modo demo: esquiva el peligro más cercano en
// carretera, busca el tronco/tortuga más próximo en el río, y avanza
// hacia arriba el resto del tiempo.
static HopDir demo_ai(int player) {
    Frog *f = &frogs[player];
    int row = f->row, px = FP2PX(f->x_fp);

    if (row >= ROW_ROAD_FIRST && row <= ROW_ROAD_LAST) {
        int lane = row - ROW_ROAD_FIRST;
        for (int o = 0; o < OBJ_PER_LANE; o++) {
            int ox = obj_x[1][lane][o];
            int w  = road_lanes[lane].width_px;
            int approach = (road_lanes[lane].dir > 0) ? (px - (ox + w)) : (ox - (px + CELL_W));
            if (approach >= -w && approach < CELL_W*2) return rnd(2) ? HOP_LEFT : HOP_RIGHT;
        }
    } else if (row >= ROW_RIVER_FIRST && row <= ROW_RIVER_LAST) {
        int lane = row - ROW_RIVER_FIRST;
        int32_t spd;
        if (!river_ride_at(lane, px, &spd)) {
            int best_dx = 0; bool found = false;
            for (int o = 0; o < OBJ_PER_LANE; o++) {
                if (!obj_visible[0][lane][o]) continue;
                int dx = obj_x[0][lane][o] - px;
                if (!found || absi(dx) < absi(best_dx)) { best_dx = dx; found = true; }
            }
            if (found) return best_dx > 0 ? HOP_RIGHT : HOP_LEFT;
        }
    }
    if (rnd(6) == 0) return rnd(2) ? HOP_LEFT : HOP_RIGHT;
    return HOP_UP;
}

static void handle_input(int player) {
    HopDir dir = HOP_NONE;
    if (demo) {
        dir = demo_ai(player);
    } else {
        int btn_a = (player == 0) ? BTN_J1_A : BTN_J2_A;
        int btn_b = (player == 0) ? BTN_J1_B : BTN_J2_B;
        if (controls_button_pressed(btn_a)) {
            dir = HOP_UP;
        } else if (controls_button_pressed(btn_b)) {
            dir = HOP_DOWN;
        } else {
            int d = controls_get_raw_delta(player);
            enc_acc[player] += d;
            if (enc_acc[player] >= 4)       { dir = HOP_RIGHT; enc_acc[player] -= 4; }
            else if (enc_acc[player] <= -4) { dir = HOP_LEFT;  enc_acc[player] += 4; }
        }
    }
    if (dir != HOP_NONE && time_reached(next_hop_ok[player])) {
        perform_hop(player, dir);
        next_hop_ok[player] = make_timeout_time_ms(HOP_COOLDOWN_MS);
    }
}

// Decide qué pasa cuando termina la pausa de una rana (murió o llegó
// a una meta que no completó el nivel): la deja lista para la
// siguiente vida, o la marca "fuera" si ya no le quedan. Con 2
// jugadores SIMULTÁNEOS esto ya no implica ningún cambio de turno --
// cada jugador sigue su propio ciclo con total independencia del
// otro.
static void next_life(int player) {
    Frog *f = &frogs[player];
    if (f->lives <= 0) { f->pstate = PS_DONE; return; }
    // Fuerza el redibujado inmediato en el próximo draw_frog_if_moved
    // en vez de confiar en que la comparación con la posición
    // anterior ya lo detecte como "visible por primera vez" -- así
    // la rana aparece en su celda de salida desde el primer tick,
    // sin esperar a que el jugador la mueva.
    prev_frog_shown[player] = false;
    spawn_frog(player);   // deja pstate = PS_PLAYING
}

static void begin_level(void) {
    for (int k = 0; k < N_HOME_SLOTS; k++) home_filled[k] = false;
    init_lanes(level_g);
    field_needs_redraw = true;
    for (int p = 0; p < n_players; p++) {
        if (frogs[p].lives > 0) spawn_frog(p);
        else                    frogs[p].pstate = PS_DONE;
    }
}

static void game_start(void) {
    level_g = 1;
    for (int p = 0; p < 2; p++) { frogs[p].lives = 3; frogs[p].score = 0; }
    for (int k = 0; k < N_HOME_SLOTS; k++) home_filled[k] = false;
    init_lanes(level_g);
    for (int p = 0; p < n_players; p++) spawn_frog(p);
    for (int p = n_players; p < 2; p++) frogs[p].pstate = PS_DONE;   // rana no usada en 1P
    enc_acc[0] = enc_acc[1] = 0;
    next_hop_ok[0] = next_hop_ok[1] = get_absolute_time();
    prev_frog_shown[0] = prev_frog_shown[1] = false;
    prev_hud_score[0] = prev_hud_score[1] = -1;
    prev_hud_lives[0] = prev_hud_lives[1] = -1;
    prev_hud_timer_s[0] = prev_hud_timer_s[1] = -1;
    prev_hud_level = -1;
    prev_msg[0] = '\0';
}

static void fr_tick(void) {
    blink++;
    update_dt_scale();

    if (demo) {
        bool any = controls_menu_select() || controls_get_raw_delta(0) != 0 || controls_get_raw_delta(1) != 0
                 || controls_button_down(BTN_J1_A) || controls_button_down(BTN_J1_B);
        if (any || ++demo_ticks >= 60*30) { g_done = true; return; }
    }

    switch (state) {

    case FR_READY:
        if (!demo) {
            int d = controls_get_raw_delta(0);
            if (d) {
                menu_enc_acc += d;
                if (menu_enc_acc >= 4)  { n_players = (n_players == 1) ? 2 : 1; menu_enc_acc = 0; draw_ready_screen(); }
                if (menu_enc_acc <= -4) { n_players = (n_players == 1) ? 2 : 1; menu_enc_acc = 0; draw_ready_screen(); }
            }
        }
        if (time_reached(ready_input_ok_time) && controls_menu_select()) {
            sound_stop_menu_music();
            game_start();
            field_needs_redraw = true;
            state = FR_PLAYING;
        }
        break;

    // 2 jugadores SIMULTÁNEOS: cada rana sigue su propio sub-estado
    // (Frog.pstate) de forma independiente dentro del mismo tick --
    // una puede estar parpadeando tras morir mientras la otra sigue
    // saltando con total normalidad. El progreso de huecos de meta
    // (home_filled) es compartido: el nivel se completa en cuanto
    // los 5 están llenos, sin esperar a que el otro jugador termine
    // su cruce.
    case FR_PLAYING: {
        update_lanes();

        for (int p = 0; p < n_players; p++) {
            Frog *f = &frogs[p];
            switch (f->pstate) {
                case PS_PLAYING:
                    handle_input(p);
                    if (f->pstate == PS_PLAYING) frog_physics_tick(p);
                    break;
                case PS_DYING:
                    draw_death_flash_player(p, blink);
                    if (time_reached(f->pause_until)) {
                        clear_death_flash_player(p);
                        next_life(p);
                    }
                    break;
                case PS_HOME_PAUSE:
                    if (time_reached(f->pause_until)) next_life(p);
                    break;
                case PS_DONE:
                    break;
            }
        }

        if (field_needs_redraw) draw_board_static();
        draw_lanes_if_moved();
        for (int p = 0; p < n_players; p++)
            draw_frog_if_moved(p, frogs[p].pstate == PS_PLAYING);
        draw_hud_if_changed(false);

        bool all_filled = true;
        for (int k = 0; k < N_HOME_SLOTS; k++) if (!home_filled[k]) all_filled = false;
        if (all_filled) {
            for (int p = 0; p < n_players; p++) frogs[p].score += LEVEL_CLEAR_BONUS;
            sound_effect_victory();
            update_message("NIVEL COMPLETO", COLOR_YELLOW, 2);
            pause_until = make_timeout_time_ms(1500);
            state = FR_LEVEL_CLEAR;
            break;
        }

        bool all_done = true;
        for (int p = 0; p < n_players; p++) if (frogs[p].pstate != PS_DONE) all_done = false;
        if (all_done) {
            sound_effect_alarm();
            update_message("GAME OVER", COLOR_RED, 3);
            pause_until = make_timeout_time_ms(2000);
            game_over_deadline = make_timeout_time_ms(8000);   // avanza solo si nadie pulsa nada
            state = FR_GAME_OVER;
        }
        break;
    }

    case FR_LEVEL_CLEAR:
        draw_hud_if_changed(false);   // refleja el bonus de nivel completado
        if (time_reached(pause_until)) {
            update_message("", COLOR_WHITE, 1);
            level_g++;
            begin_level();
            state = FR_PLAYING;
        }
        break;

    case FR_GAME_OVER:
        update_message((blink/15)%2==0 ? "PULSA PARA CONTINUAR" : "GAME OVER", COLOR_YELLOW, 2);
        if (time_reached(pause_until) &&
            (controls_menu_select() || time_reached(game_over_deadline))) {
            if (!demo) {
                for (int p = 0; p < n_players; p++)
                    if (highscores_is_top(FR_GAME_ID, (uint32_t)frogs[p].score))
                        highscores_enter(FR_GAME_ID, (uint32_t)frogs[p].score);   // bloqueante
            }
            draw_scores_screen();
            pause_until = make_timeout_time_ms(8000);
            state = FR_SCORES;
        }
        break;

    case FR_SCORES:
        if (controls_menu_select() || time_reached(pause_until)) g_done = true;
        break;
    }
}

// ---------------------------------------------------------------------------
// Pantallas "estáticas"
// ---------------------------------------------------------------------------
static void draw_ready_screen(void) {
    renderer_clear(COLOR_BLACK);
    renderer_draw_text(centered_x("FROGGER", 4), 6, "FROGGER", COLOR_GREEN, COLOR_BLACK, 4);

    // Rana grande decorativa (no reutiliza draw_frog_shape porque
    // esa está pensada para el tamaño de celda del tablero, 22x18).
    int fw = 60, fh = 46, fx = CX - fw/2, fy = 40;
    renderer_fill_rect(fx, fy, fw, fh, COLOR_FROG1);
    renderer_fill_rect(fx+10,       fy+9, 9, 9, COLOR_WHITE);
    renderer_fill_rect(fx+fw-19,    fy+9, 9, 9, COLOR_WHITE);
    renderer_fill_rect(fx+12,       fy+11, 4, 4, COLOR_BLACK);
    renderer_fill_rect(fx+fw-17,    fy+11, 4, 4, COLOR_BLACK);

    const char *l1 = "ARRIBA: A    ABAJO: B";
    const char *l2 = "GIRA: IZQUIERDA/DERECHA";
    renderer_draw_text(centered_x(l1, 2), 92, l1, COLOR_WHITE, COLOR_BLACK, 2);
    renderer_draw_text(centered_x(l2, 2), 114, l2, COLOR_WHITE, COLOR_BLACK, 2);

    // Selector de 1/2 jugadores -- igual que en pong.c: se gira el
    // encoder de J1 para cambiar (aún no se usa para nada más en
    // esta pantalla) y se resalta la opción activa con guiones.
    const char *s1 = (n_players == 1) ? "- 1 JUGADOR -" : "  1 JUGADOR  ";
    const char *s2 = (n_players == 2) ? "- 2 JUGADORES -" : "  2 JUGADORES  ";
    renderer_draw_text(centered_x(s1, 2), 138, s1, COLOR_WHITE, COLOR_BLACK, 2);
    renderer_draw_text(centered_x(s2, 2), 160, s2, COLOR_WHITE, COLOR_BLACK, 2);

    const char *l3 = "GIRA PARA CAMBIAR MODO";
    const char *l4 = "PULSA PARA JUGAR";
    renderer_draw_text(centered_x(l3, 1), 184, l3, COLOR_GREEN,  COLOR_BLACK, 1);
    renderer_draw_text(centered_x(l4, 2), 198, l4, COLOR_YELLOW, COLOR_BLACK, 2);

    prev_msg[0] = '\0';
    renderer_flush();
}

static void draw_scores_screen(void) {
    renderer_clear(COLOR_BLACK);
    highscores_draw(FR_GAME_ID, "FROGGER", 20);
    renderer_flush();
}

// ---------------------------------------------------------------------------
// API pública
// ---------------------------------------------------------------------------
void game_frogger_run(game_mode_t mode) {
    srand(time_us_32());

    demo = (mode == GAME_MODE_DEMO);
    n_players = (mode == GAME_MODE_2P) ? 2 : 1;
    menu_enc_acc = 0;
    blink = 0; demo_ticks = 0; g_done = false;
    field_needs_redraw = true;
    last_tick_time = get_absolute_time();
    reset_obj_trace();
    prev_frog_shown[0] = prev_frog_shown[1] = false;

    if (demo) {
        n_players = 1;
        game_start();
        state = FR_PLAYING;
    } else {
        state = FR_READY;
        draw_ready_screen();
        sound_start_menu_music();
        ready_input_ok_time = make_timeout_time_ms(300);
    }

    while (!g_done) {
        controls_update();
        fr_tick();
        sound_update();
        sleep_ms(8);
    }

    highscores_flush();
}