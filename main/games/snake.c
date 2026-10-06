/**
 * snake.c -- remake de "snake" competitivo de 2 serpientes estilo
 * neon/Tron, adaptado al mismo esquema que asteroids.c/
 * lunar_lander.c/frogger.c a partir de un prototipo HTML de
 * referencia (snake.html) que ya usaba esta misma resolucion logica
 * (320x240, PLAY_X/Y/W/H) y margenes de juego.
 *
 * DECISIONES DE ADAPTACION respecto al prototipo HTML:
 *
 *  - SIN resplandor (shadowBlur) ni estela semitransparente tras la
 *    serpiente: el hardware solo pinta rectangulos planos, sin
 *    mezcla alfa. El look "neon" se conserva en la PALETA (cian /
 *    violeta sobre negro), no en el resplandor -- que aqui no existe.
 *
 *  - RENDER CASI GRATIS, a proposito distinto de frogger.c: alli el
 *    trafico se mueve en continuo y hay que redibujar cada tick.
 *    Aqui el tablero solo cambia en los "pasos de movimiento" (cada
 *    ~90-180ms, NO cada tick de render) y cada paso toca como mucho
 *    3 celdas por serpiente (recolorea la cabeza vieja a color de
 *    cuerpo, pinta la cabeza nueva, borra la cola si no ha crecido).
 *    No hace falta ningun sistema de "solo redibuja si cambio" como
 *    en asteroids/frogger: sabemos exactamente que celdas cambiaron
 *    en cada paso, asi que se dibujan directamente.
 *
 *  - Serpiente como BUFFER CIRCULAR de longitud fija
 *    (MAX_SNAKE_LEN=256): moverse es O(1) -- retrocede head_idx y
 *    escribe la celda nueva -- sin desplazar el resto del array como
 *    haria un array-lista clasico.
 *
 *  - CONTROL ABSOLUTO con el stick (arriba/abajo/izquierda/derecha =
 *    direccion de la serpiente), como las flechas/WASD del prototipo,
 *    pero solo se admiten giros de 90 grados respecto al ultimo
 *    movimiento: el sentido contrario (180) se ignora, porque seria
 *    meterse de cabeza contra el propio cuello. Ver handle_turn_input().
 *
 *  - IMPULSO ("boost") con el boton A del jugador mantenido: acelera el ritmo de
 *    movimiento de AMBAS serpientes mientras lo mantenga pulsado
 *    CUALQUIERA de los dos jugadores humanos (igual que el
 *    prototipo, que usa max(boost1,boost2)) -- para no desincronizar
 *    el paso de cuadricula, que es compartido.
 *
 *  - Franja de HUD dedicada (2 filas superiores de la cuadricula,
 *    16px) en vez de la barra que el prototipo redibujaba encima del
 *    tablero cada frame: aqui, como en frogger.c, se reservan filas
 *    fuera del area jugable para no tener que "restaurar lo que
 *    habia debajo" cada vez que cambia el marcador.
 *
 *  - 1 jugador = humano vs IA (como pong.c), 2 jugadores = ambos
 *    humanos, demo = ambos IA. La IA evita colisiones inmediatas
 *    (propio cuerpo, pared, rival) y prioriza acercarse a la comida,
 *    mismo nivel de heuristica que el resto de demo_ai() del
 *    proyecto.
 *
 *  - SUBIDA DE NIVEL por FRUTAS (rediseñada): el prototipo comprobaba
 *    "puntuacion_combinada % (500*nivel) === 0", que podia no disparar
 *    nunca; la primera version del port uso un umbral de puntos
 *    (5000 en el nivel 1, +1000*nivel despues), que hacia el nivel 1
 *    cinco veces mas largo que los demas (50 frutas frente a 10) y
 *    dejaba que la IA subiera el nivel por ti. Ahora cada nivel
 *    exige FOODS_PER_LEVEL (10) frutas DEL HUMANO (la roja cuenta
 *    SPECIAL_FOOD_PROGRESS = 3); el progreso se ve como una barra de
 *    10 casillas en el centro del HUD, y al empezar cada ronda el
 *    mensaje dice cuantas faltan.
 *
 *  - REGLAS DE LA PARTIDA:
 *      * Subir de nivel: mas rapido (-10 ms/paso, 180 -> 90 ms), otro
 *        diseño de tablero (6 diseños en ciclo + obstaculos sueltos
 *        que crecen con el nivel) y la longitud de las serpientes se
 *        CONSERVA (hasta CARRY_MAX_GROWTH): reaparecen cortas y
 *        recuperan esos segmentos en los primeros pasos.
 *      * Zona de aparicion segura: sin paredes en el pasillo de salida
 *        de cada serpiente; ademas, cualquier hueco que quedase
 *        aislado se rellena (fill_pockets), asi que toda fruta es
 *        alcanzable.
 *      * 1 jugador: la IA NO gasta vidas y su muerte NO termina la
 *        ronda -- reaparece a los AI_RESPAWN_MS en la esquina libre
 *        mas lejana. Solo termina la ronda si mueres tu.
 *      * 2 jugadores: ganar una ronda da ROUND_WIN_BONUS_MUL*nivel
 *        puntos; la partida termina cuando uno se queda sin vidas y
 *        el otro la gana (si ambos a la vez, el de mas puntos).
 *      * Impulso (A mantenido): mas velocidad Y las frutas valen x2.
 *      * Vida extra cada EXTRA_LIFE_EVERY puntos (maximo MAX_LIVES).
 *      * Entrada: buffer de 2 giros para encadenar un giro en U.
 *
 *  - Titulo mostrado en pantalla: "SNAKE" (no "TRON SNAKE" -- se
 *    evita el nombre de la franquicia; la estetica neon si se
 *    conserva en la paleta de colores).
 *
 * PORT A ESP32 -- igual que el resto de juegos ya portados: nada de
 * Pico SDK (pico/stdlib.h, absolute_time_t, sleep_ms, time_us_32),
 * sustituido por esp_timer.h + FreeRTOS:
 *  - Tiempo real: last_tick_time, pause_until, game_over_deadline y
 *    ready_input_ok_time eran absolute_time_t -- ahora uint32_t en ms
 *    via esp_timer_get_time() (now_ms()/make_timeout_ms()/
 *    time_reached_ms(), mismo patron que frogger.c/lunar_lander.c).
 *  - Bucle a ritmo fijo con vTaskDelayUntil() a 60 FPS en vez de
 *    sleep_ms(8). El paso de movimiento ya va por tiempo real
 *    (move_timer_ms), asi que esto solo evita que el bucle corra sin
 *    descanso.
 *  - CONTROL ABSOLUTO: cada direccion del stick (arriba / abajo /
 *    izquierda / derecha) es una direccion de la serpiente. Solo se
 *    permiten giros de 90 grados respecto al ultimo movimiento; pedir
 *    el sentido contrario (180) se ignora -- ver handle_turn_input()
 *    (eje X = controls_get_raw_delta_x, eje Y = controls_get_raw_delta;
 *    derecha y abajo son valores positivos). La deteccion de actividad
 *    en demo mira los dos ejes. El selector 1P/2P del menu usa el eje Y
 *    (arriba = 1P, abajo = 2P), un cambio por empujon -- ver SN_MENU_*.
 *  - Botones: BTN_x_A -> indices 0 (J1_A) y 2 (J2_A), definidos como
 *    BTN_IDX_J1_A/BTN_IDX_J2_A mas abajo (controls.h no define
 *    nombres).
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "snake.h"
#include "../renderer.h"
#include "../controls.h"
#include "../highscores.h"
#include "../sound.h"

// Indices de controls_button_down(), tal cual los documenta controls.h
// de ESP32 (0=J1_A, 1=J1_B, 2=J2_A, 3=J2_B). controls.h no define
// estos nombres.
#define BTN_IDX_J1_A  0
#define BTN_IDX_J2_A  2

// Ritmo objetivo del bucle principal (vTaskDelayUntil en
// game_snake_run). El movimiento usa tiempo real, no depende de esto.
#define TARGET_FPS 60

// Tiempo real en ms desde el arranque (sustituye a absolute_time_t).
static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
// Sustituye a make_timeout_time_ms(ms) del SDK de Pico.
static inline uint32_t make_timeout_ms(uint32_t ms) { return now_ms() + ms; }
// Sustituye a time_reached(t) del SDK de Pico (resta con signo: robusto
// al desbordamiento de 32 bits a los ~49 dias de uptime).
static inline bool time_reached_ms(uint32_t deadline) { return (int32_t)(now_ms() - deadline) >= 0; }

// ---------------------------------------------------------------------------
// Area de juego -- literales fijos, igual que el resto de juegos.
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
// Cuadricula: celda de 8x8. 39 columnas x 8 = 312px exacto. 2 filas de
// HUD (16px) + 27 filas de arena jugable (216px) = 232px, dentro de
// los 234 de PLAY_H (2px de margen sin usar, irrelevante).
// ---------------------------------------------------------------------------
#define CELL         8
#define COLS         39
#define HUD_ROWS     2
#define ARENA_ROWS   27
#define ARENA_X0     PLAY_X
#define ARENA_Y0     (PLAY_Y + HUD_ROWS*CELL)
#define ARENA_W      (COLS * CELL)          // 312
#define ARENA_H      (ARENA_ROWS * CELL)    // 216

static int cell_x(int c) { return ARENA_X0 + c * CELL; }
static int cell_y(int r) { return ARENA_Y0 + r * CELL; }

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int absi(int v) { return v < 0 ? -v : v; }
static int rnd(int n) { return n > 0 ? rand() % n : 0; }

static int centered_x(const char *text, int scale) {
    int w = (int)st7789_text_width(text, (uint8_t)scale);
    int x = (TFT_WIDTH - w) / 2;
    return (x < 0) ? 0 : x;
}

// ---------------------------------------------------------------------------
// Colores -- paleta neon del prototipo (cian/violeta sobre negro),
// sin resplandor (ver cabecera del archivo).
// ---------------------------------------------------------------------------
#define COLOR_P1    COLOR_CYAN
#define COLOR_P2    0xA01F   // violeta neon (aprox. #a600ff en RGB565)
#define COLOR_HEAD  COLOR_WHITE
#define COLOR_WALL  0x2B70   // azul acero, distinto de P1/P2 para no confundir obstaculos con serpientes
#define COLOR_FOOD  COLOR_GREEN
#define COLOR_SFOOD COLOR_RED   // comida especial (+5 de longitud)

// ---------------------------------------------------------------------------
// Tiempo delta real -- identico mecanismo que el resto del proyecto.
// ---------------------------------------------------------------------------
static uint32_t last_tick_time_ms;
static int32_t g_dt_scale = FP;
static int32_t g_elapsed_ms = 16;

static void update_dt_scale(void) {
    uint32_t now = now_ms();
    int32_t elapsed_ms = (int32_t)(now - last_tick_time_ms);
    last_tick_time_ms = now;
    if (elapsed_ms < 1)  elapsed_ms = 1;
    if (elapsed_ms > 50) elapsed_ms = 50;
    g_elapsed_ms = elapsed_ms;
    g_dt_scale = elapsed_ms * FP / 16;
}

// ---------------------------------------------------------------------------
// Serpiente -- buffer circular (ver cabecera del archivo).
// ---------------------------------------------------------------------------
#define MAX_SNAKE_LEN 256

typedef struct {
    int8_t  x[MAX_SNAKE_LEN];
    int8_t  y[MAX_SNAKE_LEN];
    int     head_idx;
    int     len;
    int     dx, dy;      // rumbo actual (aplicado este paso)
    int     qdx, qdy;    // rumbo en cola (se copia a dx/dy en el proximo paso)
    bool    alive;
    bool    human;       // false = controlada por la IA
    int32_t score;
    int     lives;       // 0 = eliminada para el resto de la partida (no vuelve a aparecer en begin_round())
    int     pending_growth;   // segmentos que quedan por crecer -- ver comida especial mas abajo
    int     q2dx, q2dy;       // SEGUNDO giro en cola (buffer de 2 giros, solo humanos) -- ver queue_turn()
    bool    q2valid;
} Snake;

static Snake snakes[2];

static int seg_x(const Snake *s, int i) { return s->x[(s->head_idx + i) % MAX_SNAKE_LEN]; }
static int seg_y(const Snake *s, int i) { return s->y[(s->head_idx + i) % MAX_SNAKE_LEN]; }

static void reset_snake(Snake *s, int x, int y, int dx, int dy) {
    s->head_idx = 0; s->len = 3;
    s->x[0] = (int8_t)x;      s->y[0] = (int8_t)y;
    s->x[1] = (int8_t)(x-dx); s->y[1] = (int8_t)(y-dy);
    s->x[2] = (int8_t)(x-2*dx); s->y[2] = (int8_t)(y-2*dy);
    s->dx = dx; s->dy = dy; s->qdx = dx; s->qdy = dy;
    s->alive = true;
    s->pending_growth = 0;
    s->q2valid = false;
}

static void snake_step(Snake *s, int nx, int ny, bool grow) {
    s->head_idx = (s->head_idx - 1 + MAX_SNAKE_LEN) % MAX_SNAKE_LEN;
    s->x[s->head_idx] = (int8_t)nx; s->y[s->head_idx] = (int8_t)ny;
    if (grow) s->len++;
    // si no crece, el segmento de cola anterior queda simplemente
    // fuera de [0,len) -- no hace falta tocarlo en el array, solo en
    // pantalla (ver do_move_step: erase_cell de la cola vieja).
}

// ---------------------------------------------------------------------------
// Arena -- paredes fijas por ronda (borde + obstaculos), comida.
// ---------------------------------------------------------------------------
static bool wall[ARENA_ROWS][COLS];

typedef struct { int x, y; bool active; } Food;
static Food food;

// Comida especial: roja, +5 de longitud (repartidos en los 5
// siguientes movimientos reales -- ver Snake.pending_growth, para
// que cada segmento nuevo sea siempre una celda por la que la
// serpiente ha pasado de verdad, nunca una posicion inventada de
// golpe). Aparece con cierta probabilidad al comer la comida normal,
// y desaparece sola si no se recoge a tiempo.
#define SPECIAL_FOOD_GROWTH    5
#define SPECIAL_FOOD_SCORE_MUL 500   // puntos = SPECIAL_FOOD_SCORE_MUL * nivel
#define SPECIAL_FOOD_CHANCE    30    // % de probabilidad de aparecer al comer la comida normal
#define SPECIAL_FOOD_LIFE_MS   6000  // tiempo antes de desaparecer si no se recoge

typedef struct { int x, y; bool active; int32_t life_ms; } SpecialFood;
static SpecialFood sfood;

// ---------------------------------------------------------------------------
// Zona de aparicion segura: ninguna pared en la "autopista" de cada
// serpiente al empezar la ronda (3 celdas por detras = el cuerpo, 6 por
// delante y 1 a cada lado). Antes solo se protegia el centro y podia salir
// una pared justo delante de la cabeza, sin tiempo de reaccion.
// ---------------------------------------------------------------------------
#define SPAWN_SAFE_BEHIND 3
#define SPAWN_SAFE_AHEAD  6
#define SPAWN_SAFE_SIDE   1

static bool near_spawn(int x, int y) {
    for (int s = 0; s < 2; s++) {
        const Snake *sn = &snakes[s];
        if (!sn->alive) continue;
        int fx = x - seg_x(sn, 0), fy = y - seg_y(sn, 0);
        int along  = fx * sn->dx + fy * sn->dy;
        int across = absi(fx * sn->dy - fy * sn->dx);
        if (along >= -SPAWN_SAFE_BEHIND && along <= SPAWN_SAFE_AHEAD && across <= SPAWN_SAFE_SIDE) return true;
    }
    return false;
}

static void put_wall(int x, int y) {
    if (x < 1 || x > COLS-2 || y < 1 || y > ARENA_ROWS-2) return;
    if (near_spawn(x, y)) return;
    wall[y][x] = true;
}

static void wall_rect(int x0, int y0, int x1, int y1) {
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            put_wall(x, y);
}

// ---------------------------------------------------------------------------
// Diseños de tablero: en vez de celdas sueltas al azar, cada nivel usa una
// estructura (y encima se anaden unos pocos obstaculos aleatorios que crecen
// con el nivel). Se alternan en ciclo de 6; el 0 es el tablero abierto.
// Todas dejan huecos para que se pueda rodear, y fill_pockets() garantiza
// que cualquier zona que hubiera quedado aislada se rellena de pared.
// ---------------------------------------------------------------------------
#define N_LAYOUTS 6

static void layout_pattern(int id) {
    switch (id) {
    case 1:   // PILARES: cuatro bloques de 2x2 y uno central de 3x3
        wall_rect(11, 6, 12, 7);   wall_rect(26, 6, 27, 7);
        wall_rect(11, 19, 12, 20); wall_rect(26, 19, 27, 20);
        wall_rect(18, 12, 20, 14);
        break;
    case 2:   // CRUZ: barras vertical y horizontal con hueco central
        wall_rect(19, 3, 19, 9);  wall_rect(19, 17, 19, 23);
        wall_rect(3, 13, 13, 13); wall_rect(25, 13, 35, 13);
        break;
    case 3:   // CAJAS: dos habitaciones con una sola puerta
        wall_rect(10, 6, 16, 6);  wall_rect(10, 10, 16, 10);
        wall_rect(10, 6, 10, 10); wall_rect(16, 6, 16, 10);
        wall[10][13] = false;
        wall_rect(22, 16, 28, 16); wall_rect(22, 20, 28, 20);
        wall_rect(22, 16, 22, 20); wall_rect(28, 16, 28, 20);
        wall[16][25] = false;
        break;
    case 4:   // SLALOM: tres barras que obligan a zigzaguear
        wall_rect(3, 7, 30, 7);
        wall_rect(8, 13, 35, 13);
        wall_rect(3, 19, 30, 19);
        break;
    case 5:   // ESQUINAS: una L en cada esquina y una cruz pequena en el centro
        for (int i = 0; i < 4; i++) {
            int x0 = (i & 1) ? COLS-4 : 3,       y0 = (i & 2) ? ARENA_ROWS-4 : 3;
            int sx = (i & 1) ? -1 : 1,           sy = (i & 2) ? -1 : 1;
            for (int k = 0; k <= 6; k++) { put_wall(x0 + sx*k, y0); put_wall(x0, y0 + sy*k); }
        }
        for (int k = -2; k <= 2; k++) { put_wall(19 + k, 13); put_wall(19, 13 + k); }
        break;
    default:  // 0 = abierto
        break;
    }
}

// Rellena de pared toda zona libre que no este conectada con la principal
// (la mayor, o las que contengan la cabeza de una serpiente viva): asi nunca
// puede aparecer comida en un hueco inalcanzable.
static void fill_pockets(void) {
    static uint8_t  comp[ARENA_ROWS][COLS];
    static uint16_t queue[ARENA_ROWS * COLS];
    uint16_t size[256]; bool keep[256];
    memset(comp, 0, sizeof(comp));
    memset(size, 0, sizeof(size)); memset(keep, 0, sizeof(keep));
    int ncomp = 0, largest = 0;
    for (int y = 1; y < ARENA_ROWS-1; y++)
        for (int x = 1; x < COLS-1; x++) {
            if (wall[y][x] || comp[y][x] || ncomp >= 255) continue;
            int id = ++ncomp, qh = 0, qt = 0;
            comp[y][x] = (uint8_t)id; queue[qt++] = (uint16_t)(y * COLS + x);
            while (qh < qt) {
                int cx = queue[qh] % COLS, cy = queue[qh] / COLS; qh++;
                size[id]++;
                static const int8_t D[4][2] = { {1,0}, {-1,0}, {0,1}, {0,-1} };
                for (int d = 0; d < 4; d++) {
                    int nx = cx + D[d][0], ny = cy + D[d][1];
                    if (nx < 1 || nx > COLS-2 || ny < 1 || ny > ARENA_ROWS-2) continue;
                    if (wall[ny][nx] || comp[ny][nx]) continue;
                    comp[ny][nx] = (uint8_t)id; queue[qt++] = (uint16_t)(ny * COLS + nx);
                }
            }
            if (largest == 0 || size[id] > size[largest]) largest = id;
        }
    if (largest) keep[largest] = true;
    for (int s = 0; s < 2; s++) {
        if (!snakes[s].alive) continue;
        int hx = seg_x(&snakes[s], 0), hy = seg_y(&snakes[s], 0);
        if (comp[hy][hx]) keep[comp[hy][hx]] = true;
    }
    for (int y = 1; y < ARENA_ROWS-1; y++)
        for (int x = 1; x < COLS-1; x++)
            if (!wall[y][x] && !keep[comp[y][x]]) wall[y][x] = true;   // comp==0 (sin etiquetar) tambien se rellena
}

static void make_arena(int level) {
    memset(wall, 0, sizeof(wall));
    for (int c = 0; c < COLS; c++) { wall[0][c] = true; wall[ARENA_ROWS-1][c] = true; }
    for (int r = 0; r < ARENA_ROWS; r++) { wall[r][0] = true; wall[r][COLS-1] = true; }
    layout_pattern((level - 1) % N_LAYOUTS);
    int extra = clampi(6 + (level - 1) * 2, 6, 26);   // obstaculos sueltos encima del diseño
    for (int i = 0; i < extra; i++) {
        int x = 3 + rnd(COLS-6), y = 3 + rnd(ARENA_ROWS-6);
        if (absi(x-19) < 4 && absi(y-13) < 3) continue;   // centro despejado
        put_wall(x, y);
    }
    fill_pockets();
}

static bool cell_occupied_by_snake(int x, int y) {
    for (int s = 0; s < 2; s++) {
        Snake *sn = &snakes[s];
        if (!sn->alive) continue;
        for (int i = 0; i < sn->len; i++)
            if (seg_x(sn, i) == x && seg_y(sn, i) == y) return true;
    }
    return false;
}

static void spawn_food(void) {
    for (int tries = 0; tries < 500; tries++) {
        int x = 1 + rnd(COLS-2), y = 1 + rnd(ARENA_ROWS-2);
        if (wall[y][x]) continue;
        if (cell_occupied_by_snake(x, y)) continue;
        if (sfood.active && sfood.x == x && sfood.y == y) continue;
        food.x = x; food.y = y; food.active = true;
        return;
    }
    food.active = false;   // no se encontro hueco libre (muy improbable)
}

static void spawn_special_food(void) {
    for (int tries = 0; tries < 500; tries++) {
        int x = 1 + rnd(COLS-2), y = 1 + rnd(ARENA_ROWS-2);
        if (wall[y][x]) continue;
        if (cell_occupied_by_snake(x, y)) continue;
        if (food.active && food.x == x && food.y == y) continue;
        sfood.x = x; sfood.y = y; sfood.active = true; sfood.life_ms = SPECIAL_FOOD_LIFE_MS;
        return;
    }
    sfood.active = false;   // no se encontro hueco libre (muy improbable)
}

// ¿celda bloqueada para la serpiente self_idx? Pared o cuerpo propio
// siempre; cuerpo rival solo si include_other (ver cabecera: la
// validacion de movimiento real NO mira al rival -- eso lo resuelve
// el cruce de colisiones aparte, igual que el prototipo -- pero la
// IA si lo usa para evitarlo de forma proactiva).
static bool cell_blocked(int x, int y, int self_idx, bool include_other) {
    if (x < 0 || x >= COLS || y < 0 || y >= ARENA_ROWS) return true;
    if (wall[y][x]) return true;
    Snake *sn = &snakes[self_idx];
    if (sn->alive)
        for (int i = 1; i < sn->len; i++)   // i=1: la cabeza actual se va a mover, no cuenta
            if (seg_x(sn, i) == x && seg_y(sn, i) == y) return true;
    if (include_other) {
        Snake *ot = &snakes[1 - self_idx];
        if (ot->alive)
            for (int i = 0; i < ot->len; i++)
                if (seg_x(ot, i) == x && seg_y(ot, i) == y) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Particulas -- explosion simple (sin resplandor), borradas contra el
// fondo real de cada pixel (pared o negro) para no dejar marcas.
// ---------------------------------------------------------------------------
#define MAX_PARTICLES 48
typedef struct { int32_t x, y, vx, vy; int life; bool active; uint16_t color; } Particle;
static Particle particles[MAX_PARTICLES];

static uint16_t bg_at_px(int px, int py) {
    if (px < ARENA_X0 || px >= ARENA_X0+ARENA_W || py < ARENA_Y0 || py >= ARENA_Y0+ARENA_H)
        return COLOR_BLACK;
    int c = (px-ARENA_X0)/CELL, r = (py-ARENA_Y0)/CELL;
    if (r >= 0 && r < ARENA_ROWS && c >= 0 && c < COLS && wall[r][c]) return COLOR_WALL;
    return COLOR_BLACK;
}

static void spawn_burst(int cx, int cy, int count, const uint16_t *colors, int ncolors) {
    for (int n = 0; n < count; n++) {
        int slot = -1;
        for (int i = 0; i < MAX_PARTICLES; i++) if (!particles[i].active) { slot = i; break; }
        if (slot < 0) break;
        Particle *p = &particles[slot];
        p->x = PX2FP(cx); p->y = PX2FP(cy);
        p->vx = (rnd(21)-10) * FP / 5;
        p->vy = (rnd(21)-10) * FP / 5;
        p->life = 14 + rnd(14);
        p->color = colors[rnd(ncolors)];
        p->active = true;
    }
}

static void update_and_draw_particles(void) {
    bool any = false;
    for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle *p = &particles[i];
        if (!p->active) continue;
        int ox = FP2PX(p->x), oy = FP2PX(p->y);
        p->x += p->vx * g_dt_scale / FP;
        p->y += p->vy * g_dt_scale / FP;
        p->vx = p->vx * 240 / 256;   // friccion -- se frenan con el tiempo
        p->vy = p->vy * 240 / 256;
        if (--p->life <= 0) p->active = false;
        renderer_fill_rect(ox, oy, 2, 2, bg_at_px(ox, oy));
        if (p->active) {
            int nx = FP2PX(p->x), ny = FP2PX(p->y);
            renderer_fill_rect(nx, ny, 2, 2, p->color);
        }
        any = true;
    }
    if (any) renderer_flush();
}



// ---------------------------------------------------------------------------
// Estado general de la partida
// ---------------------------------------------------------------------------
typedef enum { SN_READY, SN_ROUND_START, SN_PLAYING, SN_ROUND_OVER, SN_GAME_OVER, SN_SCORES } SnState;

static SnState state;
static int     level = 1;
static int     n_players = 1;   // cuantos son humanos (1 o 2); siempre hay 2 serpientes en el tablero
static bool    demo = false;
static bool    g_done = false;
static int     blink = 0;
static int     winner = 0;      // 0 = doble choque, 1/2 = gana ese jugador

// ---------------------------------------------------------------------------
// Reglas de progresion (ver cabecera del archivo)
// ---------------------------------------------------------------------------
#define FOODS_PER_LEVEL        10     // frutas del humano para subir de nivel
#define SPECIAL_FOOD_PROGRESS  3      // la fruta roja cuenta como 3 para el nivel
#define ROUND_WIN_BONUS_MUL    300    // 2 jugadores: puntos = 300 * nivel al ganar una ronda
#define EXTRA_LIFE_EVERY       10000  // vida extra cada tantos puntos de un humano
#define MAX_LIVES              5
#define CARRY_MAX_GROWTH       40     // al subir de nivel se conserva hasta esta longitud extra
#define AI_RESPAWN_MS          3000   // contra la IA: tiempo que tarda en reaparecer

static int      level_foods = 0;        // frutas comidas en el nivel actual
static int32_t  next_life_at[2];
static bool     boost_now[2];           // el humano mantiene A: sus frutas valen el doble
static bool     ai_waiting = false;     // la IA esta muerta, esperando reaparecer (solo 1 jugador)
static uint32_t ai_respawn_at;
static int      match_winner = 0;       // 2 jugadores: 1/2 = ganador de la partida, 0 = empate

// Contra la IA (1 jugador, no demo): la serpiente 1 es la IA. Su muerte NO
// termina la ronda ni le quita vidas: reaparece sola. Solo termina la ronda
// si muere el humano.
static bool solo_vs_ai(void) { return !demo && n_players == 1; }

// Entrada con buffer de 2 giros (ver queue_turn): ultimo candidato visto y
// cuantos ticks seguidos lleva.
#define SN_TURN_DEBOUNCE_TICKS 2
static int8_t  in_cand_dx[2], in_cand_dy[2];
static uint8_t in_cand_ticks[2];
static bool    turn_locked[2];   // true = ya se aplico un giro este intervalo de movimiento (ver ai_turn/handle_turn_input)
// Selector 1P/2P de la pantalla de inicio: EJE Y del stick, un cambio por
// "empujon". El joystick analogico devuelve un valor distinto de cero
// MIENTRAS esta inclinado (no es un evento puntual como en un encoder), asi
// que el cambio solo se "arma" cuando el stick vuelve al centro; ademas hay
// un tiempo minimo entre cambios por si rebota al soltarlo.
//   arriba -> "1 JUGADOR (VS IA)" (la opcion de arriba en pantalla)
//   abajo  -> "2 JUGADORES"       (la opcion de abajo)
// Si en tu stick resulta al reves, pon SN_MENU_DOWN_IS_POSITIVE a 0.
#define SN_MENU_DOWN_IS_POSITIVE 1
#define SN_MENU_REARM_MS         120   // stick en el centro este tiempo -> se rearma
#define SN_MENU_MIN_GAP_MS       300   // minimo entre dos cambios
static bool     menu_armed        = false;
static bool     menu_centered     = false;
static uint32_t menu_center_since = 0;
static uint32_t menu_gap_until    = 0;
static int32_t move_timer_ms;
static uint32_t pause_until;
static uint32_t game_over_deadline;
static uint32_t ready_input_ok_time;
static int     demo_ticks;

static int32_t move_interval_ms(int lv) {
    int32_t v = 180 - (lv-1) * 10;
    return clampi(v, 90, 180);
}

// ---------------------------------------------------------------------------
// Render de celdas -- helpers minimos, sin seguimiento de "anterior"
// (ver cabecera del archivo: sabemos siempre exactamente que celda
// cambia en cada paso, no hace falta comparar con nada).
// ---------------------------------------------------------------------------
static void draw_cell(int x, int y, uint16_t color) {
    renderer_fill_rect(cell_x(x), cell_y(y), CELL-1, CELL-1, color);
}
static void erase_cell(int x, int y) {
    renderer_fill_rect(cell_x(x), cell_y(y), CELL-1, CELL-1, COLOR_BLACK);
}

// Cuenta atras de la comida especial -- se llama cada tick (no solo
// en cada paso de movimiento) para que el tiempo de vida corra en
// tiempo real, igual que el resto de temporizadores del proyecto.
static void update_special_food(void) {
    if (!sfood.active) return;
    sfood.life_ms -= g_elapsed_ms;
    if (sfood.life_ms <= 0) {
        erase_cell(sfood.x, sfood.y);
        sfood.active = false;
        renderer_flush();
    }
}

static void draw_arena_static(void) {
    renderer_clear(COLOR_BLACK);
    for (int r = 0; r < ARENA_ROWS; r++)
        for (int c = 0; c < COLS; c++)
            if (wall[r][c]) draw_cell(c, r, COLOR_WALL);
    if (food.active) draw_cell(food.x, food.y, COLOR_FOOD);
    if (sfood.active) draw_cell(sfood.x, sfood.y, COLOR_SFOOD);
    for (int s = 0; s < 2; s++) {
        Snake *sn = &snakes[s];
        if (!sn->alive) continue;
        uint16_t body = (s == 0) ? COLOR_P1 : COLOR_P2;
        for (int i = 0; i < sn->len; i++)
            draw_cell(seg_x(sn, i), seg_y(sn, i), i == 0 ? COLOR_HEAD : body);
    }
}

// ---------------------------------------------------------------------------
// Mensajes centrales (2 lineas) -- para "NIVEL N / PREPARADOS",
// "GANA J.1 / SIGUIENTE RONDA", etc. Al limpiarlos hay que restaurar
// lo que hubiera debajo (paredes/comida/serpientes), a diferencia de
// lunar_lander (cielo vacio) -- aqui el recuadro cae en pleno
// tablero.
// ---------------------------------------------------------------------------
#define MSG_BOX_W 260
#define MSG_BOX_H 46
#define MSG_BOX_X (CX - MSG_BOX_W/2)
#define MSG_BOX_Y (ARENA_Y0 + ARENA_H/2 - MSG_BOX_H/2)

static char prev_msg1[32] = "";
static char prev_msg2[32] = "";

static void update_messages(const char *l1, uint16_t c1, int s1, const char *l2, uint16_t c2, int s2) {
    if (strcmp(l1, prev_msg1) == 0 && strcmp(l2, prev_msg2) == 0) return;
    renderer_fill_rect(MSG_BOX_X, MSG_BOX_Y, MSG_BOX_W, MSG_BOX_H, COLOR_BLACK);
    if (l1[0]) renderer_draw_text(centered_x(l1, s1), MSG_BOX_Y+8, l1, c1, COLOR_BLACK, s1);
    if (l2[0]) renderer_draw_text(centered_x(l2, s2), MSG_BOX_Y+8+s1*9+6, l2, c2, COLOR_BLACK, s2);
    strncpy(prev_msg1, l1, 31); prev_msg1[31] = '\0';
    strncpy(prev_msg2, l2, 31); prev_msg2[31] = '\0';
    renderer_flush();
}

static void restore_arena_rect(int rx, int ry, int rw, int rh) {
    renderer_fill_rect(rx, ry, rw, rh, COLOR_BLACK);
    int c0 = clampi((rx-ARENA_X0)/CELL, 0, COLS-1);
    int c1 = clampi((rx+rw-ARENA_X0+CELL-1)/CELL, 0, COLS-1);
    int r0 = clampi((ry-ARENA_Y0)/CELL, 0, ARENA_ROWS-1);
    int r1 = clampi((ry+rh-ARENA_Y0+CELL-1)/CELL, 0, ARENA_ROWS-1);
    for (int r = r0; r <= r1; r++)
        for (int c = c0; c <= c1; c++)
            if (wall[r][c]) draw_cell(c, r, COLOR_WALL);
    if (food.active && food.x >= c0 && food.x <= c1 && food.y >= r0 && food.y <= r1)
        draw_cell(food.x, food.y, COLOR_FOOD);
    if (sfood.active && sfood.x >= c0 && sfood.x <= c1 && sfood.y >= r0 && sfood.y <= r1)
        draw_cell(sfood.x, sfood.y, COLOR_SFOOD);
    for (int s = 0; s < 2; s++) {
        Snake *sn = &snakes[s];
        if (!sn->alive) continue;
        uint16_t body = (s == 0) ? COLOR_P1 : COLOR_P2;
        for (int i = 0; i < sn->len; i++) {
            int sx = seg_x(sn, i), sy = seg_y(sn, i);
            if (sx >= c0 && sx <= c1 && sy >= r0 && sy <= r1)
                draw_cell(sx, sy, i == 0 ? COLOR_HEAD : body);
        }
    }
}

static void clear_messages(void) {
    if (prev_msg1[0] == '\0' && prev_msg2[0] == '\0') return;
    restore_arena_rect(MSG_BOX_X, MSG_BOX_Y, MSG_BOX_W, MSG_BOX_H);
    prev_msg1[0] = '\0'; prev_msg2[0] = '\0';
    renderer_flush();
}

// ---------------------------------------------------------------------------
// HUD -- P1 (izq, cian), nivel (centro), P2 (der, violeta). Ocupa la
// franja dedicada de 16px, redibujo incremental solo si cambia.
// ---------------------------------------------------------------------------
static int32_t prev_hud_p1 = -1, prev_hud_p2 = -1;
static int prev_hud_level = -1, prev_hud_foods = -1;
static int prev_hud_lives1 = -1, prev_hud_lives2 = -1;

static void draw_hud_if_changed(bool force) {
    char buf[40];
    bool changed = false;
    // P1 a la izquierda y P2 a la derecha: 124 px cada uno (caben puntuaciones
    // de hasta 7 cifras), dejando 60 px centrales para nivel + progreso.
    if (snakes[0].score != prev_hud_p1 || snakes[0].lives != prev_hud_lives1 || force) {
        renderer_fill_rect(PLAY_X+1, PLAY_Y+1, 124, 14, COLOR_BLACK);
        snprintf(buf, sizeof(buf), "%05ld x%d", (long)snakes[0].score, snakes[0].lives);
        renderer_draw_text(PLAY_X+3, PLAY_Y+3, buf, COLOR_P1, COLOR_BLACK, 2);
        prev_hud_p1 = snakes[0].score; prev_hud_lives1 = snakes[0].lives; changed = true;
    }
    if (level != prev_hud_level || level_foods != prev_hud_foods || force) {
        renderer_fill_rect(CX-30, PLAY_Y+1, 60, 14, COLOR_BLACK);
        snprintf(buf, sizeof(buf), "NIVEL %d", level);
        renderer_draw_text(centered_x(buf, 1), PLAY_Y+2, buf, COLOR_WHITE, COLOR_BLACK, 1);
        // Barra de progreso: una casilla por fruta que hace falta comer.
        int bx = CX - (FOODS_PER_LEVEL * 6 - 1) / 2;
        for (int i = 0; i < FOODS_PER_LEVEL; i++)
            renderer_fill_rect(bx + i * 6, PLAY_Y + 10, 5, 4, i < level_foods ? COLOR_GREEN : 0x2104);
        prev_hud_level = level; prev_hud_foods = level_foods; changed = true;
    }
    if (snakes[1].score != prev_hud_p2 || snakes[1].lives != prev_hud_lives2 || force) {
        renderer_fill_rect(PLAY_X+PLAY_W-125, PLAY_Y+1, 124, 14, COLOR_BLACK);
        if (solo_vs_ai()) snprintf(buf, sizeof(buf), "%05ld IA", (long)snakes[1].score);   // la IA no gasta vidas
        else              snprintf(buf, sizeof(buf), "%05ld x%d", (long)snakes[1].score, snakes[1].lives);
        int tx = PLAY_X+PLAY_W-3-(int)st7789_text_width(buf, 2);
        renderer_draw_text(tx, PLAY_Y+3, buf, COLOR_P2, COLOR_BLACK, 2);
        prev_hud_p2 = snakes[1].score; prev_hud_lives2 = snakes[1].lives; changed = true;
    }
    if (changed) renderer_flush();
}

// ---------------------------------------------------------------------------
// Ronda -- arranca/reinicia el tablero conservando puntuaciones.
// ---------------------------------------------------------------------------
static void begin_round(bool carry_length) {
    // Al subir de nivel se conserva la longitud (hasta CARRY_MAX_GROWTH): la
    // serpiente reaparece corta y recupera esos segmentos en los primeros
    // pasos (pending_growth), asi cada segmento es siempre una celda por la
    // que ha pasado de verdad. Tras morir, en cambio, se empieza de cero.
    int grow[2] = { 0, 0 };
    if (carry_length)
        for (int i = 0; i < 2; i++)
            if (snakes[i].alive && snakes[i].lives > 0)
                grow[i] = clampi(snakes[i].len - 3 + snakes[i].pending_growth, 0, CARRY_MAX_GROWTH);

    // Solo reaparece quien le queden vidas -- si una serpiente ya
    // esta eliminada, se queda fuera del tablero el resto de la
    // partida (ver crash_snake) y la otra puede seguir jugando sola.
    if (snakes[0].lives > 0) reset_snake(&snakes[0], 7, 8, 0, 1);
    else                     snakes[0].alive = false;
    if (snakes[1].lives > 0) reset_snake(&snakes[1], COLS-8, ARENA_ROWS-9, 0, -1);
    else                     snakes[1].alive = false;
    for (int i = 0; i < 2; i++) if (snakes[i].alive) snakes[i].pending_growth = grow[i];
    ai_waiting = false;
    in_cand_ticks[0] = in_cand_ticks[1] = 0;
    make_arena(level);
    spawn_food();
    sfood.active = false;   // la comida especial no persiste entre rondas
    for (int i = 0; i < MAX_PARTICLES; i++) particles[i].active = false;
    draw_arena_static();
    prev_hud_p1 = prev_hud_p2 = -1; prev_hud_level = -1; prev_hud_lives1 = prev_hud_lives2 = -1; prev_hud_foods = -1;
    draw_hud_if_changed(true);
    char l1[40], l2[40];
    int left = clampi(FOODS_PER_LEVEL - level_foods, 1, FOODS_PER_LEVEL);   // acotado: el compilador sabe que son 1-2 cifras
    snprintf(l1, sizeof(l1), "NIVEL %d", level);
    if (left == 1) snprintf(l2, sizeof(l2), "FALTA 1 FRUTA");
    else           snprintf(l2, sizeof(l2), "FALTAN %d FRUTAS", left);
    prev_msg1[0] = '\0'; prev_msg2[0] = '\0';   // el clear de arriba se llevo cualquier mensaje anterior
    update_messages(l1, COLOR_YELLOW, 2, l2, COLOR_WHITE, 1);
    pause_until = make_timeout_ms(1400);
}

static void game_reset_full(void) {
    level = 1;
    level_foods = 0;
    next_life_at[0] = next_life_at[1] = EXTRA_LIFE_EVERY;
    boost_now[0] = boost_now[1] = false;
    ai_waiting = false;
    match_winner = 0;
    snakes[0].score = 0; snakes[1].score = 0;
    snakes[0].human = !demo;
    snakes[1].human = !demo && (n_players == 2);
    // En demo se dan vidas de sobra: el propio demo ya se corta solo
    // por tiempo o por cualquier pulsacion (ver sn_tick), así que no
    // hace falta que el sistema de vidas también lo termine.
    snakes[0].lives = demo ? 99 : 3;
    snakes[1].lives = demo ? 99 : 3;
    winner = 0;
    turn_locked[0] = turn_locked[1] = false;
}

// ---------------------------------------------------------------------------
// IA -- heuristica sencilla: entre las 3 direcciones validas (recto,
// giro horario, giro antihorario) descarta las que chocan de forma
// inmediata (pared, propio cuerpo o cuerpo rival) y elige la que mas
// acerca a la comida, con un poco de aleatoriedad para que no sea
// perfectamente robotica.
//
// turn_locked limita a UN giro por intervalo de movimiento para la IA:
// sin este limite, dos giros de 90 grados antes del siguiente paso
// sumarian un giro de 180 -- la serpiente se meteria de cabeza contra
// su propio cuello. Se desbloquea en do_move_step() al confirmarse cada
// paso. (El jugador humano no lo necesita: handle_turn_input() valida
// cada direccion contra el ultimo rumbo realmente movido.)
// ---------------------------------------------------------------------------
static void ai_turn(int idx) {
    Snake *s = &snakes[idx];
    if (!s->alive || turn_locked[idx]) return;
    int hx = seg_x(s, 0), hy = seg_y(s, 0);
    int cdx = s->qdx, cdy = s->qdy;
    int cand_dx[3] = { cdx, -cdy, cdy };
    int cand_dy[3] = { cdy,  cdx, -cdx };
    int best = -1, best_score = -1000000;
    for (int i = 0; i < 3; i++) {
        int nx = hx + cand_dx[i], ny = hy + cand_dy[i];
        if (cell_blocked(nx, ny, idx, true)) continue;
        int dist = absi(nx - food.x) + absi(ny - food.y);
        int score = -dist + rnd(3);
        if (score > best_score) { best_score = score; best = i; }
    }
    if (best < 0) return;   // sin salida segura -- sigue recto, choque inevitable
    s->qdx = cand_dx[best]; s->qdy = cand_dy[best];
    if (best != 0) turn_locked[idx] = true;   // best==0 es "seguir recto", no consume el giro disponible
}

// ---------------------------------------------------------------------------
// CONTROL ABSOLUTO: cada direccion del stick (arriba / abajo / izquierda /
// derecha) es una direccion de la serpiente, en vez de girar a izquierda o
// derecha respecto a su rumbo.
//
// Solo se admiten giros de 90 grados: la direccion pedida se compara con
// el ULTIMO RUMBO REALMENTE MOVIDO (sn->dx, sn->dy):
//   - perpendicular (producto escalar 0)  -> giro de 90, se pone en cola
//   - la misma    (producto escalar +1)   -> se pone en cola igualmente, lo
//     que permite "cancelar" un giro pedido por error antes del siguiente paso
//   - la contraria (producto escalar -1)  -> 180 grados: se IGNORA
// Como se valida contra el rumbo ya aplicado y no contra el que esta en
// cola, da igual cuantas veces cambie de idea el jugador dentro de un mismo
// intervalo de movimiento: el rumbo que se aplique en el siguiente paso es
// siempre el mismo o perpendicular al ultimo movimiento, asi que es
// imposible invertir el sentido sobre el propio cuerpo (por eso aqui ya no
// hace falta turn_locked, que solo usa la IA).
//
// Ejes (coordenadas de pantalla): X derecha > 0 / izquierda < 0;
// Y abajo > 0 / arriba < 0. Si el stick esta inclinado en diagonal manda el
// eje con mas inclinacion; si hay empate exacto, no se hace nada.
// SN_STICK_MIN: inclinacion minima para contar como direccion (si el stick
// en reposo diera un valor pequeno y la serpiente girase sola, subelo a 2).
// ---------------------------------------------------------------------------
#define SN_STICK_MIN 1
// Si en tu stick una direccion sale invertida, pon la constante del eje
// correspondiente a 0 (no hace falta tocar nada mas).
#define SN_STICK_X_RIGHT_IS_POSITIVE 1
#define SN_STICK_Y_DOWN_IS_POSITIVE  1

// Pone en cola la direccion (ddx,ddy) pedida por el jugador, con BUFFER DE 2
// GIROS. Sea H el ultimo rumbo realmente movido y P1/P2 los giros ya en cola:
//   - sin giros en cola:  perpendicular a H -> P1; la misma -> nada; la
//     contraria (180) -> se ignora.
//   - con P1 en cola:     la misma que P1 -> nada; H -> cancela el giro;
//     la opuesta a P1 -> cambia de idea (sustituye P1); la contraria a H
//     (que es perpendicular a P1) -> se encadena como P2 (giro en U en dos pasos).
//   - con P1 y P2:        la opuesta a P2 la sustituye; el resto, cola llena.
// Cada cambio de rumbo que llega a aplicarse es siempre de 90 grados.
static void queue_turn(Snake *s, int ddx, int ddy) {
    bool has1 = (s->qdx != s->dx || s->qdy != s->dy);
    if (!has1) {
        if (ddx == s->dx && ddy == s->dy) return;
        if (ddx * s->dx + ddy * s->dy != 0) return;        // 180 grados: se ignora
        s->qdx = ddx; s->qdy = ddy;
        sound_effect_select();
        return;
    }
    if (!s->q2valid) {
        if (ddx == s->qdx && ddy == s->qdy) return;
        if (ddx == s->dx && ddy == s->dy) { s->qdx = s->dx; s->qdy = s->dy; return; }   // cancela el giro
        if (ddx == -s->qdx && ddy == -s->qdy) { s->qdx = ddx; s->qdy = ddy; sound_effect_select(); return; }
        s->q2dx = ddx; s->q2dy = ddy; s->q2valid = true;   // (ddx,ddy) == -H, perpendicular a P1
        sound_effect_select();
        return;
    }
    if (ddx == s->q2dx && ddy == s->q2dy) return;
    if (ddx == -s->q2dx && ddy == -s->q2dy) { s->q2dx = ddx; s->q2dy = ddy; sound_effect_select(); }
}

static void handle_turn_input(int player) {
    Snake *s = &snakes[player];
    if (!s->alive || !s->human) return;
    // Se leen SIEMPRE los dos ejes (aunque luego no se use el valor), para no
    // acumular un remanente que dispare un giro de mas mas tarde.
    int ax = controls_get_raw_delta_x(player);
    int ay = controls_get_raw_delta(player);
    if (!SN_STICK_X_RIGHT_IS_POSITIVE) ax = -ax;
    if (!SN_STICK_Y_DOWN_IS_POSITIVE)  ay = -ay;
    int mx = ax < 0 ? -ax : ax;
    int my = ay < 0 ? -ay : ay;

    int ddx = 0, ddy = 0;
    if      (mx >= SN_STICK_MIN && mx > my) ddx = (ax > 0) ? 1 : -1;   // domina el eje horizontal
    else if (my >= SN_STICK_MIN && my > mx) ddy = (ay > 0) ? 1 : -1;   // domina el vertical
    else { in_cand_ticks[player] = 0; return; }                         // stick en el centro, o diagonal exacta

    // Filtro: la direccion debe mantenerse SN_TURN_DEBOUNCE_TICKS ticks
    // seguidos; asi un barrido rapido del stick por direcciones intermedias
    // no encola giros que el jugador no queria.
    if (ddx == in_cand_dx[player] && ddy == in_cand_dy[player]) {
        if (in_cand_ticks[player] < 255) in_cand_ticks[player]++;
    } else {
        in_cand_dx[player] = (int8_t)ddx; in_cand_dy[player] = (int8_t)ddy; in_cand_ticks[player] = 1;
    }
    if (in_cand_ticks[player] < SN_TURN_DEBOUNCE_TICKS) return;
    queue_turn(s, ddx, ddy);
}

static void crash_snake(int idx, int hx, int hy) {
    Snake *sn = &snakes[idx];
    if (!sn->alive) return;   // evita partirculas dobles si ya se marco por otra via este mismo paso
    sn->alive = false;
    if (sn->lives > 0 && !(idx == 1 && solo_vs_ai())) sn->lives--;   // la IA contra un humano no gasta vidas
    uint16_t cols[3] = { idx == 0 ? COLOR_P1 : COLOR_P2, COLOR_WHITE, COLOR_YELLOW };
    spawn_burst(cell_x(hx)+CELL/2, cell_y(hy)+CELL/2, 16, cols, 3);
}

// Vida extra cada EXTRA_LIFE_EVERY puntos de un jugador humano (maximo
// MAX_LIVES). El aviso es el sonido y el contador de vidas del HUD.
static void check_extra_life(int s) {
    Snake *sn = &snakes[s];
    if (!sn->human || demo) return;
    while (sn->score >= next_life_at[s]) {
        next_life_at[s] += EXTRA_LIFE_EVERY;
        if (sn->lives < MAX_LIVES) { sn->lives++; sound_effect_extra_life(); }
    }
}

// Contra la IA: la IA acaba de morir y la ronda sigue. Se borra su cuerpo del
// tablero y se programa su reaparicion.
static void ai_down(void) {
    Snake *ai = &snakes[1];
    for (int i = 0; i < ai->len; i++) erase_cell(seg_x(ai, i), seg_y(ai, i));
    ai_waiting = true;
    ai_respawn_at = make_timeout_ms(AI_RESPAWN_MS);
}

// ¿Esta libre la "autopista" de una posible aparicion (2 celdas por detras
// y SPAWN_SAFE_AHEAD por delante)? Sin paredes, serpientes ni fruta encima.
static bool spawn_corridor_free(int x, int y, int dx, int dy) {
    for (int k = -2; k <= SPAWN_SAFE_AHEAD; k++) {
        int cx = x + dx * k, cy = y + dy * k;
        if (cx < 1 || cx > COLS-2 || cy < 1 || cy > ARENA_ROWS-2) return false;
        if (wall[cy][cx]) return false;
        if (cell_occupied_by_snake(cx, cy)) return false;
        if (food.active && food.x == cx && food.y == cy) return false;
        if (sfood.active && sfood.x == cx && sfood.y == cy) return false;
    }
    return true;
}

// La IA reaparece en la esquina libre mas alejada del humano; si ninguna
// esta libre todavia, lo vuelve a intentar medio segundo despues.
static void try_respawn_ai(void) {
    static const int RS[4][4] = {
        { COLS-8, ARENA_ROWS-9, 0, -1 }, { 7, ARENA_ROWS-9, 0, -1 },
        { COLS-8, 8, 0, 1 },             { 7, 8, 0, 1 },
    };
    int hx = seg_x(&snakes[0], 0), hy = seg_y(&snakes[0], 0);
    int best = -1, best_dist = -1;
    for (int i = 0; i < 4; i++) {
        int dist = absi(hx - RS[i][0]) + absi(hy - RS[i][1]);
        if (dist < 12) continue;                                   // demasiado cerca del humano
        if (!spawn_corridor_free(RS[i][0], RS[i][1], RS[i][2], RS[i][3])) continue;
        if (dist > best_dist) { best_dist = dist; best = i; }
    }
    if (best < 0) { ai_respawn_at = make_timeout_ms(500); return; }
    Snake *ai = &snakes[1];
    reset_snake(ai, RS[best][0], RS[best][1], RS[best][2], RS[best][3]);
    turn_locked[1] = false;
    ai_waiting = false;
    for (int i = 0; i < ai->len; i++)
        draw_cell(seg_x(ai, i), seg_y(ai, i), i == 0 ? COLOR_HEAD : COLOR_P2);
    renderer_flush();
}

// ---------------------------------------------------------------------------
// Paso de movimiento -- ver cabecera del archivo: unico momento en
// que el tablero cambia. Primero cada serpiente intenta moverse por
// su cuenta (pared/cuerpo propio, sin mirar al rival, igual que el
// prototipo); despues se cruzan las colisiones entre ambas (choque
// de frente, o cabeza contra cuerpo rival) SOLO si ninguna murio ya
// en su propio intento -- misma logica que checkPlayerCollision() en
// el prototipo, que se salta por completo si alguna ya esta muerta.
// ---------------------------------------------------------------------------
static void do_move_step(void) {
    bool ai_was_alive = snakes[1].alive;
    for (int s = 0; s < 2; s++) {
        Snake *sn = &snakes[s];
        if (!sn->alive) continue;
        sn->dx = sn->qdx; sn->dy = sn->qdy;
        if (sn->q2valid) {                       // el 2o giro del buffer pasa a ser el siguiente
            sn->qdx = sn->q2dx; sn->qdy = sn->q2dy; sn->q2valid = false;
        }
        turn_locked[s] = false;   // el giro de este intervalo ya se ha consumido -- desbloquea para el siguiente
        int hx = seg_x(sn, 0), hy = seg_y(sn, 0);
        int nx = hx + sn->dx, ny = hy + sn->dy;

        if (cell_blocked(nx, ny, s, false)) {
            crash_snake(s, hx, hy);
            continue;
        }

        int mult = boost_now[s] ? 2 : 1;           // con el impulso pulsado, las frutas valen el doble
        bool counts = demo || sn->human;           // la fruta de la IA no hace subir de nivel
        if (food.active && nx == food.x && ny == food.y) {
            sn->pending_growth += 1;
            sn->score += 100 * level * mult;
            if (counts) level_foods += 1;
            sound_effect_coin();
            uint16_t fcols[2] = { COLOR_FOOD, COLOR_WHITE };
            spawn_burst(cell_x(nx)+CELL/2, cell_y(ny)+CELL/2, 8, fcols, 2);
            spawn_food();
            if (food.active) draw_cell(food.x, food.y, COLOR_FOOD);
            // Ocasionalmente aparece tambien la comida especial (si
            // no hay ya una activa), ver cabecera del archivo.
            if (!sfood.active && rnd(100) < SPECIAL_FOOD_CHANCE) {
                spawn_special_food();
                if (sfood.active) draw_cell(sfood.x, sfood.y, COLOR_SFOOD);
            }
            check_extra_life(s);
        }
        if (sfood.active && nx == sfood.x && ny == sfood.y) {
            sn->pending_growth += SPECIAL_FOOD_GROWTH;
            sn->score += SPECIAL_FOOD_SCORE_MUL * level * mult;
            if (counts) level_foods += SPECIAL_FOOD_PROGRESS;
            sound_effect_coin();
            uint16_t scols[3] = { COLOR_SFOOD, COLOR_WHITE, COLOR_YELLOW };
            spawn_burst(cell_x(nx)+CELL/2, cell_y(ny)+CELL/2, 14, scols, 3);
            sfood.active = false;
            check_extra_life(s);
        }

        bool grow = false;
        if (sn->pending_growth > 0) { grow = true; sn->pending_growth--; }

        int old_tail_x = -1, old_tail_y = -1;
        if (!grow) { old_tail_x = seg_x(sn, sn->len-1); old_tail_y = seg_y(sn, sn->len-1); }

        draw_cell(hx, hy, s == 0 ? COLOR_P1 : COLOR_P2);   // la cabeza vieja pasa a ser cuerpo
        snake_step(sn, nx, ny, grow);
        draw_cell(nx, ny, COLOR_HEAD);
        if (!grow) erase_cell(old_tail_x, old_tail_y);
    }

    if (snakes[0].alive && snakes[1].alive) {
        int h0x = seg_x(&snakes[0], 0), h0y = seg_y(&snakes[0], 0);
        int h1x = seg_x(&snakes[1], 0), h1y = seg_y(&snakes[1], 0);
        if (h0x == h1x && h0y == h1y) {
            crash_snake(0, h0x, h0y);
            crash_snake(1, h1x, h1y);
        } else {
            bool hit01 = false, hit10 = false;
            for (int i = 1; i < snakes[1].len && !hit01; i++)
                if (seg_x(&snakes[1], i) == h0x && seg_y(&snakes[1], i) == h0y) hit01 = true;
            for (int i = 1; i < snakes[0].len && !hit10; i++)
                if (seg_x(&snakes[0], i) == h1x && seg_y(&snakes[0], i) == h1y) hit10 = true;
            if (hit01) crash_snake(0, h0x, h0y);
            if (hit10) crash_snake(1, h1x, h1y);
        }
    }

    renderer_flush();

    bool dead0 = !snakes[0].alive, dead1 = !snakes[1].alive;
    bool solo = solo_vs_ai();
    bool both_now = ai_was_alive && dead0 && dead1;

    // Contra la IA: si solo ha muerto ella, la ronda continua.
    if (solo && ai_was_alive && dead1 && !dead0) {
        ai_down();
        sound_effect_explosion();
    }

    bool round_over = solo ? dead0 : (dead0 || dead1);
    if (round_over) {
        sound_effect_explosion();
        char l1[40], l2[48];
        uint16_t c1 = COLOR_RED;
        snprintf(l2, sizeof(l2), "SIGUIENTE RONDA...");
        if (solo) {
            winner = snakes[1].alive ? 2 : 0;
            if (winner == 2)     { snprintf(l1, sizeof(l1), "GANA LA IA"); c1 = COLOR_P2; }
            else if (both_now)   snprintf(l1, sizeof(l1), "DOBLE CHOQUE");
            else                 snprintf(l1, sizeof(l1), "HAS CHOCADO");
        } else {
            winner = (dead0 && dead1) ? 0 : (dead0 ? 2 : 1);
            if (winner == 0) snprintf(l1, sizeof(l1), "DOBLE CHOQUE");
            else {
                snprintf(l1, sizeof(l1), "GANA JUGADOR %d", winner);
                c1 = (winner == 1) ? COLOR_P1 : COLOR_P2;
                if (!demo) {   // 2 jugadores: ganar la ronda da puntos
                    int32_t bonus = (int32_t)ROUND_WIN_BONUS_MUL * level;
                    snakes[winner-1].score += bonus;
                    check_extra_life(winner - 1);
                    draw_hud_if_changed(false);
                    snprintf(l2, sizeof(l2), "+%ld PUNTOS - SIGUIENTE RONDA", (long)bonus);
                }
            }
        }
        update_messages(l1, c1, 2, l2, COLOR_WHITE, 1);
        pause_until = make_timeout_ms(1600);
        state = SN_ROUND_OVER;
        return;
    }

    if (level_foods >= FOODS_PER_LEVEL) {
        level++;
        level_foods -= FOODS_PER_LEVEL;
        sound_effect_teleport();
        begin_round(true);        // conserva la longitud de las serpientes vivas
        state = SN_ROUND_START;
    }
}

// ---------------------------------------------------------------------------
// Pantallas "estaticas"
// ---------------------------------------------------------------------------
static void draw_ready_screen(void) {
    renderer_clear(COLOR_BLACK);
    renderer_draw_text(centered_x("SNAKE", 4), 10, "SNAKE", COLOR_CYAN, COLOR_BLACK, 4);

    const char *l1 = "STICK: ELIGE DIRECCION";
    const char *l2 = "A: IMPULSO = x2 PUNTOS";
    renderer_draw_text(centered_x(l1, 2), 60, l1, COLOR_WHITE, COLOR_BLACK, 2);
    renderer_draw_text(centered_x(l2, 2), 82, l2, COLOR_WHITE, COLOR_BLACK, 2);
    const char *l2b = "3 VIDAS - COME 10 FRUTAS PARA SUBIR DE NIVEL";
    renderer_draw_text(centered_x(l2b, 1), 102, l2b, COLOR_GREEN, COLOR_BLACK, 1);

    // Vista previa de las dos serpientes (decorativa)
    int py = 116;
    renderer_fill_rect(CX-70, py, 9, 9, COLOR_HEAD);
    renderer_fill_rect(CX-60, py, 9, 9, COLOR_P1);
    renderer_fill_rect(CX-50, py, 9, 9, COLOR_P1);
    const char *vs = "VS";
    renderer_draw_text(centered_x(vs, 2), py-3, vs, COLOR_WHITE, COLOR_BLACK, 2);
    renderer_fill_rect(CX+41, py, 9, 9, COLOR_HEAD);
    renderer_fill_rect(CX+31, py, 9, 9, COLOR_P2);
    renderer_fill_rect(CX+21, py, 9, 9, COLOR_P2);

    const char *s1 = (n_players == 1) ? "- 1 JUGADOR (VS IA) -" : "  1 JUGADOR (VS IA)  ";
    const char *s2 = (n_players == 2) ? "- 2 JUGADORES -" : "  2 JUGADORES  ";
    renderer_draw_text(centered_x(s1, 2), 144, s1, COLOR_WHITE, COLOR_BLACK, 2);
    renderer_draw_text(centered_x(s2, 2), 166, s2, COLOR_WHITE, COLOR_BLACK, 2);

    const char *l3 = "STICK ARRIBA/ABAJO PARA CAMBIAR MODO";
    const char *l4 = "PULSA PARA JUGAR";
    renderer_draw_text(centered_x(l3, 1), 190, l3, COLOR_GREEN, COLOR_BLACK, 1);
    renderer_draw_text(centered_x(l4, 2), 204, l4, COLOR_YELLOW, COLOR_BLACK, 2);

    renderer_flush();
}

static void draw_scores_screen(void) {
    renderer_clear(COLOR_BLACK);
    highscores_draw(SN_GAME_ID, "SNAKE", 20);
    renderer_flush();
}

// ---------------------------------------------------------------------------
// Maquina de estados
// ---------------------------------------------------------------------------
// FIN DE PARTIDA (se comprueba al acabar cada ronda):
//   - 1 jugador (contra la IA): cuando el humano se queda sin vidas.
//   - 2 jugadores: cuando CUALQUIERA de los dos se queda sin vidas -- el
//     otro gana la partida (antes seguia jugando solo hasta perder las suyas).
//   - demo: nunca (se corta por tiempo o pulsacion, ver sn_tick).
static bool match_over(void) {
    if (demo) return false;
    if (n_players == 2) return snakes[0].lives == 0 || snakes[1].lives == 0;
    return snakes[0].lives == 0;
}

// Ganador de la partida a 2 jugadores: quien conserva vidas; si ambos se
// quedaron a 0 en la misma ronda, el de mayor puntuacion (0 = empate).
static int compute_match_winner(void) {
    if (snakes[0].lives > 0 && snakes[1].lives == 0) return 1;
    if (snakes[1].lives > 0 && snakes[0].lives == 0) return 2;
    if (snakes[0].score > snakes[1].score) return 1;
    if (snakes[1].score > snakes[0].score) return 2;
    return 0;
}

static void sn_tick(void) {
    blink++;
    update_dt_scale();

    if (demo) {
        bool any = controls_menu_select() || controls_get_raw_delta_x(0) != 0 || controls_get_raw_delta_x(1) != 0
                 || controls_get_raw_delta(0) != 0 || controls_get_raw_delta(1) != 0
                 || controls_button_down(BTN_IDX_J1_A) || controls_button_down(BTN_IDX_J2_A);
        if (any || ++demo_ticks >= 60*30) { g_done = true; return; }
    }

    switch (state) {

    case SN_READY:
        if (!demo) {
            int d = controls_get_raw_delta(0);      // eje Y (arriba < 0, abajo > 0)
            if (d == 0) {
                // stick en el centro: pasado un instante se vuelve a armar el cambio
                if (!menu_centered) { menu_centered = true; menu_center_since = now_ms(); }
                else if (now_ms() - menu_center_since >= SN_MENU_REARM_MS) menu_armed = true;
            } else {
                menu_centered = false;
                if (menu_armed && time_reached_ms(menu_gap_until)) {
                    int want = ((d > 0) == (SN_MENU_DOWN_IS_POSITIVE != 0)) ? 2 : 1;
                    if (want != n_players) {
                        n_players = want;
                        menu_armed = false;                      // no vuelve a cambiar hasta soltar el stick
                        menu_gap_until = make_timeout_ms(SN_MENU_MIN_GAP_MS);
                        draw_ready_screen();
                    }
                }
            }
        }
        if (time_reached_ms(ready_input_ok_time) && controls_menu_select()) {
            sound_stop_menu_music();
            game_reset_full();
            begin_round(false);
            state = SN_ROUND_START;
        }
        break;

    case SN_ROUND_START:
        if (time_reached_ms(pause_until)) {
            clear_messages();
            move_timer_ms = move_interval_ms(level);
            state = SN_PLAYING;
        }
        break;

    case SN_PLAYING: {
        for (int p = 0; p < 2; p++) {
            if (snakes[p].human) handle_turn_input(p);
            else                 ai_turn(p);
        }
        boost_now[0] = snakes[0].human && controls_button_down(BTN_IDX_J1_A);   // impulso: mas rapido y fruta x2
        boost_now[1] = snakes[1].human && controls_button_down(BTN_IDX_J2_A);
        bool boosting = boost_now[0] || boost_now[1];
        if (ai_waiting && !snakes[1].alive && time_reached_ms(ai_respawn_at)) try_respawn_ai();

        move_timer_ms -= g_elapsed_ms;
        if (move_timer_ms <= 0) {
            int32_t interval = move_interval_ms(level);
            if (boosting) interval = interval * 6 / 10;
            move_timer_ms += interval;
            do_move_step();   // puede cambiar 'state' (ronda perdida o subida de nivel)
        }
        update_and_draw_particles();
        update_special_food();
        draw_hud_if_changed(false);
        break;
    }

    case SN_ROUND_OVER:
        update_and_draw_particles();
        if (time_reached_ms(pause_until)) {
            if (match_over()) {
                if (n_players == 2) match_winner = compute_match_winner();
                sound_effect_game_over();
                update_messages("GAME OVER", COLOR_RED, 3, "", COLOR_BLACK, 1);
                pause_until = make_timeout_ms(2000);
                game_over_deadline = make_timeout_ms(8000);
                state = SN_GAME_OVER;
            } else {
                begin_round(false);
                state = SN_ROUND_START;
            }
        }
        break;

    case SN_GAME_OVER:
        if (!demo && n_players == 2) {
            // 2 jugadores: alterna "GAME OVER" con el ganador de la partida
            char w[40]; uint16_t wc = COLOR_WHITE;
            if (match_winner == 0) snprintf(w, sizeof(w), "EMPATE");
            else { snprintf(w, sizeof(w), "GANA JUGADOR %d", match_winner); wc = (match_winner == 1) ? COLOR_P1 : COLOR_P2; }
            if ((blink/15)%2==0) update_messages(w, wc, 2, "PULSA PARA CONTINUAR", COLOR_WHITE, 1);
            else                 update_messages("GAME OVER", COLOR_RED, 2, "PULSA PARA CONTINUAR", COLOR_WHITE, 1);
        } else {
            update_messages((blink/15)%2==0 ? "PULSA PARA CONTINUAR" : "GAME OVER", COLOR_YELLOW, 2, "", COLOR_BLACK, 1);
        }
        if (time_reached_ms(pause_until) &&
            (controls_menu_select() || time_reached_ms(game_over_deadline))) {
            if (!demo) {
                for (int p = 0; p < 2; p++)
                    if (snakes[p].human && highscores_is_top(SN_GAME_ID, (uint32_t)snakes[p].score))
                        highscores_enter(SN_GAME_ID, (uint32_t)snakes[p].score);   // bloqueante
            }
            draw_scores_screen();
            pause_until = make_timeout_ms(8000);
            state = SN_SCORES;
        }
        break;

    case SN_SCORES:
        if (controls_menu_select() || time_reached_ms(pause_until)) g_done = true;
        break;
    }
}

// ---------------------------------------------------------------------------
// API publica
// ---------------------------------------------------------------------------
void game_snake_run(game_mode_t mode) {
    srand((unsigned)esp_timer_get_time());

    demo = (mode == GAME_MODE_DEMO);
    n_players = (mode == GAME_MODE_2P) ? 2 : 1;
    menu_armed = false; menu_centered = false; menu_center_since = 0;   // exige centrar el stick antes del primer cambio
    menu_gap_until = 0;
    blink = 0; demo_ticks = 0; g_done = false;
    last_tick_time_ms = now_ms();
    for (int i = 0; i < MAX_PARTICLES; i++) particles[i].active = false;
    prev_msg1[0] = '\0'; prev_msg2[0] = '\0';

    if (demo) {
        n_players = 2;   // solo afecta al HUD/selector -- game_reset_full() ya fuerza IA en ambas por "demo"
        game_reset_full();
        begin_round(false);
        state = SN_ROUND_START;
    } else {
        state = SN_READY;
        draw_ready_screen();
        sound_start_menu_music();
        ready_input_ok_time = make_timeout_ms(300);
    }

    // Bucle a ritmo fijo con vTaskDelayUntil() (ver nota PORT A ESP32).
    const TickType_t period_ticks = pdMS_TO_TICKS(1000 / TARGET_FPS);
    TickType_t last_wake = xTaskGetTickCount();

    while (!g_done) {
        controls_update();
        sn_tick();
        sound_update();
        vTaskDelayUntil(&last_wake, period_ticks ? period_ticks : 1);
    }

    highscores_flush();
}