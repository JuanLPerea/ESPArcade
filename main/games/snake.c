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
 *  - GIRO RELATIVO por encoder (un detent = 90 grados, horario o
 *    antihorario), no direcciones absolutas (WASD/flechas del
 *    prototipo): con un encoder + 2 botones por jugador no hay pines
 *    para 4 direcciones absolutas, pero tampoco hacen falta -- en
 *    este juego solo es valido girar a izquierda/derecha respecto al
 *    rumbo actual (un giro de 180 grados en dos pasos equivale a
 *    chocar contra el propio cuello, asi que ni falta hace impedirlo
 *    por software: el sistema de colision ya lo resuelve solo, igual
 *    que en el prototipo).
 *
 *  - IMPULSO ("boost") con BTN_x_A mantenido: acelera el ritmo de
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
 *  - SUBIDA DE NIVEL corregida: el prototipo comprueba
 *    "puntuacion_combinada % (500*nivel) === 0", que puede no
 *    disparar NUNCA a partir del segundo nivel si los incrementos no
 *    caen justo en un multiplo exacto (los residuos posibles no
 *    siempre incluyen el 0 -- se puede comprobar con nivel=2:
 *    incrementos de 200 partiendo de 500 nunca vuelven a tocar un
 *    multiplo de 1000). Aqui se usa un UMBRAL acumulado
 *    (next_level_score, con >=) que siempre dispara tarde o
 *    temprano, sin ese riesgo.
 *
 *  - Titulo mostrado en pantalla: "SNAKE" (no "TRON SNAKE" -- se
 *    evita el nombre de la franquicia; la estetica neon si se
 *    conserva en la paleta de colores).
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "snake.h"
#include "renderer.h"
#include "controls.h"
#include "highscores.h"
#include "sound.h"

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
static absolute_time_t last_tick_time;
static int32_t g_dt_scale = FP;
static int32_t g_elapsed_ms = 16;

static void update_dt_scale(void) {
    int64_t elapsed_us = absolute_time_diff_us(last_tick_time, get_absolute_time());
    last_tick_time = get_absolute_time();
    int32_t elapsed_ms = (int32_t)(elapsed_us / 1000);
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
}

static void snake_step(Snake *s, int nx, int ny, bool grow) {
    s->head_idx = (s->head_idx - 1 + MAX_SNAKE_LEN) % MAX_SNAKE_LEN;
    s->x[s->head_idx] = (int8_t)nx; s->y[s->head_idx] = (int8_t)ny;
    if (grow) s->len++;
    // si no crece, el segmento de cola anterior queda simplemente
    // fuera de [0,len) -- no hace falta tocarlo en el array, solo en
    // pantalla (ver do_move_step: erase_cell de la cola vieja).
}

static void turn_relative(Snake *s, int dir) {
    int dx = s->qdx, dy = s->qdy;
    if (dir > 0) { s->qdx = -dy; s->qdy = dx; }   // horario
    else         { s->qdx =  dy; s->qdy = -dx; }  // antihorario
    sound_effect_select();
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

static void make_arena(int level) {
    memset(wall, 0, sizeof(wall));
    for (int c = 0; c < COLS; c++) { wall[0][c] = true; wall[ARENA_ROWS-1][c] = true; }
    for (int r = 0; r < ARENA_ROWS; r++) { wall[r][0] = true; wall[r][COLS-1] = true; }
    int n = clampi(10 + (level-1)*4, 10, 42);
    for (int i = 0; i < n; i++) {
        int x = 3 + rnd(COLS-6), y = 3 + rnd(ARENA_ROWS-6);
        if (absi(x-19) < 7 && absi(y-13) < 5) continue;   // deja libre la zona central de salida
        wall[y][x] = true;
    }
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
static int32_t next_level_score;
static int32_t enc_acc[2];
static bool    turn_locked[2];   // true = ya se aplico un giro este intervalo de movimiento (ver ai_turn/handle_turn_input)
static int     menu_enc_acc = 0;
static int32_t move_timer_ms;
static absolute_time_t pause_until;
static absolute_time_t game_over_deadline;
static absolute_time_t ready_input_ok_time;
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
static int prev_hud_level = -1;
static int prev_hud_lives1 = -1, prev_hud_lives2 = -1;

static void draw_hud_if_changed(bool force) {
    char buf[20];
    bool changed = false;
    if (snakes[0].score != prev_hud_p1 || snakes[0].lives != prev_hud_lives1 || force) {
        renderer_fill_rect(PLAY_X+1, PLAY_Y+1, 130, 14, COLOR_BLACK);
        snprintf(buf, sizeof(buf), "%05ld x%d", (long)snakes[0].score, snakes[0].lives);
        renderer_draw_text(PLAY_X+3, PLAY_Y+3, buf, COLOR_P1, COLOR_BLACK, 2);
        prev_hud_p1 = snakes[0].score; prev_hud_lives1 = snakes[0].lives; changed = true;
    }
    if (level != prev_hud_level || force) {
        renderer_fill_rect(CX-30, PLAY_Y+1, 60, 14, COLOR_BLACK);
        snprintf(buf, sizeof(buf), "L%d", level);
        renderer_draw_text(centered_x(buf, 2), PLAY_Y+3, buf, COLOR_WHITE, COLOR_BLACK, 2);
        prev_hud_level = level; changed = true;
    }
    if (snakes[1].score != prev_hud_p2 || snakes[1].lives != prev_hud_lives2 || force) {
        renderer_fill_rect(PLAY_X+PLAY_W-131, PLAY_Y+1, 130, 14, COLOR_BLACK);
        snprintf(buf, sizeof(buf), "%05ld x%d", (long)snakes[1].score, snakes[1].lives);
        int tx = PLAY_X+PLAY_W-3-(int)st7789_text_width(buf, 2);
        renderer_draw_text(tx, PLAY_Y+3, buf, COLOR_P2, COLOR_BLACK, 2);
        prev_hud_p2 = snakes[1].score; prev_hud_lives2 = snakes[1].lives; changed = true;
    }
    if (changed) renderer_flush();
}

// ---------------------------------------------------------------------------
// Ronda -- arranca/reinicia el tablero conservando puntuaciones.
// ---------------------------------------------------------------------------
static void begin_round(void) {
    // Solo reaparece quien le queden vidas -- si una serpiente ya
    // esta eliminada, se queda fuera del tablero el resto de la
    // partida (ver crash_snake) y la otra puede seguir jugando sola.
    if (snakes[0].lives > 0) reset_snake(&snakes[0], 7, 8, 0, 1);
    else                     snakes[0].alive = false;
    if (snakes[1].lives > 0) reset_snake(&snakes[1], COLS-8, ARENA_ROWS-9, 0, -1);
    else                     snakes[1].alive = false;
    make_arena(level);
    spawn_food();
    sfood.active = false;   // la comida especial no persiste entre rondas
    for (int i = 0; i < MAX_PARTICLES; i++) particles[i].active = false;
    draw_arena_static();
    prev_hud_p1 = prev_hud_p2 = -1; prev_hud_level = -1; prev_hud_lives1 = prev_hud_lives2 = -1;
    draw_hud_if_changed(true);
    char l1[24];
    snprintf(l1, sizeof(l1), "NIVEL %d", level);
    prev_msg1[0] = '\0'; prev_msg2[0] = '\0';   // el clear de arriba se llevo cualquier mensaje anterior
    update_messages(l1, COLOR_YELLOW, 2, "PREPARADOS", COLOR_WHITE, 1);
    pause_until = make_timeout_time_ms(1400);
}

static void game_reset_full(void) {
    level = 1;
    next_level_score = 5000;
    snakes[0].score = 0; snakes[1].score = 0;
    snakes[0].human = !demo;
    snakes[1].human = !demo && (n_players == 2);
    // En demo se dan vidas de sobra: el propio demo ya se corta solo
    // por tiempo o por cualquier pulsacion (ver sn_tick), así que no
    // hace falta que el sistema de vidas también lo termine.
    snakes[0].lives = demo ? 99 : 3;
    snakes[1].lives = demo ? 99 : 3;
    winner = 0;
    enc_acc[0] = enc_acc[1] = 0;
    turn_locked[0] = turn_locked[1] = false;
}

// ---------------------------------------------------------------------------
// IA -- heuristica sencilla: entre las 3 direcciones validas (recto,
// giro horario, giro antihorario) descarta las que chocan de forma
// inmediata (pared, propio cuerpo o cuerpo rival) y elige la que mas
// acerca a la comida, con un poco de aleatoriedad para que no sea
// perfectamente robotica.
//
// turn_locked limita a UN giro por intervalo de movimiento (tanto
// para la IA como para el jugador humano, ver handle_turn_input):
// sin este limite, girar el encoder con cierta rapidez podia meter
// 2 "detents" antes del siguiente paso, sumando dos giros de 90
// grados -- un giro de 180 grados de golpe, es decir, la serpiente
// se mete de cabeza contra su propio cuello. Eso se sentia como un
// choque "falso", ya que el jugador solo habia pretendido girar una
// vez. Se desbloquea en do_move_step() al confirmarse cada paso.
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

static void handle_turn_input(int player) {
    Snake *s = &snakes[player];
    if (!s->alive || !s->human) return;
    int d = controls_get_raw_delta(player);   // se consume SIEMPRE (aunque el giro este bloqueado), para no acumular un remanente que dispare un giro de mas al desbloquear
    if (turn_locked[player]) return;
    if (!d) return;
    enc_acc[player] += d;
    if (enc_acc[player] >= 4)       { turn_relative(s, +1); turn_locked[player] = true; enc_acc[player] = 0; }
    else if (enc_acc[player] <= -4) { turn_relative(s, -1); turn_locked[player] = true; enc_acc[player] = 0; }
}

static void crash_snake(int idx, int hx, int hy) {
    Snake *sn = &snakes[idx];
    if (!sn->alive) return;   // evita partirculas dobles si ya se marco por otra via este mismo paso
    sn->alive = false;
    if (sn->lives > 0) sn->lives--;
    uint16_t cols[3] = { idx == 0 ? COLOR_P1 : COLOR_P2, COLOR_WHITE, COLOR_YELLOW };
    spawn_burst(cell_x(hx)+CELL/2, cell_y(hy)+CELL/2, 16, cols, 3);
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
    for (int s = 0; s < 2; s++) {
        Snake *sn = &snakes[s];
        if (!sn->alive) continue;
        sn->dx = sn->qdx; sn->dy = sn->qdy;
        turn_locked[s] = false;   // el giro de este intervalo ya se ha consumido -- desbloquea para el siguiente
        int hx = seg_x(sn, 0), hy = seg_y(sn, 0);
        int nx = hx + sn->dx, ny = hy + sn->dy;

        if (cell_blocked(nx, ny, s, false)) {
            crash_snake(s, hx, hy);
            continue;
        }

        if (food.active && nx == food.x && ny == food.y) {
            sn->pending_growth += 1;
            sn->score += 100 * level;
            sound_effect_success();
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
        }
        if (sfood.active && nx == sfood.x && ny == sfood.y) {
            sn->pending_growth += SPECIAL_FOOD_GROWTH;
            sn->score += SPECIAL_FOOD_SCORE_MUL * level;
            sound_effect_success();
            uint16_t scols[3] = { COLOR_SFOOD, COLOR_WHITE, COLOR_YELLOW };
            spawn_burst(cell_x(nx)+CELL/2, cell_y(ny)+CELL/2, 14, scols, 3);
            sfood.active = false;
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
    if (dead0 || dead1) {
        winner = (dead0 && dead1) ? 0 : (dead0 ? 2 : 1);
        sound_effect_explosion();
        if (winner == 0) update_messages("DOBLE CHOQUE", COLOR_RED, 2, "SIGUIENTE RONDA...", COLOR_WHITE, 1);
        else {
            char l1[24]; snprintf(l1, sizeof(l1), "GANA JUGADOR %d", winner);
            update_messages(l1, winner == 1 ? COLOR_P1 : COLOR_P2, 2, "SIGUIENTE RONDA...", COLOR_WHITE, 1);
        }
        pause_until = make_timeout_time_ms(1600);
        state = SN_ROUND_OVER;
        return;
    }

    int32_t combined = snakes[0].score + snakes[1].score;
    if (combined >= next_level_score) {
        level++;
        next_level_score += 1000L * level;
        sound_effect_victory();
        begin_round();
        state = SN_ROUND_START;
    }
}

// ---------------------------------------------------------------------------
// Pantallas "estaticas"
// ---------------------------------------------------------------------------
static void draw_ready_screen(void) {
    renderer_clear(COLOR_BLACK);
    renderer_draw_text(centered_x("SNAKE", 4), 10, "SNAKE", COLOR_CYAN, COLOR_BLACK, 4);

    const char *l1 = "GIRA: IZQUIERDA/DERECHA";
    const char *l2 = "MANTEN A: IMPULSO";
    renderer_draw_text(centered_x(l1, 2), 60, l1, COLOR_WHITE, COLOR_BLACK, 2);
    renderer_draw_text(centered_x(l2, 2), 82, l2, COLOR_WHITE, COLOR_BLACK, 2);
    const char *l2b = "3 VIDAS POR JUGADOR";
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

    const char *l3 = "GIRA PARA CAMBIAR MODO";
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
// La partida termina cuando TODOS los jugadores humanos se han
// quedado sin vidas (0 en demo no cuenta -- devuelve false si no hay
// ningun humano, así que el demo nunca termina por esta vía). Si un
// humano aun tiene vidas mientras el otro puesto (humano o IA) ya se
// quedo sin ellas, la partida sigue -- ese puesto simplemente no
// vuelve a aparecer en begin_round().
static bool humans_out(void) {
    bool any_human = false;
    for (int i = 0; i < 2; i++) {
        if (snakes[i].human) {
            any_human = true;
            if (snakes[i].lives > 0) return false;
        }
    }
    return any_human;
}

static void sn_tick(void) {
    blink++;
    update_dt_scale();

    if (demo) {
        bool any = controls_menu_select() || controls_get_raw_delta(0) != 0 || controls_get_raw_delta(1) != 0
                 || controls_button_down(BTN_J1_A) || controls_button_down(BTN_J2_A);
        if (any || ++demo_ticks >= 60*30) { g_done = true; return; }
    }

    switch (state) {

    case SN_READY:
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
            game_reset_full();
            begin_round();
            state = SN_ROUND_START;
        }
        break;

    case SN_ROUND_START:
        if (time_reached(pause_until)) {
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
        bool boosting = (snakes[0].human && controls_button_down(BTN_J1_A)) ||
                        (snakes[1].human && controls_button_down(BTN_J2_A));

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
        if (time_reached(pause_until)) {
            if (humans_out()) {
                sound_effect_game_over();
                update_messages("GAME OVER", COLOR_RED, 3, "", COLOR_BLACK, 1);
                pause_until = make_timeout_time_ms(2000);
                game_over_deadline = make_timeout_time_ms(8000);
                state = SN_GAME_OVER;
            } else {
                begin_round();
                state = SN_ROUND_START;
            }
        }
        break;

    case SN_GAME_OVER:
        update_messages((blink/15)%2==0 ? "PULSA PARA CONTINUAR" : "GAME OVER", COLOR_YELLOW, 2, "", COLOR_BLACK, 1);
        if (time_reached(pause_until) &&
            (controls_menu_select() || time_reached(game_over_deadline))) {
            if (!demo) {
                for (int p = 0; p < 2; p++)
                    if (snakes[p].human && highscores_is_top(SN_GAME_ID, (uint32_t)snakes[p].score))
                        highscores_enter(SN_GAME_ID, (uint32_t)snakes[p].score);   // bloqueante
            }
            draw_scores_screen();
            pause_until = make_timeout_time_ms(8000);
            state = SN_SCORES;
        }
        break;

    case SN_SCORES:
        if (controls_menu_select() || time_reached(pause_until)) g_done = true;
        break;
    }
}

// ---------------------------------------------------------------------------
// API publica
// ---------------------------------------------------------------------------
void game_snake_run(game_mode_t mode) {
    srand(time_us_32());

    demo = (mode == GAME_MODE_DEMO);
    n_players = (mode == GAME_MODE_2P) ? 2 : 1;
    menu_enc_acc = 0;
    blink = 0; demo_ticks = 0; g_done = false;
    last_tick_time = get_absolute_time();
    for (int i = 0; i < MAX_PARTICLES; i++) particles[i].active = false;
    prev_msg1[0] = '\0'; prev_msg2[0] = '\0';

    if (demo) {
        n_players = 2;   // solo afecta al HUD/selector -- game_reset_full() ya fuerza IA en ambas por "demo"
        game_reset_full();
        begin_round();
        state = SN_ROUND_START;
    } else {
        state = SN_READY;
        draw_ready_screen();
        sound_start_menu_music();
        ready_input_ok_time = make_timeout_time_ms(300);
    }

    while (!g_done) {
        controls_update();
        sn_tick();
        sound_update();
        sleep_ms(8);
    }

    highscores_flush();
}