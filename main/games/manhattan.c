/*
 * MANHATTAN ARCADE: LABERINTO NEUROTICO (ESP32 / ESPArcade)
 *
 * Port a C del juego HTML manhattan_arcade.html. Mismo patron que
 * commando.c: sin Pico SDK, tiempo real con esp_timer, bucle con
 * vTaskDelayUntil, controles con la API de controls.h, rotacion 1
 * (apaisado 320x240) y flush completo por frame.
 *
 * Diseno de pantalla (320x240):
 *   - Campo de juego: 6x5 celdas de 44 px = 264x220 (arriba a la izquierda).
 *   - Panel derecho (56 px): brujula, 3 medidores verticales (EGO, PAR, HIP)
 *     y contadores. Solo se repinta lo que cambia.
 *   - Franja inferior (20 px): frase de Woody en 2 lineas (sin scroll).
 *
 * Controles: stick J1 = moverse, A = saltar (esquiva ratas y goteras),
 * B = frase demoledora, click de cualquier stick = salir al menu.
 *
 * Todos los dibujos son rectangulos: se disenaron sobre una celda de 64 px
 * (como en el HTML) y R() los escala a TILE.
 *
 * Los textos estan sin acentos ni signos de apertura porque la fuente 5x7
 * del driver solo cubre ' '..'z'.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "manhattan.h"
#include "../renderer.h"
#include "../controls.h"
#include "../sound.h"
#include "../highscores.h"

/* ---------------------------------------------------------
 * Configuracion
 * --------------------------------------------------------- */
#define TARGET_FPS      60
#define MH_ROTATION     1            /* apaisado, igual que el menu */
#define MENU_ROTATION   1

/* Si al empujar el stick Woody va al reves en un eje, pon 1. */
#ifndef MH_INVERT_X
#define MH_INVERT_X 0
#endif
#ifndef MH_INVERT_Y
#define MH_INVERT_Y 0
#endif
#define MH_STICK_THR    450          /* sobre 1000 */

#define BTN_JUMP        0            /* J1_A */
#define BTN_QUOTE       1            /* J1_B */
#define BTN_IDX_J1_SW   4
#define BTN_IDX_J2_SW   5

#define SCR_W   320
#define SCR_H   240
#define GRID    30
#define TILE    44
#define VCOLS   6
#define VROWS   5
#define VIEW_W  (VCOLS * TILE)       /* 264 */
#define VIEW_H  (VROWS * TILE)       /* 220 */
#define PANEL_X VIEW_W
#define PANEL_W (SCR_W - VIEW_W)     /* 56 */
#define STRIP_Y VIEW_H
#define PL_SX   ((VIEW_W - TILE) / 2)
#define PL_SY   ((VIEW_H - TILE) / 2)

#define PI_F 3.14159265f

#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define HEX(h) RGB(((h) >> 16) & 0xFF, ((h) >> 8) & 0xFF, (h) & 0xFF)

#define C_PANEL_BG  HEX(0x0d0c12)
#define C_PANEL_LN  HEX(0x3f3f46)
#define C_AMBER     HEX(0xfbbf24)
#define C_SKIN      HEX(0xf2c6a0)

/* ---------------------------------------------------------
 * Aleatorios y utilidades
 * --------------------------------------------------------- */
static int ego_decay_cnt;
static uint32_t rng_state = 0x2468ace1;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
static int rnd_int(int n) { return (int)(rng() % (uint32_t)n); }
static float frnd(void) { return (float)(rng() & 0xFFFF) / 65536.0f; }
static inline uint64_t now_us(void) { return (uint64_t)esp_timer_get_time(); }

static int fdiv(int a, int b)
{
    int q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

static void shuffle16(uint16_t *a, int n)
{
    for (int i = n - 1; i > 0; i--) {
        int j = rnd_int(i + 1);
        uint16_t t = a[i]; a[i] = a[j]; a[j] = t;
    }
}

/* ---------------------------------------------------------
 * Textos
 * --------------------------------------------------------- */
typedef struct { int8_t ego, love, hypo; const char *popup; const char *line[2]; } EnemyInfo;

enum { EN_EX, EN_THIEF, EN_DIRECTOR, EN_ACTOR, EN_PRETTY, EN_CRITIC, EN_MOTHER, EN_ANALYST,
       EN_YALE, EN_PRODUCER, EN_JAZZ, EN_AUNT, EN_WRITER, EN_STUDENT, EN_NEIGHBOR, EN_RABBI, EN_COUNT };

static const EnemyInfo ENEMY_INFO[EN_COUNT] = {
    { 0, -20, 0, "-20% PAREJA",
      { "Mi ex. Manhattan es un piso franco para el pasado.", "Ella otra vez. El guion se repite y yo sin derecho a reescritura." } },
    { -18, 0, 0, "-18% EGO",
      { "Me han robado. El ego, sobre todo. La cartera es lo de menos.", "En Nueva York te quitan la cartera y la dignidad a la vez." } },
    { 0, 0, 22, "+22% HIPOC",
      { "Un director rival. Habla de mi obra como si fuera un diagnostico.", "El tiene un festival. Yo tengo sudores frios." } },
    { -16, 0, 0, "-16% EGO",
      { "Un actor. Sonrie como si el universo le debiera un primer plano.", "El brilla. Yo parpadeo. Esa es la diferencia." } },
    { 0, -18, 0, "-18% PAREJA",
      { "Una sonrisa peligrosa. Mi monogamia acaba de pedir la palabra.", "Hermosa e inalcanzable. Perfecta para arruinarme la noche." } },
    { 0, 0, 20, "+20% HIPOC",
      { "Un critico. Habla de la muerte como quien pide la cuenta.", "Bergman, la mortalidad y yo. Una tertulia que no pedi." } },
    { -12, 0, 14, "-12% EGO +14% HIP",
      { "Mama: has comido? Y si es un tumor y no son gases?", "Mi madre diagnostica por telefono. Y siempre acierta mal." } },
    { 0, 0, 18, "+18% HIPOC",
      { "Mi analista. Callado. Caro. Y ahora tambien en la calle.", "Cincuenta minutos de silencio en plena acera." } },
    { -10, -14, 0, "-10% EGO -14% PAR",
      { "Mi amigo, el que cita a Yeats y se acuesta con tu pareja.", "Traicion intelectual. La peor clase de cliche, y aun asi duele." } },
    { -20, 0, 0, "-20% EGO",
      { "Un productor. Sonrie como quien ya ha vendido tu alma por lotes.", "Quiere mi guion, mi dignidad y ofrece un almuerzo." } },
    { -15, 0, 0, "-15% EGO",
      { "Un saxofonista. Improvisa mejor de lo que yo vivo.", "El tiene swing. Yo tengo neurosis. No es un intercambio justo." } },
    { 0, 0, 16, "+16% HIPOC",
      { "La tia de Brooklyn. Especialista en culpa kosher y postres pesados.", "Por que no llamas mas? Traduccion: eres un fracaso afectivo." } },
    { -17, 0, 0, "-17% EGO",
      { "Un escritor del Village. Publico en The New Yorker. Yo, ansiedad.", "Su prosa fluye; la mia tropieza con el psicoanalisis." } },
    { -8, 0, 12, "-8% EGO +12% HIP",
      { "Una estudiante de filosofia pregunta por el Ser. Yo, por la salida.", "Nietzsche a los veinte. Yo, a los mios, hipocondria." } },
    { 0, -12, 8, "-12% PAR +8% HIP",
      { "La vecina sabe mas de mi vida que mi terapeuta. Y cobra menos.", "Sonrie. Traduccion: ya se lo ha contado a todo el edificio." } },
    { -10, 0, 10, "-10% EGO +10% HIP",
      { "Un rabino. Amable, severo, imposible de rebatir sin sentirme peor.", "Fe, culpa y un abrigo negro. Mi trinidad personal." } },
};

static const char *const L_START[2]    = { "Manhattan otra vez. Si llego al taxi sin crisis existencial, sera un milagro.",
                                           "Solo tengo que llegar al taxi. Simple. Como el amor: imposible." };
static const char *const L_NOAMMO[2]   = { "Se me han acabado las frases. Solo me queda el silencio y el panico.",
                                           "Sin municion verbal. Es como ir a una cena de intelectuales desarmado." };
static const char *const L_QHIT[2]     = { "Por una vez las palabras duelen. A ellos.",
                                           "Touche intelectual. Freud estaria moderadamente impresionado." };
static const char *const L_BOOK[2]     = { "Kierkegaard en el bolsillo. Ahora puedo deprimirme con fundamento.",
                                           "Un libro. Para sentirme superior e inferior a la vez." };
static const char *const L_FLOWERS[2]  = { "Flores y entradas. El romance como estrategia de supervivencia.",
                                           "Un ramo: simbolo de que no se que mas hacer." };
static const char *const L_COFFEE[2]   = { "Cafe y tranquilizante. El desayuno del hombre moderno.",
                                           "La hipocondria baja. Temporalmente, como la fe en la humanidad." };
static const char *const L_TYPEW[2]    = { "La Olympia. Ahora puedo insultar con mejor puntuacion.",
                                           "Mas frases. Callar nunca ha sido una opcion." };
static const char *const L_RAT[2]      = { "Una rata! En Manhattan, casi un vecino.",
                                           "Roedores: metafora perfecta de mis pensamientos a las tres de la manana." };
static const char *const L_RATDODGE[2] = { "Esquive una rata. Hoy ya es un exito existencial.",
                                           "Salto atletico. Mi terapeuta no se lo creeria." };
static const char *const L_PIPE[2]     = { "Agua del techo. Hasta la fontaneria es una amenaza.",
                                           "Goteo. Seguro que es plomo. O peor: metafora." };
static const char *const L_GENERIC[2]  = { "Otro encuentro fortuito. Manhattan nunca te deja solo.",
                                           "Cada esquina es una confrontacion. O una metafora." };
static const char *const L_ALARM[6]    = { "El ego por los suelos. Manhattan 1, Woody 0.",
                                           "Demasiado ego. Estoy a un monologo de creerme Bergman.",
                                           "La relacion hace aguas. Como mi fontaneria emocional.",
                                           "Demasiada dependencia. Escribire sonetos en la acera.",
                                           "La hipocondria sube. Infarto o ansiedad? Si.",
                                           "Demasiados tranquilizantes. Estoy tan calmado que da miedo." };
static const char *const QUOTES[7]     = { "Tu argumento es un sofisma con pretensiones de Kierkegaard!",
                                           "Freud diria que proyectas tus neurosis en mi!",
                                           "Esto es peor que un flashback de Bergman sin subtitulos!",
                                           "McLuhan tenia razon: el medio es el mensaje, y tu eres estatica!",
                                           "Tu logica es tan solida como mi salud mental!",
                                           "Nihilismo puro... y ni siquiera con estilo!",
                                           "Spare me the amateur existentialism!" };

/* Pantallas de fin: 0 = llegada al taxi, 1..6 = derrotas (ver end_game) */
typedef struct { const char *sub, *title, *quote; } EndInfo;
static const EndInfo END_INFO[7] = {
    { "FIN DE LA PELICULA!", "LLEGADA AL TAXI",
      "Una obra maestra absoluta sobre las dudas existenciales en Manhattan." },
    { "URGENCIAS MEDICAS", "MONTE SINAI",
      "Sabia que ese pinchazo detras de la oreja no eran gases, era algo terminal..." },
    { "SOBREDOSIS EXISTENCIAL", "DEMASIADOS TRANQUILIZANTES",
      "Baje tanto la ansiedad que tambien baje, en general. El equilibrio, Woody, el equilibrio." },
    { "COLAPSO DE IDENTIDAD", "EGO EN BANCARROTA",
      "Sin ego no hay guion. Sin guion no hay Woody. Solo un tipo en la acera." },
    { "INFLACION NARCISISTA", "EGO DESCONTROLADO",
      "Me crei una obra maestra andante. Resulta que era un monologo demasiado largo." },
    { "CRISIS DE PAREJA", "RUPTURA EN EL UPPER EAST SIDE",
      "Nuestra relacion es como un tiburon: si no avanza, muere." },
    { "DEPENDENCIA TOTAL", "AMOR SIN OXIGENO",
      "La queria tanto que olvide quien era yo. Clasico. Tragico. Muy yo." },
};

/* ---------------------------------------------------------
 * Estado del juego
 * --------------------------------------------------------- */
enum { T_ROAD = 0, T_WALL = 1, T_EXIT = 2, T_CROSS = 3 };
typedef enum { ST_TITLE, ST_PLAY, ST_END, ST_SCORES } GameState;

#define MAX_EN   28
#define MAX_RAT  16
#define MAX_PIPE 14
#define MAX_REW  28
#define MAX_PROJ 8
#define MAX_POP  6

typedef struct { int x, y, px, py; bool moving; float prog, wait, cooldown, defprog; bool active, defeated; uint8_t type; } Enemy;
typedef struct { int x, y, px, py, axis, dir; bool moving, jumped, active; float prog, wait, cooldown; } Rat;
typedef struct { int x, y; float phase, cooldown; bool active; } Pipe;
typedef struct { int x, y; uint8_t type; bool collected; } Reward;
typedef struct { float wx, wy, dist; int dx, dy; bool active; } Proj;
typedef struct { int x, y; float life; uint16_t color; char text[24]; bool active; } Popup;
typedef struct {
    int x, y, px, py, dir;
    bool moving, jumping;
    float prog, jprog, invuln;
} Player;

static uint8_t maze[GRID][GRID];
static uint8_t lmg[GRID][GRID];          /* monumento: 0 = ninguno, si no tipo+1 */
static int start_x, start_y, exit_x, exit_y;

static Enemy  enemies[MAX_EN];
static Rat    rats[MAX_RAT];
static Pipe   pipes[MAX_PIPE];
static Reward rewards[MAX_REW];
static Proj   projs[MAX_PROJ];
static Popup  popups[MAX_POP];
static Player pl;

static int ego, love, hypo, ammo, steps, items, defeated_n;
static uint32_t score;
static GameState state;
static bool g_demo, g_done;
static int end_reason;
static float end_t;
static bool end_drawn, title_drawn, scores_drawn;
static float scores_t;
static bool alarm_flag[6];

static uint16_t cells[GRID * GRID];
static uint16_t tmpc[GRID * GRID];
static uint16_t freec[GRID * GRID];
static int free_n;

/* entrada leida una vez por frame */
static int  in_dir;                      /* -1 ninguna, 0 arriba, 1 abajo, 2 izq, 3 der */
static bool in_jump, in_quote, in_quit, in_start;

#define CELL(x, y) ((uint16_t)((y) * GRID + (x)))
#define CX(v) ((v) % GRID)
#define CY(v) ((v) / GRID)

static inline bool walkable(int t) { return t != T_WALL; }
static int idist(int x, int y, int x2, int y2) { return abs(x - x2) + abs(y - y2); }

/* ---------------------------------------------------------
 * Mensajes de Woody (2 lineas en la franja inferior)
 * --------------------------------------------------------- */
static char  msg_buf[128];
static float msg_t;
static bool  msg_dirty, msg_active;

static void say(const char *s)
{
    int n = 0;
    if (!s) return;
    msg_buf[n++] = '"';
    while (*s && n < (int)sizeof(msg_buf) - 2) msg_buf[n++] = *s++;
    msg_buf[n++] = '"';
    msg_buf[n] = '\0';
    msg_t = 4200.0f;
    msg_dirty = true;
    msg_active = true;
}
static const char *pick2(const char *const *a) { return a[rnd_int(2)]; }

/* parte s en lineas de hasta maxc caracteres (maxc <= 63) */
static int wrap_text(const char *s, int maxc, char out[][64], int maxlines)
{
    int n = 0;
    const char *p = s;
    while (*p && n < maxlines) {
        while (*p == ' ') p++;
        if (!*p) break;
        int len = (int)strlen(p);
        if (len <= maxc) {
            memcpy(out[n], p, (size_t)len);
            out[n][len] = '\0';
            n++;
            break;
        }
        int br = maxc;
        while (br > 0 && p[br] != ' ') br--;
        if (br == 0) br = maxc;
        memcpy(out[n], p, (size_t)br);
        out[n][br] = '\0';
        n++;
        p += br;
    }
    return n;
}

static void draw_text_c(int x0, int w, int y, const char *t, int scale, uint16_t fg, uint16_t bg)
{
    int tw = (int)st7789_text_width(t, (uint8_t)scale);
    int x = x0 + (w - tw) / 2;
    if (x < 0) x = 0;
    renderer_draw_text((uint16_t)x, (uint16_t)y, t, fg, bg, (uint8_t)scale);
}

static void draw_strip(void)
{
    renderer_fill_rect(0, STRIP_Y, SCR_W, SCR_H - STRIP_Y, COLOR_BLACK);
    if (!msg_active) return;
    char lines[2][64];
    int n = wrap_text(msg_buf, 52, lines, 2);
    int y = STRIP_Y + (n == 1 ? 6 : 2);
    for (int i = 0; i < n; i++) {
        draw_text_c(0, SCR_W, y, lines[i], 1, HEX(0xfde68a), COLOR_BLACK);
        y += 9;
    }
}

static void add_popup(int x, int y, const char *text, uint16_t color)
{
    int slot = 0;
    float oldest = 2.0f;
    for (int i = 0; i < MAX_POP; i++) {
        if (!popups[i].active) { slot = i; oldest = -1.0f; break; }
        if (popups[i].life < oldest) { oldest = popups[i].life; slot = i; }
    }
    Popup *p = &popups[slot];
    p->active = true; p->x = x; p->y = y; p->life = 1.0f; p->color = color;
    int n = 0;
    while (text[n] && n < (int)sizeof(p->text) - 1) { p->text[n] = text[n]; n++; }
    p->text[n] = '\0';
}

#define POP_SKY   HEX(0x38bdf8)
#define POP_GREEN HEX(0x34d399)
#define POP_ROSE  HEX(0xfb7185)
#define POP_YEL   HEX(0xfde047)

/* ---------------------------------------------------------
 * Laberinto
 * --------------------------------------------------------- */
static void generate_maze(void)
{
    static uint8_t stx[256], sty[256];
    static const int8_t dxs[4] = { 0, 0, -2, 2 };
    static const int8_t dys[4] = { -2, 2, 0, 0 };
    int sp = 0;

    memset(maze, T_WALL, sizeof(maze));
    memset(lmg, 0, sizeof(lmg));

    int cx = 1 + 2 * rnd_int(14), cy = 1 + 2 * rnd_int(14);
    maze[cy][cx] = T_ROAD;
    stx[0] = (uint8_t)cx; sty[0] = (uint8_t)cy; sp = 1;
    while (sp > 0) {
        int x = stx[sp - 1], y = sty[sp - 1];
        int cand[4], nc = 0;
        for (int d = 0; d < 4; d++) {
            int nx = x + dxs[d], ny = y + dys[d];
            if (nx > 0 && nx < GRID - 1 && ny > 0 && ny < GRID - 1 && maze[ny][nx] == T_WALL) cand[nc++] = d;
        }
        if (nc == 0) { sp--; continue; }
        int d = cand[rnd_int(nc)];
        maze[y + dys[d] / 2][x + dxs[d] / 2] = T_ROAD;
        maze[y + dys[d]][x + dxs[d]] = T_ROAD;
        stx[sp] = (uint8_t)(x + dxs[d]); sty[sp] = (uint8_t)(y + dys[d]); sp++;
    }

    /* todas las celdas de carretera */
    int nr = 0;
    for (int y = 1; y < GRID - 1; y++)
        for (int x = 1; x < GRID - 1; x++)
            if (maze[y][x] == T_ROAD) cells[nr++] = CELL(x, y);

    int s = cells[rnd_int(nr)];
    start_x = CX(s); start_y = CY(s);

    /* salida: de las mas lejanas (>= 85% de la distancia maxima, minimo 10) */
    int maxd = -1;
    for (int i = 0; i < nr; i++) {
        int x = CX(cells[i]), y = CY(cells[i]);
        if (x == start_x && y == start_y) continue;
        int d = idist(x, y, start_x, start_y);
        if (d > maxd) maxd = d;
    }
    float thr = (float)maxd * 0.85f;
    if (thr < 10.0f) thr = 10.0f;
    int nf = 0;
    for (int i = 0; i < nr; i++) {
        int x = CX(cells[i]), y = CY(cells[i]);
        if (x == start_x && y == start_y) continue;
        if ((float)idist(x, y, start_x, start_y) >= thr) tmpc[nf++] = cells[i];
    }
    if (nf == 0) {
        for (int i = 0; i < nr; i++) {
            int x = CX(cells[i]), y = CY(cells[i]);
            if (!(x == start_x && y == start_y) && idist(x, y, start_x, start_y) == maxd) tmpc[nf++] = cells[i];
        }
    }
    int e = tmpc[rnd_int(nf)];
    exit_x = CX(e); exit_y = CY(e);
    maze[exit_y][exit_x] = T_EXIT;

    /* pasos de cebra en cruces y tramos rectos */
    int ncc = 0;
    for (int y = 1; y < GRID - 1; y++) {
        for (int x = 1; x < GRID - 1; x++) {
            if (maze[y][x] != T_ROAD) continue;
            if (x == start_x && y == start_y) continue;
            int n = (walkable(maze[y - 1][x]) ? 1 : 0) + (walkable(maze[y + 1][x]) ? 1 : 0) +
                    (walkable(maze[y][x - 1]) ? 1 : 0) + (walkable(maze[y][x + 1]) ? 1 : 0);
            bool horiz = walkable(maze[y][x - 1]) && walkable(maze[y][x + 1]);
            bool vert = walkable(maze[y - 1][x]) && walkable(maze[y + 1][x]);
            if (n >= 3 || (n == 2 && (horiz || vert))) cells[ncc++] = CELL(x, y);
        }
    }
    shuffle16(cells, ncc);
    int cc = (int)((float)ncc * 0.25f);
    if (cc < 8) cc = 8;
    if (cc > 18) cc = 18;
    if (cc > ncc) cc = ncc;
    for (int i = 0; i < cc; i++) maze[CY(cells[i])][CX(cells[i])] = T_CROSS;

    /* monumentos sobre muros junto a calles */
    int nw = 0;
    for (int y = 1; y < GRID - 1; y++) {
        for (int x = 1; x < GRID - 1; x++) {
            if (maze[y][x] != T_WALL) continue;
            if (walkable(maze[y - 1][x]) || walkable(maze[y + 1][x]) ||
                walkable(maze[y][x - 1]) || walkable(maze[y][x + 1])) cells[nw++] = CELL(x, y);
        }
    }
    shuffle16(cells, nw);
    for (int i = 0; i < 8 && i < nw; i++) lmg[CY(cells[i])][CX(cells[i])] = (uint8_t)(i + 1);
}

static void build_free(int mind)
{
    free_n = 0;
    for (int y = 1; y < GRID - 1; y++)
        for (int x = 1; x < GRID - 1; x++)
            if ((maze[y][x] == T_ROAD || maze[y][x] == T_CROSS) && idist(x, y, start_x, start_y) >= mind)
                freec[free_n++] = CELL(x, y);
    shuffle16(freec, free_n);
}

static bool pop_free(int *x, int *y)
{
    if (free_n <= 0) return false;
    uint16_t v = freec[--free_n];
    *x = CX(v); *y = CY(v);
    return true;
}

static void populate(void)
{
    memset(enemies, 0, sizeof(enemies));
    memset(rats, 0, sizeof(rats));
    memset(pipes, 0, sizeof(pipes));
    memset(rewards, 0, sizeof(rewards));
    memset(projs, 0, sizeof(projs));
    memset(popups, 0, sizeof(popups));

    build_free(4);
    int x, y;

    for (int i = 0; i < MAX_EN && pop_free(&x, &y); i++) {
        Enemy *e = &enemies[i];
        e->active = true; e->type = (uint8_t)(i % EN_COUNT);
        e->x = e->px = x; e->y = e->py = y;
        e->wait = frnd() * 600.0f;
    }

    /* goteras: celdas de carretera junto a un muro */
    int np = 0;
    for (int yy = 1; yy < GRID - 1; yy++) {
        for (int xx = 1; xx < GRID - 1; xx++) {
            if (maze[yy][xx] != T_ROAD && maze[yy][xx] != T_CROSS) continue;
            if (maze[yy - 1][xx] == T_WALL || maze[yy + 1][xx] == T_WALL ||
                maze[yy][xx - 1] == T_WALL || maze[yy][xx + 1] == T_WALL) tmpc[np++] = CELL(xx, yy);
        }
    }
    shuffle16(tmpc, np);
    int pc = 0;
    for (int i = 0; i < np && pc < MAX_PIPE; i++) {
        int xx = CX(tmpc[i]), yy = CY(tmpc[i]);
        if ((xx == start_x && yy == start_y) || (xx == exit_x && yy == exit_y)) continue;
        pipes[pc++] = (Pipe){ xx, yy, frnd() * 6.2831f, 0.0f, true };
    }

    for (int i = 0; i < MAX_RAT && pop_free(&x, &y); i++) {
        Rat *r = &rats[i];
        bool horiz = walkable(maze[y][x - 1]) || walkable(maze[y][x + 1]);
        r->active = true;
        r->x = r->px = x; r->y = r->py = y;
        r->axis = horiz ? 0 : 1;
        r->dir = (rng() & 1) ? 1 : -1;
        r->wait = frnd() * 250.0f;
    }

    for (int i = 0; i < MAX_REW && pop_free(&x, &y); i++) {
        rewards[i] = (Reward){ x, y, (uint8_t)(i % 4), false };
    }
}

/* ---------------------------------------------------------
 * Estadisticas, alarmas y fin de partida
 * --------------------------------------------------------- */
static void end_game(int reason)
{
    state = ST_END;
    end_reason = reason;
    end_t = 0.0f;
    end_drawn = false;
    if (reason == 0) {
        int bonus = 3000 - steps * 10;
        if (bonus < 0) bonus = 0;
        score += 1000u + (uint32_t)bonus;
        sound_start_scramble_victory();
    } else {
        sound_effect_game_over();
    }
}

static void stats_changed(void)
{
    if (ego < 0) ego = 0;
    if (ego > 100) ego = 100;
    if (love < 0) love = 0;
    if (love > 100) love = 100;
    if (hypo < 0) hypo = 0;
    if (hypo > 100) hypo = 100;
    if (state != ST_PLAY) return;

    if (hypo >= 100) { end_game(1); return; }
    if (hypo <= 0)   { end_game(2); return; }
    if (ego <= 0)    { end_game(3); return; }
    if (ego >= 100)  { end_game(4); return; }
    if (love <= 0)   { end_game(5); return; }
    if (love >= 100) { end_game(6); return; }

    const bool on[6] = { ego <= 25, ego >= 85, love <= 25, love >= 85, hypo >= 75, hypo <= 25 };
    for (int i = 0; i < 6; i++) {
        if (on[i] && !alarm_flag[i]) {
            alarm_flag[i] = true;
            sound_effect_alarm();
            say(L_ALARM[i]);
        } else if (!on[i]) {
            alarm_flag[i] = false;
        }
    }
}

/* ---------------------------------------------------------
 * Logica
 * --------------------------------------------------------- */
typedef struct { int8_t ego, love, hypo, quotes; const char *popup; uint16_t color; const char *const *lines; } RewardInfo;
static const RewardInfo REW_INFO[4] = {
    { 25, 0, 0, 0, "+25% EGO",    POP_SKY,   L_BOOK },
    { 0, 25, 0, 0, "+25% PAREJA", POP_GREEN, L_FLOWERS },
    { 0, 0, -30, 0, "-30% HIPOC", POP_ROSE,  L_COFFEE },
    { 5, 0, 0, 2, "+2 CITAS",     POP_YEL,   L_TYPEW },
};

static float pl_ix(void) { return pl.moving ? (float)pl.px + (float)(pl.x - pl.px) * pl.prog : (float)pl.x; }
static float pl_iy(void) { return pl.moving ? (float)pl.py + (float)(pl.y - pl.py) * pl.prog : (float)pl.y; }

static void try_move(int dir)
{
    if (state != ST_PLAY || pl.moving) return;
    int dx = 0, dy = 0;
    if (dir == 0) dy = -1;
    else if (dir == 1) dy = 1;
    else if (dir == 2) dx = -1;
    else dx = 1;
    pl.dir = dir;

    int nx = pl.x + dx, ny = pl.y + dy;
    if (nx < 0 || nx >= GRID || ny < 0 || ny >= GRID || maze[ny][nx] == T_WALL) return;

    pl.px = pl.x; pl.py = pl.y;
    pl.x = nx; pl.y = ny;
    pl.moving = true; pl.prog = 0.0f;
    steps++;

    for (int i = 0; i < MAX_REW; i++) {
        Reward *r = &rewards[i];
        if (r->collected || r->x != pl.x || r->y != pl.y) continue;
        const RewardInfo *ri = &REW_INFO[r->type];
        r->collected = true;
        items++;
        score += 50;
        ego += ri->ego; love += ri->love; hypo += ri->hypo; ammo += ri->quotes;
        sound_effect_coin();
        add_popup(r->x, r->y, ri->popup, ri->color);
        say(pick2(ri->lines));
        stats_changed();
    }

    if (state == ST_PLAY && pl.x == exit_x && pl.y == exit_y) end_game(0);
}

static void shoot_quote(void)
{
    if (state != ST_PLAY) return;
    if (ammo <= 0) { say(pick2(L_NOAMMO)); return; }
    int slot = -1;
    for (int i = 0; i < MAX_PROJ; i++) if (!projs[i].active) { slot = i; break; }
    if (slot < 0) return;

    int dx = 0, dy = 0;
    if (pl.dir == 0) dy = -1;
    else if (pl.dir == 1) dy = 1;
    else if (pl.dir == 2) dx = -1;
    else dx = 1;

    ammo--;
    sound_effect_laser();
    say(QUOTES[rnd_int(7)]);
    projs[slot] = (Proj){ (float)pl.x, (float)pl.y, 0.0f, dx, dy, true };
}

static void update_projs(float dts)
{
    for (int i = 0; i < MAX_PROJ; i++) {
        Proj *p = &projs[i];
        if (!p->active) continue;
        float st = 9.5f * dts;
        p->wx += (float)p->dx * st;
        p->wy += (float)p->dy * st;
        p->dist += st;
        int gx = (int)floorf(p->wx + 0.5f), gy = (int)floorf(p->wy + 0.5f);
        if (gx < 0 || gx >= GRID || gy < 0 || gy >= GRID || maze[gy][gx] == T_WALL || p->dist >= 4.5f) {
            p->active = false;
            continue;
        }
        for (int k = 0; k < MAX_EN; k++) {
            Enemy *e = &enemies[k];
            if (!e->active || e->defeated) continue;
            if (fabsf((float)e->x - p->wx) < 0.65f && fabsf((float)e->y - p->wy) < 0.65f) {
                e->defeated = true; e->defprog = 0.0f; e->moving = false;
                ego += 6;
                score += 100;
                defeated_n++;
                sound_effect_success();
                add_popup(e->x, e->y, "TOUCHE! +6% EGO", POP_SKY);
                say(pick2(L_QHIT));
                p->active = false;
                stats_changed();
                break;
            }
        }
    }
}

static void update_rats(float dt)
{
    for (int i = 0; i < MAX_RAT; i++) {
        Rat *r = &rats[i];
        if (!r->active) continue;
        if (r->cooldown > 0) r->cooldown -= dt;
        if (r->moving) {
            r->prog += dt / 240.0f;
            if (r->prog >= 1.0f) {
                r->prog = 0.0f; r->moving = false;
                r->px = r->x; r->py = r->y;
                r->wait = 90.0f + frnd() * 140.0f;
            }
        } else {
            r->wait -= dt;
            if (r->wait <= 0) {
                int nx = r->x + (r->axis == 0 ? r->dir : 0);
                int ny = r->y + (r->axis == 1 ? r->dir : 0);
                if (nx > 0 && nx < GRID - 1 && ny > 0 && ny < GRID - 1 && maze[ny][nx] != T_WALL) {
                    r->px = r->x; r->py = r->y;
                    r->x = nx; r->y = ny;
                    r->moving = true; r->prog = 0.0f;
                } else {
                    r->dir = -r->dir;
                    r->wait = 120.0f;
                }
            }
        }
        float rx = r->moving ? (float)r->px + (float)(r->x - r->px) * r->prog : (float)r->x;
        float ry = r->moving ? (float)r->py + (float)(r->y - r->py) * r->prog : (float)r->y;
        bool close = fabsf(rx - pl_ix()) < 0.52f && fabsf(ry - pl_iy()) < 0.52f;
        if (close) {
            if (pl.jumping) {
                if (!r->jumped) {
                    r->jumped = true;
                    hypo -= 3;
                    score += 10;
                    add_popup(pl.x, pl.y, "ESQUIVADA! -3% HIP", POP_YEL);
                    say(pick2(L_RATDODGE));
                    stats_changed();
                }
            } else if (pl.invuln <= 0 && r->cooldown <= 0) {
                r->cooldown = 1800.0f;
                pl.invuln = 1400.0f;
                sound_effect_hit();
                hypo += 22;
                add_popup(pl.x, pl.y, "+22% HIPOCONDRIA", POP_ROSE);
                say(pick2(L_RAT));
                stats_changed();
            }
        } else {
            r->jumped = false;
        }
        if (state != ST_PLAY) return;
    }
}

static void update_pipes(float dt)
{
    for (int i = 0; i < MAX_PIPE; i++) {
        Pipe *p = &pipes[i];
        if (!p->active) continue;
        if (p->cooldown > 0) p->cooldown -= dt;
        bool on = fabsf((float)p->x - pl_ix()) < 0.45f && fabsf((float)p->y - pl_iy()) < 0.45f;
        if (on && !pl.jumping && pl.invuln <= 0 && p->cooldown <= 0) {
            p->cooldown = 1600.0f;
            pl.invuln = 1200.0f;
            sound_effect_hit();
            hypo += 12;
            add_popup(pl.x, pl.y, "+12% HIP (GOTEO)", POP_SKY);
            say(pick2(L_PIPE));
            stats_changed();
            if (state != ST_PLAY) return;
        }
    }
}

static void update_enemies(float dt)
{
    for (int i = 0; i < MAX_EN; i++) {
        Enemy *e = &enemies[i];
        if (!e->active) continue;
        if (e->defeated) {
            e->defprog += dt / 900.0f;
            if (e->defprog >= 1.0f) e->active = false;
            continue;
        }
        if (e->cooldown > 0) e->cooldown -= dt;

        if (e->moving) {
            e->prog += dt / 340.0f;
            if (e->prog >= 1.0f) {
                e->prog = 0.0f; e->moving = false;
                e->px = e->x; e->py = e->y;
            }
        } else {
            e->wait -= dt;
            if (e->wait <= 0) {
                static const int8_t dxs[4] = { 0, 0, -1, 1 };
                static const int8_t dys[4] = { -1, 1, 0, 0 };
                int ord[4] = { 0, 1, 2, 3 };
                for (int k = 3; k > 0; k--) {
                    int j = rnd_int(k + 1);
                    int t = ord[k]; ord[k] = ord[j]; ord[j] = t;
                }
                for (int k = 0; k < 4; k++) {
                    int nx = e->x + dxs[ord[k]], ny = e->y + dys[ord[k]];
                    if (nx > 0 && nx < GRID - 1 && ny > 0 && ny < GRID - 1 && maze[ny][nx] != T_WALL) {
                        e->px = e->x; e->py = e->y;
                        e->x = nx; e->y = ny;
                        e->moving = true; e->prog = 0.0f;
                        break;
                    }
                }
                e->wait = 500.0f + frnd() * 800.0f;
            }
        }

        if (pl.invuln <= 0 && e->cooldown <= 0 && e->x == pl.x && e->y == pl.y) {
            const EnemyInfo *ei = &ENEMY_INFO[e->type];
            e->cooldown = 2200.0f;
            pl.invuln = 1400.0f;
            sound_effect_hit();
            ego += ei->ego; love += ei->love; hypo += ei->hypo;
            add_popup(pl.x, pl.y, ei->popup, ei->hypo > 0 ? POP_ROSE : (ei->love < 0 ? POP_GREEN : POP_SKY));
            say(rnd_int(5) == 0 ? pick2(L_GENERIC) : ei->line[rnd_int(2)]);
            stats_changed();
            if (state != ST_PLAY) return;
        }
    }
}

static void update_game(float dt)
{
    float dts = dt / 1000.0f;
    if (pl.invuln > 0) pl.invuln -= dt;

    if (pl.jumping) {
        pl.jprog += dt / 460.0f;
        if (pl.jprog >= 1.0f) { pl.jprog = 0.0f; pl.jumping = false; }
    }
    if (pl.moving) {
        pl.prog += dt / 155.0f;
        if (pl.prog >= 1.0f) { pl.prog = 0.0f; pl.moving = false; pl.px = pl.x; pl.py = pl.y; }
    } else if (in_dir >= 0) {
        try_move(in_dir);
    }
    if (state != ST_PLAY) return;

    if (in_jump && !pl.jumping) {
        pl.jumping = true; pl.jprog = 0.0f;
        sound_effect_jump();
    }
    if (in_quote) shoot_quote();

    update_projs(dts);
    update_rats(dt);
    if (state == ST_PLAY) update_enemies(dt);
    if (state == ST_PLAY) update_pipes(dt);

    for (int i = 0; i < MAX_POP; i++) {
        Popup *p = &popups[i];
        if (!p->active) continue;
        p->life -= dt / 1400.0f;
        if (p->life <= 0) p->active = false;
    }
    if (msg_active) {
        msg_t -= dt;
        if (msg_t <= 0) { msg_active = false; msg_dirty = true; }
    }
}

static void new_game(void)
{
    ego_decay_cnt = 0;
    generate_maze();
    populate();
    memset(&pl, 0, sizeof(pl));
    pl.x = pl.px = start_x; pl.y = pl.py = start_y;
    pl.dir = 1;
    ego = 60; love = 60; hypo = 20; ammo = 5;
    steps = 0; items = 0; defeated_n = 0; score = 0;
    memset(alarm_flag, 0, sizeof(alarm_flag));
    msg_active = false; msg_dirty = true;
    state = ST_PLAY;
    say(pick2(L_START));
}

/* ---------------------------------------------------------
 * Dibujo: primitivas
 * --------------------------------------------------------- */
static int g_ox, g_oy;

/* rectangulo en coordenadas de celda de 64 px, escalado a TILE y
 * recortado al campo de juego (nunca pisa el panel ni la franja) */
static void R(int x, int y, int w, int h, uint16_t c)
{
    int x0 = g_ox + x * TILE / 64, y0 = g_oy + y * TILE / 64;
    int x1 = g_ox + (x + w) * TILE / 64, y1 = g_oy + (y + h) * TILE / 64;
    if (x1 <= x0) { x1 = x0 + 1; }
    if (y1 <= y0) { y1 = y0 + 1; }
    if (x0 < 0) { x0 = 0; }
    if (y0 < 0) { y0 = 0; }
    if (x1 > VIEW_W) { x1 = VIEW_W; }
    if (y1 > VIEW_H) { y1 = VIEW_H; }
    if (x1 <= x0 || y1 <= y0) return;
    renderer_fill_rect((uint16_t)x0, (uint16_t)y0, (uint16_t)(x1 - x0), (uint16_t)(y1 - y0), c);
}

static void lm_text(const char *t, uint16_t fg, uint16_t bg)
{
    if (g_ox < 0 || g_oy < 0 || g_ox + TILE > VIEW_W || g_oy + TILE > VIEW_H) return;
    int w = (int)st7789_text_width(t, 1);
    renderer_draw_text((uint16_t)(g_ox + (TILE - w) / 2), (uint16_t)(g_oy + TILE - 9), t, fg, bg, 1);
}

static void draw_landmark(int type)
{
    switch (type) {
    case 0:  /* Estatua de la Libertad */
        R(0, 0, 64, 64, HEX(0x0c4a6e));
        R(20, 28, 24, 28, HEX(0x86efac)); R(26, 10, 12, 20, HEX(0x4ade80)); R(22, 6, 20, 8, HEX(0x22c55e));
        R(28, 2, 3, 10, HEX(0xfde047)); R(26, 1, 7, 3, HEX(0xfbbf24));
        R(24, 8, 3, 3, HEX(0xfde047)); R(37, 8, 3, 3, HEX(0xfde047));
        lm_text("LIBERTY", HEX(0xe0f2fe), HEX(0x0c4a6e));
        break;
    case 1:  /* Empire State */
        R(0, 0, 64, 64, HEX(0x1e293b));
        R(18, 8, 28, 48, HEX(0x64748b)); R(22, 4, 20, 8, HEX(0x94a3b8)); R(28, 0, 8, 8, HEX(0xcbd5e1)); R(30, 0, 4, 4, HEX(0xf8fafc));
        for (int r = 12; r < 52; r += 6) {
            R(22, r, 20, 2, HEX(0x334155)); R(24, r + 1, 3, 2, HEX(0xfde047)); R(37, r + 1, 3, 2, HEX(0xfde047));
        }
        lm_text("EMPIRE", HEX(0xe2e8f0), HEX(0x1e293b));
        break;
    case 2:  /* Chrysler */
        R(0, 0, 64, 64, HEX(0x0f172a));
        R(16, 16, 32, 40, HEX(0x94a3b8)); R(20, 8, 24, 12, HEX(0xcbd5e1)); R(26, 2, 12, 10, HEX(0xe2e8f0)); R(30, 0, 4, 6, HEX(0xfbbf24));
        for (int i = 0; i < 4; i++) R(18 + i * 7, 20, 4, 30, HEX(0x64748b));
        lm_text("CHRYSLR", HEX(0xfde68a), HEX(0x0f172a));
        break;
    case 3:  /* Flatiron */
        R(0, 0, 64, 64, HEX(0x292524));
        R(28, 8, 8, 48, HEX(0xa8a29e)); R(24, 16, 16, 40, HEX(0x78716c)); R(20, 28, 24, 28, HEX(0x57534e));
        for (int r = 20; r < 56; r += 8) { R(26, r, 12, 3, HEX(0x1c1917)); R(28, r + 1, 3, 2, HEX(0xfef3c7)); }
        lm_text("FLATIRN", HEX(0xe7e5e4), HEX(0x292524));
        break;
    case 4:  /* Puente de Brooklyn */
        R(0, 0, 64, 64, HEX(0x0c4a6e));
        R(4, 40, 56, 8, HEX(0x57534e)); R(10, 12, 8, 36, HEX(0xa8a29e)); R(46, 12, 8, 36, HEX(0xa8a29e));
        R(8, 10, 12, 6, HEX(0xd6d3d1)); R(44, 10, 12, 6, HEX(0xd6d3d1));
        for (int i = 0; i < 6; i++) { R(18 + i * 5, 16 + i, 2, 2, HEX(0xe7e5e4)); R(44 - i * 5, 16 + i, 2, 2, HEX(0xe7e5e4)); }
        lm_text("BRIDGE", HEX(0xbae6fd), HEX(0x0c4a6e));
        break;
    case 5:  /* Central Park */
        R(0, 0, 64, 64, HEX(0x14532d));
        R(8, 8, 48, 48, HEX(0x166534)); R(16, 20, 32, 8, HEX(0x4ade80)); R(12, 12, 8, 8, HEX(0x22c55e));
        R(40, 14, 10, 10, HEX(0x16a34a)); R(28, 36, 12, 12, HEX(0x15803d)); R(20, 40, 6, 6, HEX(0x86efac));
        lm_text("C.PARK", HEX(0xdcfce7), HEX(0x14532d));
        break;
    case 6:  /* The Met */
        R(0, 0, 64, 64, HEX(0x44403c));
        R(6, 14, 52, 40, HEX(0xd6d3d1)); R(10, 8, 44, 10, HEX(0xa8a29e));
        R(14, 22, 8, 24, HEX(0x1c1917)); R(28, 22, 8, 24, HEX(0x1c1917)); R(42, 22, 8, 24, HEX(0x1c1917)); R(24, 36, 16, 10, HEX(0x292524));
        lm_text("THE MET", HEX(0xfef3c7), HEX(0x44403c));
        break;
    default: /* Metro */
        R(0, 0, 64, 64, HEX(0x1c1917));
        R(10, 18, 44, 30, HEX(0x365314)); R(14, 10, 36, 12, HEX(0x14532d)); R(18, 12, 28, 8, HEX(0xfef08a));
        R(22, 32, 20, 16, HEX(0x0a0a0a));
        for (int r = 0; r < 4; r++) R(24, 34 + r * 4, 16, 2, HEX(0xa3a3a3));
        lm_text("SUBWAY", HEX(0xa3e635), HEX(0x1c1917));
        break;
    }
}

static void draw_tile(int px, int py, int type, int rx, int ry)
{
    g_ox = px; g_oy = py;
    if (type == T_WALL) {
        if (rx >= 0 && rx < GRID && ry >= 0 && ry < GRID && lmg[ry][rx]) {
            draw_landmark(lmg[ry][rx] - 1);
            return;
        }
        int v = (rx * 7 + ry * 13) & 3;
        if (v == 0) {
            R(0, 0, 64, 64, HEX(0x5c3826));
            for (int r = 0; r < 64; r += 16) R(0, r, 64, 1, HEX(0x3b2318));
            for (int c = 0; c < 64; c += 20) R(c, 0, 1, 64, HEX(0x3b2318));
            R(12, 14, 16, 24, HEX(0x1e293b)); R(36, 14, 16, 24, HEX(0x1e293b));
            R(14, 16, 12, 10, HEX(0xfde047)); R(38, 16, 12, 10, HEX(0xfde047));
            R(14, 28, 12, 8, HEX(0x334155)); R(38, 28, 12, 8, HEX(0x334155));
        } else if (v == 1) {
            R(0, 0, 64, 64, HEX(0x73422e)); R(4, 4, 56, 56, HEX(0x874d35)); R(0, 0, 64, 8, HEX(0x4a2818));
            R(14, 18, 14, 22, HEX(0x0f172a)); R(36, 18, 14, 22, HEX(0x0f172a));
            R(16, 20, 10, 18, HEX(0xfbbf24)); R(38, 20, 10, 18, HEX(0xfbbf24));
        } else if (v == 2) {
            R(0, 0, 64, 64, HEX(0x2b3137));
            for (int r = 8; r < 56; r += 14) {
                R(8, r, 48, 10, HEX(0x111827));
                for (int c = 12; c < 52; c += 10) R(c, r + 2, 6, 6, HEX(0x93c5fd));
            }
            R(12, 2, 40, 6, HEX(0x4b5563));
        } else {
            R(0, 0, 64, 64, HEX(0x42281f));
            for (int r = 0; r < 64; r += 12) R(0, r, 64, 1, HEX(0x24140f));
            R(4, 10, 10, 44, HEX(0x1f2937)); R(14, 20, 20, 4, HEX(0x1f2937)); R(14, 40, 20, 4, HEX(0x1f2937));
            R(40, 16, 16, 26, HEX(0x0f172a)); R(43, 19, 10, 20, HEX(0xe2e8f0));
        }
        return;
    }

    bool in = rx >= 0 && rx < GRID && ry >= 0 && ry < GRID;
    bool wl = in && rx > 0 && maze[ry][rx - 1] == T_WALL;
    bool wr = in && rx < GRID - 1 && maze[ry][rx + 1] == T_WALL;
    bool wu = in && ry > 0 && maze[ry - 1][rx] == T_WALL;
    bool wd = in && ry < GRID - 1 && maze[ry + 1][rx] == T_WALL;

    if (type == T_ROAD) {
        R(0, 0, 64, 64, HEX(0x2a2a2e)); R(2, 2, 60, 60, HEX(0x323236));
        int seed = (rx * 17 + ry * 31) & 7;
        for (int i = 0; i < 6; i++) R(6 + ((seed * 13 + i * 19) % 48), 6 + ((seed * 7 + i * 23) % 48), 2, 2, HEX(0x3a3a40));
        bool roadH = !wl || !wr;
        bool roadV = !wu || !wd;
        if (roadH && !roadV) { R(4, 31, 10, 2, HEX(0xc4a035)); R(50, 31, 10, 2, HEX(0xc4a035)); }
        else if (roadV && !roadH) { R(31, 4, 2, 10, HEX(0xc4a035)); R(31, 50, 2, 10, HEX(0xc4a035)); }
        if (wl) R(0, 0, 3, 64, HEX(0x4a4a50));
        if (wr) R(61, 0, 3, 64, HEX(0x4a4a50));
        if (wu) R(0, 0, 64, 3, HEX(0x4a4a50));
        if (wd) R(0, 61, 64, 3, HEX(0x4a4a50));
    } else if (type == T_CROSS) {
        R(0, 0, 64, 64, HEX(0x2a2a2e)); R(2, 2, 60, 60, HEX(0x323236));
        int h = (!wl ? 1 : 0) + (!wr ? 1 : 0), vv = (!wu ? 1 : 0) + (!wd ? 1 : 0);
        bool horiz = (h == vv) ? (h >= 1) : (h > vv);
        if (horiz) { for (int r = 6; r < 58; r += 11) R(6, r, 52, 7, HEX(0xf1f5f9)); }
        else       { for (int c = 6; c < 58; c += 11) R(c, 6, 7, 52, HEX(0xf1f5f9)); }
    } else {
        R(0, 0, 64, 64, HEX(0x0f172a));
        R(8, 16, 48, 32, HEX(0xfbbf24)); R(12, 10, 40, 6, HEX(0xb45309));
        R(14, 20, 14, 10, HEX(0x1e293b)); R(36, 20, 14, 10, HEX(0x1e293b));
        if (g_ox >= 0 && g_oy >= 0 && g_ox + TILE <= VIEW_W && g_oy + TILE <= VIEW_H) {
            int w = (int)st7789_text_width("TAXI", 1);
            renderer_draw_text((uint16_t)(g_ox + (TILE - w) / 2), (uint16_t)(g_oy + TILE * 33 / 64), "TAXI", COLOR_BLACK, HEX(0xfbbf24), 1);
        }
    }
}

/* ---------------------------------------------------------
 * Dibujo: sprites
 * --------------------------------------------------------- */
static void draw_reward(int type, int t_ms)
{
    int cx = 32, cy = 32;
    R(cx - 10, cy + 14, 20, 4, HEX(0x1c1c20));
    if (type == 0) {
        R(cx - 8, cy - 6, 16, 14, HEX(0x0284c7)); R(cx - 6, cy - 8, 12, 3, HEX(0x38bdf8)); R(cx - 4, cy - 2, 8, 1, HEX(0xf8fafc));
    } else if (type == 1) {
        R(cx - 2, cy - 4, 4, 12, HEX(0x15803d)); R(cx - 8, cy - 10, 6, 6, HEX(0xec4899));
        R(cx + 2, cy - 10, 6, 6, HEX(0xa855f7)); R(cx - 3, cy - 14, 6, 6, HEX(0xe0e7ff));
    } else if (type == 2) {
        int s = (int)(sinf((float)t_ms * 0.01f) * 2.0f);
        R(cx - 7, cy - 2, 14, 12, HEX(0xf8fafc)); R(cx + 7, cy, 3, 6, HEX(0xcbd5e1));
        R(cx - 4 + s, cy - 10, 2, 4, HEX(0xe2e8f0)); R(cx + 2 - s, cy - 12, 2, 4, HEX(0xe2e8f0));
    } else {
        R(cx - 11, cy - 2, 22, 11, HEX(0x334155)); R(cx - 9, cy - 4, 18, 4, HEX(0x64748b));
        R(cx - 6, cy - 12, 12, 9, HEX(0xf8fafc)); R(cx - 4, cy - 9, 8, 1, HEX(0x0f172a)); R(cx - 4, cy - 6, 6, 1, HEX(0x0f172a));
        R(cx - 8, cy + 2, 16, 4, HEX(0xcbd5e1));
    }
}

static void draw_rat(const Rat *r, int t_ms)
{
    bool right = r->dir > 0;
    int scurry = r->moving ? (int)(sinf(r->prog * PI_F * 4.0f) * 2.0f) : 0;
    int cx = 20, cy = 38 + scurry;
    R(cx + 2, 48, 20, 4, HEX(0x1c1c20));
    if (right) { R(cx - 7, cy + 5, 9, 2, HEX(0xf472b6)); R(cx - 10, cy + 3, 4, 2, HEX(0xf472b6)); }
    else       { R(cx + 22, cy + 5, 9, 2, HEX(0xf472b6)); R(cx + 29, cy + 3, 4, 2, HEX(0xf472b6)); }
    R(cx + 2, cy + 2, 20, 9, HEX(0x52525b)); R(cx + 4, cy, 16, 3, HEX(0x71717a));
    if (right) {
        R(cx + 18, cy + 3, 7, 7, HEX(0x71717a)); R(cx + 16, cy - 2, 4, 4, HEX(0xa1a1aa));
        R(cx + 21, cy + 4, 2, 2, HEX(0xef4444)); R(cx + 25, cy + 6, 2, 2, HEX(0xf472b6));
    } else {
        R(cx - 1, cy + 3, 7, 7, HEX(0x71717a)); R(cx + 4, cy - 2, 4, 4, HEX(0xa1a1aa));
        R(cx + 1, cy + 4, 2, 2, HEX(0xef4444)); R(cx - 3, cy + 6, 2, 2, HEX(0xf472b6));
    }
    int ls = r->moving ? (int)(sinf((float)t_ms * 0.04f) * 2.0f) : 0;
    R(cx + 5 + ls, cy + 10, 3, 3, HEX(0xf472b6)); R(cx + 15 - ls, cy + 10, 3, 3, HEX(0xf472b6));
}

static void draw_pipe(const Pipe *p, int t_ms)
{
    R(26, 2, 12, 8, HEX(0x52525b)); R(28, 8, 8, 6, HEX(0x3f3f46)); R(30, 12, 4, 4, HEX(0x27272a));
    R(28, 4, 2, 2, HEX(0xa1a1aa)); R(34, 4, 2, 2, HEX(0xa1a1aa));
    float ph = fmodf((float)t_ms * 0.004f + p->phase, 1.0f);
    R(30, 16 + (int)(ph * 36.0f), 4, 6, HEX(0x38bdf8));
    if (ph < 0.8f) R(31, 22 + (int)(ph * 36.0f), 2, 3, HEX(0x7dd3fc));
    float ph2 = fmodf(ph + 0.45f, 1.0f);
    R(31, 16 + (int)(ph2 * 36.0f), 3, 5, HEX(0x0ea5e9));
    R(22, 54, 20, 4, HEX(0x1e4a6a));
}

static void draw_bubble(const Proj *p)
{
    int cx = 32, cy = 18;
    R(cx - 16, cy - 4, 32, 14, HEX(0x09090b)); R(cx - 14, cy - 6, 28, 18, HEX(0x09090b));
    R(cx - 14, cy - 4, 28, 14, HEX(0xfef08a)); R(cx - 12, cy - 5, 24, 16, HEX(0xfef08a));
    R(cx - 10, cy - 3, 8, 3, HEX(0xfef9c3));
    if (p->dy < 0)      { R(cx - 2, cy + 10, 5, 4, HEX(0x09090b)); R(cx - 1, cy + 10, 3, 5, HEX(0xfef08a)); }
    else if (p->dy > 0) { R(cx - 2, cy - 10, 5, 4, HEX(0x09090b)); R(cx - 1, cy - 11, 3, 5, HEX(0xfef08a)); }
    else if (p->dx < 0) { R(cx + 12, cy, 5, 5, HEX(0x09090b)); R(cx + 12, cy + 1, 6, 3, HEX(0xfef08a)); }
    else                { R(cx - 18, cy, 5, 5, HEX(0x09090b)); R(cx - 19, cy + 1, 6, 3, HEX(0xfef08a)); }
    R(cx - 8, cy + 2, 3, 3, HEX(0x09090b)); R(cx - 1, cy + 2, 3, 3, HEX(0x09090b)); R(cx + 6, cy + 2, 3, 3, HEX(0x09090b));
}

static void en_eyes(int x, int y, int dy)
{
    R(x + 11, y + 8 + dy, 3, 3, HEX(0x111827)); R(x + 18, y + 8 + dy, 3, 3, HEX(0x111827));
}
static void en_head(int x, int y, uint16_t skin)
{
    R(x + 10, y + 4, 12, 13, skin);
}

static void draw_enemy(const Enemy *e)
{
    int x = 16, y = 10;
    if (e->defeated) {
        float t = e->defprog;
        if (t > 0.5f && ((int)(t * 24.0f) & 1)) return;
        int fy = (int)(t * 8.0f);
        R(x + 4, y + 26 + fy, 28, 14, HEX(0x334155));
        R(x + 8, y + 22 + fy, 12, 10, HEX(0xeec39a));
        R(x + 10, y + 24 + fy, 3, 3, HEX(0x111827)); R(x + 16, y + 24 + fy, 3, 3, HEX(0x111827));
        R(x + 30, y + 24 + fy, 6, 10, HEX(0x1e293b)); R(x + 36, y + 22 + fy, 6, 8, HEX(0x1e293b));
        return;
    }
    const uint16_t skin = HEX(0xeec39a);
    R(x + 6, y + 36, 6, 14, HEX(0x1e293b)); R(x + 18, y + 36, 6, 14, HEX(0x1e293b));
    switch (e->type) {
    case EN_EX:
        R(x + 5, y + 16, 20, 20, HEX(0x581c87)); en_head(x, y, HEX(0xfbcfe8)); R(x + 8, y + 1, 16, 5, HEX(0x4a044e));
        en_eyes(x, y, 0); R(x + 14, y + 12, 4, 2, HEX(0x9f1239));
        break;
    case EN_THIEF:
        R(x + 5, y + 16, 20, 20, HEX(0x27272a)); en_head(x, y + 2, skin); R(x + 8, y + 2, 16, 8, HEX(0x18181b));
        en_eyes(x, y, 2); R(x + 2, y + 22, 8, 10, HEX(0x3f3f46)); R(x + 3, y + 24, 6, 2, HEX(0xa16207));
        break;
    case EN_DIRECTOR:
        R(x + 5, y + 16, 20, 20, HEX(0x7f1d1d)); en_head(x, y, skin); R(x + 9, y + 1, 14, 5, HEX(0x111827));
        en_eyes(x, y, 1); R(x + 24, y + 18, 8, 5, HEX(0xfbbf24)); R(x + 30, y + 19, 3, 3, HEX(0xfbbf24));
        break;
    case EN_ACTOR:
        R(x + 5, y + 16, 20, 20, HEX(0x1e293b)); R(x + 14, y + 16, 2, 18, HEX(0xf8fafc)); en_head(x, y, skin);
        R(x + 9, y + 1, 14, 4, HEX(0x78350f)); R(x + 10, y + 8, 5, 3, HEX(0x111827)); R(x + 17, y + 8, 5, 3, HEX(0x111827));
        break;
    case EN_PRETTY:
        R(x + 5, y + 16, 20, 22, HEX(0xdb2777)); en_head(x, y, HEX(0xfde68a));
        R(x + 7, y + 1, 18, 6, HEX(0x92400e)); R(x + 6, y + 6, 4, 12, HEX(0x92400e)); R(x + 22, y + 6, 4, 12, HEX(0x92400e));
        en_eyes(x, y, 0); R(x + 13, y + 12, 6, 2, HEX(0xbe123c));
        break;
    case EN_CRITIC:
        R(x + 5, y + 16, 20, 20, HEX(0x1e3a8a)); en_head(x, y, skin); R(x + 9, y + 1, 14, 4, HEX(0xcbd5e1));
        en_eyes(x, y, 0); R(x + 22, y + 15, 6, 8, HEX(0xf8fafc)); R(x + 23, y + 17, 4, 1, HEX(0x64748b));
        break;
    case EN_MOTHER:
        R(x + 4, y + 16, 22, 22, HEX(0x9a3412)); en_head(x, y + 1, skin); R(x + 8, y + 2, 16, 5, HEX(0xa8a29e));
        en_eyes(x, y, 1); R(x + 24, y + 22, 7, 10, HEX(0x78350f)); R(x + 13, y + 13, 6, 2, HEX(0x7f1d1d));
        break;
    case EN_ANALYST:
        R(x + 5, y + 16, 20, 20, HEX(0x475569)); en_head(x, y, skin); R(x + 9, y + 1, 14, 4, HEX(0x1e293b));
        R(x + 10, y + 8, 5, 4, HEX(0x111827)); R(x + 17, y + 8, 5, 4, HEX(0x111827)); R(x + 15, y + 9, 2, 2, HEX(0x111827));
        R(x + 24, y + 18, 5, 8, HEX(0xf8fafc));
        break;
    case EN_YALE:
        R(x + 5, y + 16, 20, 20, HEX(0x166534)); en_head(x, y, skin); R(x + 9, y + 1, 14, 4, HEX(0x292524));
        en_eyes(x, y, 0); R(x + 13, y + 12, 6, 2, HEX(0xb91c1c)); R(x + 2, y + 20, 6, 8, HEX(0xfef3c7));
        break;
    case EN_PRODUCER:
        R(x + 5, y + 16, 20, 20, HEX(0xa8a29e)); en_head(x, y, skin); R(x + 8, y + 1, 16, 4, HEX(0x44403c));
        R(x + 10, y + 8, 5, 3, HEX(0x111827)); R(x + 17, y + 8, 5, 3, HEX(0x111827));
        R(x + 25, y + 14, 6, 2, HEX(0xfafaf9)); R(x + 30, y + 14, 2, 2, HEX(0xef4444));
        break;
    case EN_JAZZ:
        R(x + 5, y + 16, 20, 20, HEX(0x1e3a5f)); en_head(x, y, skin); R(x + 9, y + 1, 14, 4, HEX(0x0f172a));
        en_eyes(x, y, 0); R(x + 24, y + 12, 4, 18, HEX(0xb45309)); R(x + 23, y + 10, 6, 4, HEX(0xca8a04));
        break;
    case EN_AUNT:
        R(x + 4, y + 16, 22, 22, HEX(0x6b21a8)); en_head(x, y + 2, skin); R(x + 7, y + 1, 18, 6, HEX(0x581c87));
        en_eyes(x, y, 3); R(x + 13, y + 13, 6, 2, HEX(0x9f1239));
        break;
    case EN_WRITER:
        R(x + 5, y + 16, 20, 22, HEX(0x57534e)); en_head(x, y, skin); R(x + 9, y + 1, 14, 4, HEX(0x292524));
        R(x + 8, y + 14, 16, 4, HEX(0xb91c1c)); en_eyes(x, y, 0); R(x + 24, y + 20, 5, 7, HEX(0xfefce8));
        break;
    case EN_STUDENT:
        R(x + 5, y + 16, 20, 20, HEX(0x0e7490)); en_head(x, y, skin); R(x + 9, y + 1, 14, 5, HEX(0x155e75));
        en_eyes(x, y, 0); R(x + 2, y + 18, 6, 12, HEX(0x164e63)); R(x + 24, y + 20, 5, 7, HEX(0xfef3c7));
        break;
    case EN_NEIGHBOR:
        R(x + 4, y + 16, 22, 22, HEX(0xa16207)); en_head(x, y + 1, skin);
        R(x + 8, y + 1, 4, 4, HEX(0xfcd34d)); R(x + 14, y, 4, 4, HEX(0xfcd34d)); R(x + 20, y + 1, 4, 4, HEX(0xfcd34d));
        en_eyes(x, y, 1); R(x + 13, y + 13, 6, 2, HEX(0xb45309));
        break;
    default:
        R(x + 5, y + 16, 20, 22, HEX(0x1c1917)); R(x + 10, y + 6, 12, 10, skin); R(x + 8, y + 1, 16, 6, HEX(0x0a0a0a));
        R(x + 11, y + 9, 3, 3, HEX(0x111827)); R(x + 18, y + 9, 3, 3, HEX(0x111827)); R(x + 11, y + 14, 10, 5, HEX(0xd6d3d1));
        break;
    }
}

static void draw_woody_sprite(bool jumping, int swing, int bob)
{
    int x = 14, y = 8 + bob;
    int lj = jumping ? -3 : 0;
    R(x + 10, y + 34 + (jumping ? lj : swing), 6, 13, HEX(0x3b281e));
    R(x + 20, y + 34 + (jumping ? lj : -swing), 6, 13, HEX(0x3b281e));
    R(x + 9, y + 46 + (jumping ? lj : swing), 8, 3, HEX(0x18181b));
    R(x + 19, y + 46 + (jumping ? lj : -swing), 8, 3, HEX(0x18181b));
    R(x + 7, y + 17, 22, 18, HEX(0x855834)); R(x + 14, y + 17, 8, 16, HEX(0xd6c7b2));
    R(x + 15, y + 20, 6, 1, HEX(0x991b1b)); R(x + 15, y + 24, 6, 1, HEX(0x1e40af)); R(x + 15, y + 28, 6, 1, HEX(0x991b1b));
    if (jumping) {
        R(x + 2, y + 11, 4, 12, HEX(0x855834)); R(x + 30, y + 11, 4, 12, HEX(0x855834));
        R(x + 2, y + 8, 4, 3, C_SKIN); R(x + 30, y + 8, 4, 3, C_SKIN);
    } else {
        int s = swing * 4 / 5;
        R(x + 3, y + 18 - s, 4, 13, HEX(0x855834)); R(x + 29, y + 18 + s, 4, 13, HEX(0x855834));
        R(x + 3, y + 31 - s, 4, 3, C_SKIN); R(x + 29, y + 31 + s, 4, 3, C_SKIN);
    }
    R(x + 10, y + 4, 16, 13, C_SKIN); R(x + 14, y + 16, 8, 2, HEX(0xd9a77c));
    R(x + 8, y + 1, 20, 4, HEX(0xb84a27)); R(x + 6, y + 3, 5, 6, HEX(0xb84a27)); R(x + 25, y + 3, 5, 5, HEX(0xb84a27));
    R(x + 10, y + 7, 7, 5, HEX(0x111111)); R(x + 19, y + 7, 7, 5, HEX(0x111111)); R(x + 17, y + 9, 2, 2, HEX(0x111111));
    R(x + 12, y + 8, 3, 3, COLOR_WHITE); R(x + 21, y + 8, 3, 3, COLOR_WHITE);
    R(x + 13, y + 9, 1, 2, HEX(0x111111)); R(x + 22, y + 9, 1, 2, HEX(0x111111));
    R(x + 15, y + 13, 6, 2, HEX(0x732626));
}

static void draw_player(void)
{
    if (pl.invuln > 0 && ((int)(pl.invuln / 80.0f) & 1) == 0) return;
    g_ox = PL_SX; g_oy = PL_SY;
    int jump_px = pl.jumping ? (int)(sinf(pl.jprog * PI_F) * 24.0f * (float)TILE / 64.0f) : 0;
    int shrink = jump_px / 4;
    R(20 + shrink, 54, (24 - shrink * 2) > 10 ? (24 - shrink * 2) : 10, 5, HEX(0x1c1c20));
    int swing = pl.moving ? (int)(sinf(pl.prog * PI_F * 2.0f) * 5.0f) : 0;
    int bob = (!pl.jumping && pl.moving) ? -(int)fabsf(sinf(pl.prog * PI_F * 2.0f) * 2.0f) : 0;
    g_oy -= jump_px;
    draw_woody_sprite(pl.jumping, pl.jumping ? 6 : swing, bob);
}

/* ---------------------------------------------------------
 * Dibujo: campo de juego
 * --------------------------------------------------------- */
static void render_game(int t_ms)
{
    float fx = pl_ix(), fy = pl_iy();
    int cam_x = (int)lroundf((fx - ((float)VCOLS - 1.0f) / 2.0f) * (float)TILE);
    int cam_y = (int)lroundf((fy - ((float)VROWS - 1.0f) / 2.0f) * (float)TILE);

    int c0 = fdiv(cam_x, TILE), c1 = fdiv(cam_x + VIEW_W - 1, TILE);
    int r0 = fdiv(cam_y, TILE), r1 = fdiv(cam_y + VIEW_H - 1, TILE);
    for (int r = r0; r <= r1; r++) {
        for (int c = c0; c <= c1; c++) {
            int t = (r >= 0 && r < GRID && c >= 0 && c < GRID) ? maze[r][c] : T_WALL;
            draw_tile(c * TILE - cam_x, r * TILE - cam_y, t, c, r);
        }
    }

#define VISIBLE(sx, sy) ((sx) > -TILE && (sx) < VIEW_W && (sy) > -TILE && (sy) < VIEW_H)

    for (int i = 0; i < MAX_PIPE; i++) {
        const Pipe *p = &pipes[i];
        int sx = p->x * TILE - cam_x, sy = p->y * TILE - cam_y;
        if (p->active && VISIBLE(sx, sy)) { g_ox = sx; g_oy = sy; draw_pipe(p, t_ms); }
    }
    for (int i = 0; i < MAX_REW; i++) {
        const Reward *w = &rewards[i];
        int sx = w->x * TILE - cam_x, sy = w->y * TILE - cam_y;
        if (!w->collected && VISIBLE(sx, sy)) { g_ox = sx; g_oy = sy; draw_reward(w->type, t_ms); }
    }
    for (int i = 0; i < MAX_RAT; i++) {
        const Rat *r = &rats[i];
        float rx = r->moving ? (float)r->px + (float)(r->x - r->px) * r->prog : (float)r->x;
        float ry = r->moving ? (float)r->py + (float)(r->y - r->py) * r->prog : (float)r->y;
        int sx = (int)lroundf(rx * (float)TILE) - cam_x, sy = (int)lroundf(ry * (float)TILE) - cam_y;
        if (r->active && VISIBLE(sx, sy)) { g_ox = sx; g_oy = sy; draw_rat(r, t_ms); }
    }
    for (int i = 0; i < MAX_EN; i++) {
        const Enemy *e = &enemies[i];
        float ex = e->moving ? (float)e->px + (float)(e->x - e->px) * e->prog : (float)e->x;
        float ey = e->moving ? (float)e->py + (float)(e->y - e->py) * e->prog : (float)e->y;
        int sx = (int)lroundf(ex * (float)TILE) - cam_x, sy = (int)lroundf(ey * (float)TILE) - cam_y;
        if (e->active && VISIBLE(sx, sy)) { g_ox = sx; g_oy = sy; draw_enemy(e); }
    }
    for (int i = 0; i < MAX_PROJ; i++) {
        const Proj *p = &projs[i];
        int sx = (int)lroundf(p->wx * (float)TILE) - cam_x, sy = (int)lroundf(p->wy * (float)TILE) - cam_y;
        if (p->active && VISIBLE(sx, sy)) { g_ox = sx; g_oy = sy; draw_bubble(p); }
    }
    draw_player();

    for (int i = 0; i < MAX_POP; i++) {
        const Popup *p = &popups[i];
        if (!p->active) continue;
        int w = (int)st7789_text_width(p->text, 1);
        int sx = p->x * TILE - cam_x + TILE / 2 - w / 2;
        int sy = p->y * TILE - cam_y - (int)((1.0f - p->life) * 30.0f * (float)TILE / 64.0f);
        if (sx < 0) sx = 0;
        if (sx + w > VIEW_W) sx = VIEW_W - w;
        if (sy < 0 || sy + 7 > VIEW_H) continue;
        renderer_draw_text((uint16_t)sx, (uint16_t)sy, p->text, p->color, COLOR_BLACK, 1);
    }
#undef VISIBLE
}

/* ---------------------------------------------------------
 * Panel lateral: brujula, medidores y contadores
 * --------------------------------------------------------- */
#define BAR_Y 68
#define BAR_H 90
#define LINE_Y0 176

static int c_arrow, c_dist, c_meter[3], c_score, c_steps, c_items, c_ammo;

static uint16_t meter_color(int i, int val, int st)
{
    static const uint16_t base[3] = { HEX(0x0ea5e9), HEX(0x10b981), HEX(0xf43f5e) };
    if (st == 0) return base[i];
    if (st == 2) return COLOR_WHITE;
    if (i < 2) return val <= 25 ? HEX(0xf87171) : HEX(0xfbbf24);
    return val >= 75 ? HEX(0xfb7185) : HEX(0x38bdf8);
}

static void draw_meter(int i, int val, int st)
{
    int cx = PANEL_X + 2 + i * 18;
    int bx = cx + 3;
    char buf[16];
    renderer_fill_rect((uint16_t)(bx - 1), BAR_Y - 1, 13, BAR_H + 2, C_PANEL_LN);
    renderer_fill_rect((uint16_t)bx, BAR_Y, 11, BAR_H, HEX(0x111116));
    int h = val * BAR_H / 100;
    if (h > 0) renderer_fill_rect((uint16_t)bx, (uint16_t)(BAR_Y + BAR_H - h), 11, (uint16_t)h, meter_color(i, val, st));
    for (int x = 0; x < 11; x += 4) renderer_fill_rect((uint16_t)(bx + x), BAR_Y + BAR_H / 2, 2, 1, HEX(0xfde047));
    snprintf(buf, sizeof(buf), "%-3d", val);
    static const uint16_t vcol[3] = { HEX(0x7dd3fc), HEX(0x6ee7b7), HEX(0xfda4af) };   /* igual que las etiquetas */
    renderer_draw_text((uint16_t)cx, BAR_Y + BAR_H + 4, buf, vcol[i], C_PANEL_BG, 1);
}

static void panel_init(void)
{
    static const char *const labels[3] = { "EGO", "PAR", "HIP" };
    static const uint16_t lcol[3] = { HEX(0x7dd3fc), HEX(0x6ee7b7), HEX(0xfda4af) };
    renderer_fill_rect(PANEL_X, 0, PANEL_W, VIEW_H, C_PANEL_BG);
    renderer_fill_rect(PANEL_X, 0, 1, VIEW_H, C_PANEL_LN);
    draw_text_c(PANEL_X, PANEL_W, 3, "SALIDA", 1, HEX(0xa1a1aa), C_PANEL_BG);
    for (int i = 0; i < 3; i++) renderer_draw_text((uint16_t)(PANEL_X + 2 + i * 18), 58, labels[i], lcol[i], C_PANEL_BG, 1);
    c_arrow = c_dist = c_score = c_steps = c_items = c_ammo = -1;
    c_meter[0] = c_meter[1] = c_meter[2] = -1;
    msg_dirty = true;
}

static void draw_compass(void)
{
    int dx = (exit_x > pl.x) - (exit_x < pl.x), dy = (exit_y > pl.y) - (exit_y < pl.y);
    int dist = (abs(exit_x - pl.x) + abs(exit_y - pl.y)) * 3;
    if (c_arrow != dx * 3 + dy) {
        int cx = PANEL_X + PANEL_W / 2, cy = 27;
        c_arrow = dx * 3 + dy;
        renderer_fill_rect(PANEL_X + 4, 12, PANEL_W - 5, 30, C_PANEL_BG);
        for (int k = -5; k <= 3; k++)
            renderer_fill_rect((uint16_t)(cx + dx * k * 2 - 1), (uint16_t)(cy + dy * k * 2 - 1), 3, 3, C_AMBER);
        renderer_fill_rect((uint16_t)(cx + dx * 10 - 3), (uint16_t)(cy + dy * 10 - 3), 7, 7, C_AMBER);
    }
    if (c_dist != dist) {
        char buf[12];
        c_dist = dist;
        renderer_fill_rect(PANEL_X + 1, 44, PANEL_W - 1, 9, C_PANEL_BG);
        snprintf(buf, sizeof(buf), "%dm", dist);
        draw_text_c(PANEL_X, PANEL_W, 45, buf, 1, HEX(0xfde68a), C_PANEL_BG);
    }
}

static void update_panel(uint32_t t_ms)
{
    const int vals[3] = { ego, love, hypo };
    const bool danger[3] = { ego <= 25 || ego >= 85, love <= 25 || love >= 85, hypo >= 75 || hypo <= 25 };
    const int blink = (int)((t_ms / 250u) & 1u);
    char buf[16];

    draw_compass();
    for (int i = 0; i < 3; i++) {
        int st = danger[i] ? (blink ? 2 : 1) : 0;
        int key = vals[i] * 3 + st;
        if (c_meter[i] != key) { c_meter[i] = key; draw_meter(i, vals[i], st); }
    }
    if (c_score != (int)score) {
        c_score = (int)score;
        snprintf(buf, sizeof(buf), "PTS %-5u", (unsigned)score);
        renderer_draw_text(PANEL_X + 3, LINE_Y0, buf, COLOR_YELLOW, C_PANEL_BG, 1);
    }
    if (c_steps != steps) {
        c_steps = steps;
        snprintf(buf, sizeof(buf), "PAS %-5d", steps);
        renderer_draw_text(PANEL_X + 3, LINE_Y0 + 12, buf, HEX(0xfcd34d), C_PANEL_BG, 1);
    }
    if (c_items != items) {
        c_items = items;
        snprintf(buf, sizeof(buf), "ITM %-5d", items);
        renderer_draw_text(PANEL_X + 3, LINE_Y0 + 24, buf, HEX(0x6ee7b7), C_PANEL_BG, 1);
    }
    if (c_ammo != ammo) {
        c_ammo = ammo;
        snprintf(buf, sizeof(buf), "CIT %-5d", ammo);
        renderer_draw_text(PANEL_X + 3, LINE_Y0 + 36, buf, HEX(0x7dd3fc), C_PANEL_BG, 1);
    }
}

/* ---------------------------------------------------------
 * Pantallas: titulo y fin de partida
 * --------------------------------------------------------- */

static void draw_title_screen(uint32_t t_ms)
{
    renderer_clear(COLOR_BLACK);
    draw_text_c(0, SCR_W, 8,  "MANHATTAN ARCADE", 2, COLOR_WHITE, COLOR_BLACK);
    draw_text_c(0, SCR_W, 30, "LABERINTO NEUROTICO", 2, C_AMBER, COLOR_BLACK);
    draw_text_c(0, SCR_W, 54, "una comedia interactiva", 1, HEX(0xa1a1aa), COLOR_BLACK);
    renderer_fill_rect(20, 70, SCR_W - 40, 1, C_PANEL_LN);

    /* --- Skyline a todo el ancho --- */
    const int sky_y0 = 78;
    const int sky_h  = 108;
    renderer_fill_rect(0, sky_y0, SCR_W, sky_h, HEX(0x0b1220));

    struct { int x, y, w, h; } bld[] = {
        {  4, 135, 20, 51 },
        { 26, 115, 18, 71 },
        { 46, 100, 26, 86 },
        { 74, 122, 16, 64 },
        { 92,  90, 22, 96 },
        {116, 108, 24, 78 },
        {142,  95, 18, 91 },
        {162, 118, 22, 68 },
        {186, 105, 20, 81 },
        {208, 125, 18, 61 },
        {228, 112, 24, 74 },
        {254, 130, 16, 56 },
        {272, 120, 20, 66 },
        {294, 138, 22, 48 },
    };
    for (int i = 0; i < (int)(sizeof(bld)/sizeof(bld[0])); i++) {
        int bx = bld[i].x, by = bld[i].y, bw = bld[i].w, bh = bld[i].h;
        renderer_fill_rect(bx, by, bw, bh, HEX(0x1e293b));
        for (int wy = by + 4; wy < by + bh - 6; wy += 7) {
            for (int wx = bx + 3; wx < bx + bw - 3; wx += 5) {
                if (((wx * 13 + wy * 7) & 3) != 0)
                    renderer_fill_rect(wx, wy, 2, 3, HEX(0xfbbf24));
            }
        }
        if (bh > 88) {
            renderer_fill_rect(bx + bw/2 - 1, by - 7, 2, 7, HEX(0x64748b));
            renderer_fill_rect(bx + bw/2 - 2, by - 8, 4, 2, HEX(0x94a3b8));
        }
    }

    /* Agua / reflejo */
    renderer_fill_rect(0, sky_y0 + sky_h - 10, SCR_W, 10, HEX(0x0f172a));
    for (int i = 0; i < 16; i++) {
        int rx = 8 + i * 20 + ((t_ms / 80) % 3);
        renderer_fill_rect(rx, sky_y0 + sky_h - 7, 9, 2, HEX(0x1e3a5f));
    }

    /* Estatua de la Libertad esquemática (derecha) */
    {
        const int sx = 278;
        const int sy = 92;
        renderer_fill_rect(sx + 6,  sy + 70, 28, 14, HEX(0x4b5563));
        renderer_fill_rect(sx + 2,  sy + 82, 36,  8, HEX(0x374151));
        renderer_fill_rect(sx + 10, sy + 66, 20,  6, HEX(0x6b7280));
        renderer_fill_rect(sx + 10, sy + 28, 20, 40, HEX(0x4ade80));
        renderer_fill_rect(sx + 8,  sy + 36,  6, 30, HEX(0x22c55e));
        renderer_fill_rect(sx + 26, sy + 36,  6, 30, HEX(0x22c55e));
        renderer_fill_rect(sx + 12, sy + 16, 16, 14, HEX(0x4ade80));
        renderer_fill_rect(sx + 14, sy + 20,  4,  4, HEX(0x14532d));
        renderer_fill_rect(sx + 22, sy + 20,  4,  4, HEX(0x14532d));
        renderer_fill_rect(sx + 10, sy + 12, 4, 6, HEX(0x86efac));
        renderer_fill_rect(sx + 16, sy +  8, 4, 8, HEX(0x86efac));
        renderer_fill_rect(sx + 22, sy + 12, 4, 6, HEX(0x86efac));
        renderer_fill_rect(sx + 28, sy + 14, 3, 5, HEX(0x86efac));
        renderer_fill_rect(sx + 28, sy + 22, 6, 20, HEX(0x4ade80));
        renderer_fill_rect(sx + 30, sy + 14, 4, 10, HEX(0x22c55e));
        renderer_fill_rect(sx + 29, sy +  6, 6, 10, HEX(0xfbbf24));
        renderer_fill_rect(sx + 31, sy +  2, 2,  6, HEX(0xfde047));
    }

    /* --- Personajes a pie de edificios --- */
    float t = (float)t_ms * 0.0022f;
    Enemy tmp = {0};

    #define DRAW_NAME(ox, oy, name) do { \
        int nw = (int)st7789_text_width(name, 1); \
        int nx = (ox) + (TILE - nw) / 2; \
        if (nx < 0) nx = 0; \
        renderer_draw_text((uint16_t)nx, (uint16_t)((oy) + TILE - 2), name, HEX(0xfde68a), HEX(0x0b1220), 1); \
    } while (0)

    /* Woody */
    {
        int ox = 100 + (int)(cosf(t * 0.9f) * 5.0f);
        int oy = 128 + (int)(sinf(t * 1.1f) * 4.0f);
        int swing = (int)(sinf(t * 2.4f) * 4.0f);
        g_ox = ox; g_oy = oy;
        draw_woody_sprite(false, swing, 0);
        DRAW_NAME(ox, oy, "WOODY");
    }

    /* Mama */
    {
        int ox =  22 + (int)(cosf(t * 0.7f + 1.0f) * 5.0f);
        int oy = 136 + (int)(sinf(t * 1.3f + 0.5f) * 4.0f);
        g_ox = ox; g_oy = oy;
        tmp.type = EN_MOTHER;
        draw_enemy(&tmp);
        DRAW_NAME(ox, oy, "MAMA");
    }

    /* Analista */
    {
        int ox =  58 + (int)(cosf(t * 0.85f + 2.2f) * 5.0f);
        int oy = 142 + (int)(sinf(t * 1.0f + 1.8f) * 4.0f);
        g_ox = ox; g_oy = oy;
        tmp.type = EN_ANALYST;
        draw_enemy(&tmp);
        DRAW_NAME(ox, oy, "ANALISTA");
    }

    /* Critico */
    {
        int ox = 140 + (int)(cosf(t * 0.75f + 3.5f) * 5.0f);
        int oy = 134 + (int)(sinf(t * 1.2f + 2.7f) * 4.0f);
        g_ox = ox; g_oy = oy;
        tmp.type = EN_CRITIC;
        draw_enemy(&tmp);
        DRAW_NAME(ox, oy, "CRITICO");
    }

    /* Guapa */
    {
        int ox = 178 + (int)(cosf(t * 0.95f + 4.8f) * 5.0f);
        int oy = 140 + (int)(sinf(t * 1.15f + 3.9f) * 4.0f);
        g_ox = ox; g_oy = oy;
        tmp.type = EN_PRETTY;
        draw_enemy(&tmp);
        DRAW_NAME(ox, oy, "GUAPA");
    }

    /* Nuevo personaje a la derecha de Guapa */
    {
        int ox = 218 + (int)(cosf(t * 0.8f + 5.5f) * 5.0f);
        int oy = 136 + (int)(sinf(t * 1.25f + 4.4f) * 4.0f);
        g_ox = ox; g_oy = oy;
        tmp.type = EN_DIRECTOR;
        draw_enemy(&tmp);
        DRAW_NAME(ox, oy, "DIRECTOR");
    }

    #undef DRAW_NAME

    renderer_fill_rect(20, 190, SCR_W - 40, 1, C_PANEL_LN);
}

/* Blink mejorado: el texto permanece visible y solo cambia de brillo */
static void draw_title_blink(bool on)
{
   // renderer_fill_rect(0, 196, SCR_W, 20, COLOR_BLACK);
    uint16_t col = on ? COLOR_YELLOW : HEX(0xa16207);   /* amarillo brillante / ámbar apagado */
    draw_text_c(0, SCR_W, 200, "PULSA A PARA EMPEZAR", 2, col, COLOR_BLACK);
}


static void draw_end_overlay(void)
{
    const EndInfo *ei = &END_INFO[end_reason];
    char lines[3][64];
    char buf[48];
    const int bx = 10, by = 30, bw = VIEW_W - 20, bh = 160;
    renderer_fill_rect(bx - 2, by - 2, bw + 4, bh + 4, end_reason == 0 ? HEX(0x16a34a) : HEX(0xdc2626));
    renderer_fill_rect(bx, by, bw, bh, COLOR_BLACK);
    draw_text_c(bx, bw, by + 8, ei->sub, 1, C_AMBER, COLOR_BLACK);
    int sc = (int)st7789_text_width(ei->title, 2) <= bw - 8 ? 2 : 1;
    draw_text_c(bx, bw, by + 24, ei->title, sc, COLOR_WHITE, COLOR_BLACK);
    int n = wrap_text(ei->quote, 38, lines, 3);
    for (int i = 0; i < n; i++) draw_text_c(bx, bw, by + 54 + i * 11, lines[i], 1, HEX(0xd4d4d8), COLOR_BLACK);
    snprintf(buf, sizeof(buf), "PASOS %d  OBJETOS %d", steps, items);
    draw_text_c(bx, bw, by + 100, buf, 1, HEX(0xa1a1aa), COLOR_BLACK);
    snprintf(buf, sizeof(buf), "PUNTOS %u", (unsigned)score);
    draw_text_c(bx, bw, by + 116, buf, 2, COLOR_YELLOW, COLOR_BLACK);
}

static void draw_end_blink(bool on)
{
    renderer_fill_rect(20, 30 + 138, VIEW_W - 40, 10, COLOR_BLACK);
    if (on) draw_text_c(10, VIEW_W - 20, 30 + 139, "PULSA A", 1, COLOR_WHITE, COLOR_BLACK);
}

/* ---------------------------------------------------------
 * Bucle principal
 * --------------------------------------------------------- */
static void read_input(void)
{
    int jx = controls_debug_axis_normalized(0);
    int jy = controls_debug_axis_normalized(1);
    if (MH_INVERT_X) jx = -jx;
    if (MH_INVERT_Y) jy = -jy;
    int ax = abs(jx), ay = abs(jy);
    in_dir = -1;
    if (ax >= MH_STICK_THR || ay >= MH_STICK_THR) {
        if (ax >= ay) in_dir = jx > 0 ? 3 : 2;
        else in_dir = jy > 0 ? 1 : 0;
    }
    in_jump  = controls_button_pressed(BTN_JUMP);
    in_quote = controls_button_pressed(BTN_QUOTE);
    in_start = in_jump || in_quote;
    in_quit  = controls_button_pressed(BTN_IDX_J1_SW) || controls_button_pressed(BTN_IDX_J2_SW);
}

void game_manhattan_run(game_mode_t mode)
{
    g_demo = (mode == GAME_MODE_DEMO);
    g_done = false;
    rng_state ^= (uint32_t)now_us();
    if (!rng_state) rng_state = 1;

    st7789_set_rotation(MH_ROTATION);
    state = ST_TITLE;
    title_drawn = false;
    scores_drawn = false;
    msg_active = false;

    const TickType_t period_ticks = pdMS_TO_TICKS(1000 / TARGET_FPS);
    TickType_t last_wake = xTaskGetTickCount();
    uint64_t last = now_us();
    uint64_t t0 = last;
    int blink_prev = -1;

    while (!g_done) {
        uint64_t now = now_us();
        float dt = (float)(now - last) / 1000.0f;
        if (dt > 100.0f) dt = 100.0f;
        last = now;
        uint32_t t_ms = (uint32_t)((now - t0) / 1000);

        controls_update();
        sound_update();
        read_input();
        if (in_quit) break;

         if (state == ST_TITLE) {
         draw_title_screen(t_ms);

    /* Ciclo de 800 ms: brillante ~65 % del tiempo */
    int phase = (int)(t_ms % 800u);
    bool on = (phase < 520);          // visible más tiempo
    draw_title_blink(on);

    if (g_demo) {
        if (t_ms > 5000 || in_start) break;
            } else if (in_start) {
           sound_effect_select();
           renderer_clear(COLOR_BLACK);
           panel_init();
           new_game();
           }
        } else if (state == ST_PLAY) {
            update_game(dt);
            if (state == ST_PLAY) {
                if (++ego_decay_cnt >= 100) {   // ≈ 2,5 s a 60 FPS
                    ego_decay_cnt = 0;
                    if (ego > 0) {
                    ego -= 1;
                    stats_changed();
                    }
                }
                render_game((int)t_ms);
                update_panel(t_ms);
                if (msg_dirty) { draw_strip(); msg_dirty = false; }
            }
        } else if (state == ST_END) {
            end_t += dt;
            if (!end_drawn) {
                render_game((int)t_ms);
                update_panel(t_ms);
                draw_end_overlay();
                end_drawn = true;
                blink_prev = -1;
            }
            int b = (int)((t_ms / 400u) & 1u);
            if (end_t > 1500.0f && b != blink_prev) { blink_prev = b; draw_end_blink(b == 0); }
            if (end_t > 1500.0f && in_start) {
                sound_stop_scramble_victory();
                if (!g_demo && highscores_is_top(MH_GAME_ID, score)) highscores_enter(MH_GAME_ID, score);
                state = ST_SCORES;
                scores_t = 0.0f;
                scores_drawn = false;
            }
        } else {
            scores_t += dt;
            if (!scores_drawn) {
                renderer_clear(COLOR_BLACK);
                highscores_draw(MH_GAME_ID, "MANHATTAN", 20);
                scores_drawn = true;
            }
            if (scores_t > 8000.0f || (scores_t > 1000.0f && in_start)) g_done = true;
        }

        renderer_flush();
        vTaskDelayUntil(&last_wake, period_ticks ? period_ticks : 1);
    }

    sound_stop_scramble_victory();
    highscores_flush();

    /* Vuelta al menu: rotacion horizontal y entrada limpia. */
    st7789_set_rotation(MENU_ROTATION);
    renderer_clear(COLOR_BLACK);
    renderer_flush();
    for (int k = 0; k < 2; k++) {
        controls_update();
        (void)controls_button_pressed(BTN_JUMP);
        (void)controls_button_pressed(BTN_QUOTE);
        (void)controls_button_pressed(BTN_IDX_J1_SW);
        (void)controls_button_pressed(BTN_IDX_J2_SW);
        (void)controls_get_raw_delta(0);
        (void)controls_get_raw_delta_x(0);
    }
}
