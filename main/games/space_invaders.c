/**
 * space_invaders.c -- portado de ArcadePi (https://github.com/JuanLPerea/ArcadePi),
 * mismo concepto (formación 11x5, bunkers, platillo volante, marcha
 * alienígena, récords), adaptado a ArcadeColor / ESP32.
 *
 * PORT A ESP32 -- igual que asteroids.c/scramble.c: nada de Pico SDK
 * (pico/stdlib.h, absolute_time_t, sleep_us, time_us_32...), sustituido
 * por esp_timer.h + FreeRTOS. Toda la lógica de juego (formación,
 * bunkers, platillo, bombas, HUD) es idéntica a la version Pico y no
 * depende de nada del SDK, así que se porta literal. Lo que cambia:
 *
 *  - Resolución: campo a pantalla casi completa (320x240 apaisada,
 *    igual que Pong) en vez de 768x576 de vídeo compuesto. La
 *    formación original (32x20px por alien) no cabe ni de lejos aquí
 *    -- se reduce a 14x8px, manteniendo las 11 columnas.
 *  - Tiempo real: los 3 temporizadores del juego (paso de formación,
 *    aparición del platillo, velocidad de bombas) usaban
 *    absolute_time_t/make_timeout_ms()/time_reached_ms() del SDK de
 *    Pico -- aquí se sustituyen por marcas de tiempo uint32_t en
 *    milisegundos vía esp_timer_get_time() (ver now_ms()/
 *    time_reached_ms() más abajo, mismo patrón now_ms() que
 *    asteroids.c), sin cambiar ni la lógica ni los valores de
 *    intervalo -- el comentario original de formation_interval_ms()
 *    ya explicaba por qué esto tenía que ser tiempo real y no cuentas
 *    de tick, y eso sigue aplicando igual en ESP32.
 *  - Bucle a ritmo fijo con vTaskDelayUntil() (igual que asteroids.c/
 *    scramble.c) en vez de sleep_us(1000): con el tick rate por
 *    defecto de FreeRTOS en ESP-IDF (100Hz) conviene apuntar a un
 *    período concreto con pdMS_TO_TICKS() en vez de un sleep_us fino
 *    que no tiene equivalente preciso. Como los 3 relojes del juego ya
 *    son de tiempo real (ver punto anterior), este período objetivo
 *    solo evita que el bucle corra sin descanso -- no afecta al ritmo
 *    real de formación/platillo/bombas.
 *  - Controles: controls_get_raw_delta_x(0) para mover la nave -- OJO,
 *    esto es un cambio real de eje, no solo de nombre. La version Pico
 *    usaba controls_get_raw_delta(0) (un único encoder, sin ejes X/Y
 *    que distinguir); en el controls.h de ESP32 ese mismo índice 0 ES
 *    el eje Y del joystick 1, así que dejarlo tal cual habría hecho
 *    que la nave se moviera inclinando el stick arriba/abajo en vez de
 *    izquierda/derecha. Para un movimiento horizontal de pala (mismo
 *    caso que Breakout, según la propia documentación de controls.h)
 *    el eje correcto es controls_get_raw_delta_x(0) -- mismo ajuste
 *    que se hizo para el empuje horizontal de scramble.c. El
 *    acumulador enc_acc (steps = enc_acc/2) no cambia: controls.h ya
 *    documenta que estas funciones devuelven un valor analógico
 *    pensado para integrarse con el mismo sistema de aceleración/
 *    inercia que usaban los encoders, así que la división entre 2
 *    sigue teniendo sentido igual.
 *  - Disparo: controls_menu_select() (cualquiera de los 6 botones), se
 *    mantiene igual que el original -- no es un botón "de menú" mal
 *    reutilizado, es la misma función que ya usan las demás pantallas
 *    (título, game over, scores) para confirmar, y aquí simplemente
 *    dispara. Si prefieres un botón dedicado (como BTN_IDX_J1_A en
 *    asteroids.c/scramble.c) en vez de "cualquier botón", es un cambio
 *    de una línea -- dímelo y lo ajusto.
 *  - Récords: highscores_enter() bloqueante (de highscores.c) en
 *    vez de la máquina de estados SI_ENTER_NAME/hs_input no
 *    bloqueante del original.
 *  - Sonido: sound.c no tiene los efectos específicos del original
 *    (SFX_SI_LASER, MUSIC_SAUCER...) -- se mapean a los que sí
 *    tenemos (sound_effect_shoot/select/explosion/success/game_over,
 *    sound_play_tone para las 4 notas de la marcha). La sirena
 *    continua del platillo se simplifica a un aviso puntual al
 *    aparecer.
 *  - Render incremental: nada de limpiar toda la pantalla cada
 *    frame. La formación se trata como UN bloque (se borra el
 *    rectángulo que envuelve la formación anterior y se redibujan
 *    todos los vivos de una vez, con un único flush) porque se
 *    mueve entera de golpe; bala/bombas/nave/platillo llevan cada
 *    uno su propio borrado incremental + flush, como la bola/palas
 *    de Pong.
 *  - MAX_BOMBS reducido de 10 a 3: con flush por objeto, 10 bombas
 *    simultáneas dispersas por la pantalla dispararían el número de
 *    transmisiones SPI por tick.
 *  - Bucle propio: game_space_invaders_run(mode) contiene su propio
 *    bucle, como el resto de juegos de ArcadeColor (nada de
 *    callbacks de dibujo/tick registrados sobre un bucle común).
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "space_invaders.h"
#include "../renderer.h"
#include "../controls.h"
#include "../highscores.h"
#include "../sound.h"

// Ritmo objetivo del bucle principal (vTaskDelayUntil en
// game_space_invaders_run) -- ver nota de cabecera: los relojes de
// formación/platillo/bombas ya son de tiempo real, así que esto solo
// evita que el bucle corra sin descanso.
#define TARGET_FPS 60

// Máximo de ticks de lógica que se recuperan de una vez cuando el
// dibujado va lento (ver bucle principal). 8 ticks = 80 ms de retraso.
#define SI_MAX_CATCHUP 8

// Tiempo real en ms desde el arranque -- sustituye a
// absolute_time_t/get_absolute_time() del SDK de Pico (mismo patrón
// que asteroids.c/scramble.c).
static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// Sustituye a make_timeout_time_ms(ms) del SDK de Pico: una marca de
// tiempo "en el futuro" a partir de ahora.
static inline uint32_t make_timeout_ms(uint32_t ms) { return now_ms() + ms; }

// Sustituye a time_reached(t) del SDK de Pico: true si ya se alcanzó
// o pasó esa marca de tiempo. Resta con signo (en vez de
// now_ms() >= deadline a secas) para que siga funcionando
// correctamente si now_ms() da la vuelta al desbordar los 32 bits
// (~49 días de uptime).
static inline bool time_reached_ms(uint32_t deadline) { return (int32_t)(now_ms() - deadline) >= 0; }

// ---------------------------------------------------------------------------
// Medición de tiempos (diagnóstico). Pon SI_PROFILE a 1 para que cada
// segundo se imprima por el monitor serie el TIEMPO MÁXIMO (en us) que
// ha tardado cada parte del frame durante ese segundo. Con 0 no
// compila nada de esto y no cuesta nada.
// ---------------------------------------------------------------------------
#define SI_PROFILE 0

#if SI_PROFILE
enum { P_FORM, P_BULLET, P_BOMBS, P_SAUCER, P_PLAYER, P_HUD, P_TICK, P_SOUND, P_WORK, P_N };
static uint32_t prof_max[P_N];
static uint32_t prof_over;   // frames cuyo trabajo superó el período del bucle
#define PROF(slot, call) do {                                         \
        int64_t _t0 = esp_timer_get_time();                           \
        call;                                                         \
        uint32_t _d = (uint32_t)(esp_timer_get_time() - _t0);         \
        if (_d > prof_max[slot]) prof_max[slot] = _d;                 \
    } while (0)
#else
#define PROF(slot, call) call
#endif

// ---------------------------------------------------------------------------
// Área de juego -- literales fijos (no TFT_WIDTH/TFT_HEIGHT): son en
// realidad variables en tiempo de ejecución y no sirven como
// inicializador de un array static const (ver el mismo comentario,
// más largo, en pong.c). La rotación está fijada a 320x240 y no
// cambia en marcha.
// ---------------------------------------------------------------------------
#define SCREEN_W 320
#define SCREEN_H 240

#define PLAY_X   4
#define PLAY_Y   3
#define PLAY_W   (SCREEN_W - 2 * PLAY_X)   // 312
#define PLAY_H   (SCREEN_H - 2 * PLAY_Y)   // 234
#define CX       (PLAY_X + PLAY_W / 2)
#define CY       (PLAY_Y + PLAY_H / 2)

// ---------------------------------------------------------------------------
// Dimensiones -- ver cabecera del archivo sobre el reescalado
// ---------------------------------------------------------------------------
#define ALIEN_COLS      6
#define ALIEN_ROWS      4
#define ALIEN_W        32   // tamaño ORIGINAL -- con solo 6 columnas ya cabe de sobra
#define ALIEN_H        20   // tamaño ORIGINAL
#define ALIEN_PADX      6
#define ALIEN_PADY      6
#define ALIEN_STEP_X    3
#define ALIEN_STEP_DOWN 10
#define ALIEN_START_X  (PLAY_X + 6)
#define ALIEN_START_Y  (PLAY_Y + 24)

#define PLAYER_W       28
#define PLAYER_H       14
#define PLAYER_Y       (PLAY_Y + PLAY_H - 20)
#define PLAYER_SPD      2

#define BULLET_W        3
#define BULLET_H       10
#define BULLET_SPD      3

#define BOMB_W          3
#define BOMB_H         10
#define BOMB_SPD        1
#define MAX_BOMBS       3    // reducido de 10 (ver cabecera)

#define BUNKER_COUNT    3
#define BUNKER_CELL_W   5
#define BUNKER_CELL_H   3
#define BUNKER_CELLS_X  6
#define BUNKER_CELLS_Y  7
#define BUNKER_W       (BUNKER_CELLS_X * BUNKER_CELL_W)   // 30
#define BUNKER_H       (BUNKER_CELLS_Y * BUNKER_CELL_H)   // 21
#define BUNKER_Y       (PLAYER_Y - BUNKER_H - 10)

#define LIVES_MAX       3
#define TICKS_S        60   // referencia nominal para contadores de tiempo (ver nota en pong.c sobre PAUSE_TICKS)

// Colores por tipo de alien (fila -> tipo), en vez del pixel-art
// detallado del original
#define COLOR_ALIEN_A COLOR_MAGENTA   // fila 0 -- 4 pts
#define COLOR_ALIEN_B COLOR_CYAN      // filas 1-2 -- 3 pts
#define COLOR_ALIEN_C COLOR_GREEN     // filas 3-4 -- 1 pt

// Puntuación por tipo de alien (fila -> tipo)
static const int ALIEN_PTS[ALIEN_ROWS] = { 4, 3, 3, 1 };

// ---------------------------------------------------------------------------
// Platillo volante
// ---------------------------------------------------------------------------
#define SAUCER_W        18
#define SAUCER_H         7
#define SAUCER_Y        (PLAY_Y + 22)
#define SAUCER_SPD       1
// El platillo se mueve SAUCER_SPD px por tick de lógica, pero solo se
// redibuja (borrar + dibujar + flush) cuando se ha desplazado al menos
// esto: un flush por cada píxel era un coste enorme para un objeto
// pequeño y rápido.
#define SAUCER_DRAW_STEP 2
#define SAUCER_MIN_PTS  50
#define SAUCER_MAX_PTS 300

// Intervalo mínimo entre notas de marcha (en ticks de juego), para
// que a alta velocidad de formación las notas no se superpongan.
#define MARCH_MIN_TICKS  8

// ---------------------------------------------------------------------------
// Estado interno
// ---------------------------------------------------------------------------
typedef enum {
    SI_SELECT,      // pantalla de inicio
    SI_PLAYING,
    SI_PLAYER_DEAD, // pausa tras morir
    SI_LEVEL_CLEAR, // pausa entre oleadas
    SI_GAME_OVER,
    SI_SCORES
} SIState;

typedef struct {
    int  x, y;
    bool alive;
    int  anim;      // 0 ó 1 -- alterna cada paso de formación
    // Lo que hay REALMENTE dibujado en pantalla para este alien, para
    // poder borrar/redibujar solo los que cambian (y flushear solo su
    // rectángulo) en vez de toda la formación.
    int  dx, dy, danim;
    bool drawn;
} Alien;

typedef struct {
    int  x, y;
    bool active;
} Bomb;

typedef struct {
    int  x, y;
    bool cells[BUNKER_CELLS_Y][BUNKER_CELLS_X];
} Bunker;

typedef struct {
    int  x;
    bool active;
    int  dir;          // +1 izquierda->derecha, -1 derecha->izquierda
    int  pts;
    int  explode_cnt;  // >0: mostrando puntos tras impacto
    int  pts_display;
} Saucer;

static uint32_t next_bomb_step;
#define BOMB_INTERVAL_MS  15  // Velocidad constante de bajada (a mayor número, más lentas)

static SIState state;
static bool    demo;
static int     pause_cnt;

// Formación
static Alien   aliens[ALIEN_ROWS][ALIEN_COLS];
static int     fdir;
static uint32_t next_formation_step; // tiempo real, no contador de ticks (ver formation_interval_ms)
static int     falive;
static int     fanim_phase;

// Jugador
static int     px;
static bool    bullet_active;
static int     bx, by;
static int     enc_acc;

// Bombas
static Bomb    bombs[MAX_BOMBS];

// Bunkers
static Bunker  bunkers[BUNKER_COUNT];

// Puntuación y vidas
static int     score;
static int     lives;
static int     level;
static int     dead_cnt;
static int     clear_cnt;

static int     blink;

static Saucer  saucer;
static uint32_t next_saucer_spawn; // tiempo real, mismo arreglo que next_formation_step

static int     march_beat;
static int     march_ticks_since;

static int     demo_fire_cnt;
static int     demo_ticks;

static bool    g_done;

// Si es false, draw_playing_frame() no hace nada: se usa para ejecutar
// varios ticks de lógica seguidos (recuperar retraso) y dibujar solo tras
// el último. Ver el bucle principal.
static bool    si_render = true;

static int clamp(int v, int lo, int hi) { return v<lo?lo:v>hi?hi:v; }

// ---------------------------------------------------------------------------
// Rastro para el borrado incremental (ver cabecera del archivo)
// ---------------------------------------------------------------------------
static int  prev_player_x = -1;
static bool prev_bullet_active = false;
static int  prev_bullet_x, prev_bullet_y;
static bool prev_bomb_active[MAX_BOMBS];
static int  prev_bomb_x[MAX_BOMBS], prev_bomb_y[MAX_BOMBS];
static bool prev_saucer_active = false;
static int  prev_saucer_x;
static bool prev_saucer_pts_shown = false;
static int  prev_score = -1, prev_lives = -1, prev_level = -1;
static char prev_bottom_msg[40] = "";
static bool field_needs_redraw = true;

// La formación solo se redibuja cuando algo la ha cambiado (paso de
// formación, alien muerto, platillo pasando por encima...). Antes se
// borraba y repintaba entera en CADA tick -- ~300 fill_rect + un flush
// SPI enorme por tick, que es lo que ralentizaba el juego.
static bool formation_dirty = true;
// La nave hay que repintarla aunque no se haya movido si una bala/bomba
// ha borrado (al limpiar su rastro) parte del cañón.
static bool player_dirty = false;

static void reset_render_trace(void) {
    formation_dirty = true;
    player_dirty = false;
    for (int r = 0; r < ALIEN_ROWS; r++)
        for (int c = 0; c < ALIEN_COLS; c++)
            aliens[r][c].drawn = false;   // nada dibujado todavía
    prev_player_x = -1;
    prev_bullet_active = false;
    for (int i = 0; i < MAX_BOMBS; i++) prev_bomb_active[i] = false;
    prev_saucer_active = false;
    prev_saucer_pts_shown = false;
    prev_score = prev_lives = prev_level = -1;
    prev_bottom_msg[0] = '\0';
}

// ---------------------------------------------------------------------------
// Helpers de formación / bunkers
// ---------------------------------------------------------------------------

/*
 * Intervalo REAL (ms) entre pasos de formación, según nº de vivos.
 *
 * CORREGIDO: la versión anterior devolvía un número de "ticks del
 * bucle" (fstep_ticks), y se disparaba cuando un contador de ticks
 * (fstep_cnt) lo alcanzaba. El problema: cada vuelta del bucle NO
 * dura lo mismo -- si ese tick además hay que mover la nave (input
 * de encoder) o redibujar el HUD, ese tick tarda más en tiempo real
 * por el flush extra. Contar ticks, con ticks de duración variable,
 * hace que el ritmo real de la formación se acelere o frene según
 * cuánto más esté pasando en pantalla -- justo lo que se notaba
 * correlacionado con mover el encoder. Ahora se agenda en tiempo
 * real (absolute_time_t), independiente de cuántos ticks del bucle
 * pasen o cuánto cueste dibujar cada uno.
 *
 * 24 aliens (formación completa, 6x4) -> ~500 ms; 1 alien -> ~60 ms.
 * (Antes la fórmula estaba calibrada para hasta 55 aliens; con solo
 * 24 ahora, recalibrada para el mismo tipo de curva de aceleración.)
 */
static int formation_interval_ms(int alive) {
    if (alive <= 1) return 20; // Máxima velocidad absoluta para el último alien

    int total_aliens = ALIEN_ROWS * ALIEN_COLS;
    if (total_aliens <= 1) return 20;

    int min_ms = 20; // Velocidad máxima (intervalo mínimo)
    
    // El nivel hace que empiece más lento (valor alto) en el nivel 1 
    // y que la velocidad inicial vaya siendo cada vez menor (más rápido) en niveles superiores.
    int max_ms = 280 - (level - 1) * 20;
    if (max_ms < 90) max_ms = 90; // Tope para que en niveles muy altos no sea injugable de salida

    // Usamos una progresión cúbica para la curva de aceleración por enemigos vivos
    long long numerator = (long long)alive * alive * alive * (max_ms - min_ms);
    long long denominator = (long long)total_aliens * total_aliens * total_aliens;
    
    int ms = min_ms + (int)(numerator / denominator);

    if (ms < min_ms) ms = min_ms;
    if (ms > max_ms) ms = max_ms;

    return ms;
}

static void formation_bounds(int *x_min, int *y_min, int *x_max, int *y_max) {
    *x_min = PLAY_X + PLAY_W; *y_min = PLAY_Y + PLAY_H;
    *x_max = PLAY_X;          *y_max = PLAY_Y;
    for (int r = 0; r < ALIEN_ROWS; r++)
        for (int c = 0; c < ALIEN_COLS; c++)
            if (aliens[r][c].alive) {
                if (aliens[r][c].x < *x_min) *x_min = aliens[r][c].x;
                if (aliens[r][c].y < *y_min) *y_min = aliens[r][c].y;
                if (aliens[r][c].x + ALIEN_W > *x_max) *x_max = aliens[r][c].x + ALIEN_W;
                if (aliens[r][c].y + ALIEN_H > *y_max) *y_max = aliens[r][c].y + ALIEN_H;
            }
}

static int formation_bottom(void) {
    int ymax = PLAY_Y;
    for (int r = 0; r < ALIEN_ROWS; r++)
        for (int c = 0; c < ALIEN_COLS; c++)
            if (aliens[r][c].alive && aliens[r][c].y + ALIEN_H > ymax)
                ymax = aliens[r][c].y + ALIEN_H;
    return ymax;
}

static void init_formation(void) {
    int ox = ALIEN_START_X;
    int oy = ALIEN_START_Y;
    for (int r = 0; r < ALIEN_ROWS; r++)
        for (int c = 0; c < ALIEN_COLS; c++) {
            aliens[r][c].x     = ox + c * (ALIEN_W + ALIEN_PADX);
            aliens[r][c].y     = oy + r * (ALIEN_H + ALIEN_PADY);
            aliens[r][c].alive = true;
            aliens[r][c].anim  = 0;
        }
    fdir        = 1;
    falive      = ALIEN_ROWS * ALIEN_COLS;
    fanim_phase = 0;
    next_formation_step = make_timeout_ms(formation_interval_ms(falive));
    for (int r = 0; r < ALIEN_ROWS; r++)
        for (int c = 0; c < ALIEN_COLS; c++)
            aliens[r][c].drawn = false;   // fuerza dibujado completo la próxima vez
    formation_dirty = true;
}

static void init_bunkers(void) {
    int spacing = PLAY_W / (BUNKER_COUNT + 1);
    for (int b = 0; b < BUNKER_COUNT; b++) {
        bunkers[b].x = PLAY_X + spacing * (b + 1) - BUNKER_W / 2;
        bunkers[b].y = BUNKER_Y;
        for (int row = 0; row < BUNKER_CELLS_Y; row++)
            for (int col = 0; col < BUNKER_CELLS_X; col++) {
                bool cut  = (row >= BUNKER_CELLS_Y - 2) &&
                            (col <= 1 || col >= BUNKER_CELLS_X - 2);
                bool hole = (row >= BUNKER_CELLS_Y - 3) &&
                            (col >= 2 && col <= 3);
                bunkers[b].cells[row][col] = !cut && !hole;
            }
    }
}

static void game_start(void) {
    score = 0;
    lives = LIVES_MAX;
    level = 1;
    bullet_active = false;
    for (int i = 0; i < MAX_BOMBS; i++) bombs[i].active = false;
    enc_acc = 0;
    px = CX;
    march_beat = 0;
    march_ticks_since = MARCH_MIN_TICKS;
    saucer.active = false;
    saucer.explode_cnt = 0;
    sound_siren_stop(); // por si veníamos de una partida con el platillo sonando
    next_saucer_spawn = make_timeout_ms(1000 * (8 + rand() % 15)); // 8-22s reales
    init_formation();
    init_bunkers();
    reset_render_trace();
    field_needs_redraw = true;
    next_bomb_step = make_timeout_ms(BOMB_INTERVAL_MS);
}

// Función rápida para drenar inputs acumulados
static void clear_input_buffer(void) {
    controls_menu_select(); // Lee y descarta el estado actual del botón
    controls_get_raw_delta_x(0); // Lee y descarta el delta acumulado del encoder
}


// Aplica una operación sobre las celdas de bunker que solapan un
// rectángulo:
//   draw == false -> ELIMINA esas celdas del modelo (cells[][] = false),
//                    para que dejen de existir también para las balas.
//   draw == true  -> repinta las celdas que sigan vivas (para restaurar
//                    el bunker tras borrar con negro un área que lo pisa).
static void bunkers_rect(int rx, int ry, int rw, int rh, bool draw) {
    for (int b = 0; b < BUNKER_COUNT; b++) {
        int x0 = bunkers[b].x, y0 = bunkers[b].y;
        int ax0 = (rx > x0) ? rx : x0;
        int ay0 = (ry > y0) ? ry : y0;
        int ax1 = (rx + rw < x0 + BUNKER_W) ? rx + rw : x0 + BUNKER_W;
        int ay1 = (ry + rh < y0 + BUNKER_H) ? ry + rh : y0 + BUNKER_H;
        if (ax0 >= ax1 || ay0 >= ay1) continue;

        int c0 = (ax0 - x0) / BUNKER_CELL_W, c1 = (ax1 - 1 - x0) / BUNKER_CELL_W;
        int r0 = (ay0 - y0) / BUNKER_CELL_H, r1 = (ay1 - 1 - y0) / BUNKER_CELL_H;

        for (int row = r0; row <= r1; row++)
            for (int col = c0; col <= c1; col++) {
                if (!bunkers[b].cells[row][col]) continue;
                if (draw)
                    renderer_fill_rect(x0 + col * BUNKER_CELL_W, y0 + row * BUNKER_CELL_H,
                                       BUNKER_CELL_W, BUNKER_CELL_H, COLOR_GREEN);
                else {
                    bunkers[b].cells[row][col] = false;
                    // Borrar también en pantalla: la celda entera, no solo la
                    // parte que tape el alien (si no, quedarían restos).
                    renderer_fill_rect(x0 + col * BUNKER_CELL_W, y0 + row * BUNKER_CELL_H,
                                       BUNKER_CELL_W, BUNKER_CELL_H, COLOR_BLACK);
                }
            }
    }
}

static void formation_step(void) {
    int xmin, ymin, xmax, ymax;
    formation_bounds(&xmin, &ymin, &xmax, &ymax);

    bool hit_right = (xmax + ALIEN_STEP_X * fdir > PLAY_X + PLAY_W - 2);
    bool hit_left  = (xmin + ALIEN_STEP_X * fdir < PLAY_X + 2);

    if ((fdir > 0 && hit_right) || (fdir < 0 && hit_left)) {
        for (int r = 0; r < ALIEN_ROWS; r++)
            for (int c = 0; c < ALIEN_COLS; c++)
                if (aliens[r][c].alive)
                    aliens[r][c].y += ALIEN_STEP_DOWN;
        fdir = -fdir;
    } else {
        for (int r = 0; r < ALIEN_ROWS; r++)
            for (int c = 0; c < ALIEN_COLS; c++)
                if (aliens[r][c].alive)
                    aliens[r][c].x += ALIEN_STEP_X * fdir;
    }

    fanim_phase ^= 1;
    for (int r = 0; r < ALIEN_ROWS; r++)
        for (int c = 0; c < ALIEN_COLS; c++)
            aliens[r][c].anim = fanim_phase;

    formation_dirty = true;

    // Los aliens que alcanzan la altura de los bunkers los destruyen
    // por completo: se eliminan las celdas del modelo (no solo del
    // dibujo), así las balas ya no chocan con bunkers invisibles.
    if (formation_bottom() > BUNKER_Y) {
        for (int r = 0; r < ALIEN_ROWS; r++)
            for (int c = 0; c < ALIEN_COLS; c++)
                if (aliens[r][c].alive)
                    bunkers_rect(aliens[r][c].x, aliens[r][c].y, ALIEN_W, ALIEN_H, false);
    }

    next_formation_step = make_timeout_ms(formation_interval_ms(falive));

    // Marcha: 4 notas descendentes, con el mismo throttle que el
    // original para que a alta velocidad no se solapen.
    static const uint16_t march_freq[4] = { 196, 175, 156, 131 };
    if (march_ticks_since >= MARCH_MIN_TICKS) {
        sound_play_tone(march_freq[march_beat & 3], 60);
        march_beat = (march_beat + 1) & 3;
        march_ticks_since = 0;
    }
}

static void alien_fire(void) {
    int active = 0;
    for (int i = 0; i < MAX_BOMBS; i++) if (bombs[i].active) active++;
    if (active >= MAX_BOMBS) return;

    int col = rand() % ALIEN_COLS;
    int shooter_r = -1;
    for (int r = ALIEN_ROWS - 1; r >= 0; r--)
        if (aliens[r][col].alive) { shooter_r = r; break; }
    if (shooter_r < 0) return;

    for (int i = 0; i < MAX_BOMBS; i++) {
        if (!bombs[i].active) {
            bombs[i].x      = aliens[shooter_r][col].x + ALIEN_W / 2;
            bombs[i].y      = aliens[shooter_r][col].y + ALIEN_H;
            bombs[i].active = true;
            return;
        }
    }
}

// Colisión de una bala (dir = -1, sube) o una bomba (dir = +1, baja)
// con los bunkers. Se revisan TODAS las celdas vivas que solapa el
// rectángulo (no solo la de la esquina), empezando por el borde
// delantero, y se destruye la primera que se encuentre.
static bool bullet_hits_bunker(int rx, int ry, int rw, int rh, int dir) {
    for (int b = 0; b < BUNKER_COUNT; b++) {
        int x0 = bunkers[b].x, y0 = bunkers[b].y;
        int ax0 = (rx > x0) ? rx : x0;
        int ay0 = (ry > y0) ? ry : y0;
        int ax1 = (rx + rw < x0 + BUNKER_W) ? rx + rw : x0 + BUNKER_W;
        int ay1 = (ry + rh < y0 + BUNKER_H) ? ry + rh : y0 + BUNKER_H;
        if (ax0 >= ax1 || ay0 >= ay1) continue;

        int c0 = (ax0 - x0) / BUNKER_CELL_W, c1 = (ax1 - 1 - x0) / BUNKER_CELL_W;
        int r0 = (ay0 - y0) / BUNKER_CELL_H, r1 = (ay1 - 1 - y0) / BUNKER_CELL_H;

        for (int i = 0; i <= r1 - r0; i++) {
            int row = (dir < 0) ? r0 + i : r1 - i;   // borde delantero primero
            for (int col = c0; col <= c1; col++) {
                if (!bunkers[b].cells[row][col]) continue;
                bunkers[b].cells[row][col] = false;
                renderer_fill_rect(x0 + col * BUNKER_CELL_W, y0 + row * BUNKER_CELL_H,
                                   BUNKER_CELL_W, BUNKER_CELL_H, COLOR_BLACK);
                renderer_flush();
                return true;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Dibujo -- sprites simplificados (ver cabecera del archivo)
// ---------------------------------------------------------------------------
// Dibujo -- sprites reales, portados de tu ArcadePi original (32x20px,
// ya cabe de sobra con solo 6 columnas). Cada renderer_draw_rect(...)
// del original (sin color, dibujaba en el color activo del momento)
// pasa a renderer_fill_rect(..., color) explícito.
// ---------------------------------------------------------------------------
static int centered_x(const char *text, int scale) {
    int w = (int)st7789_text_width(text, (uint8_t)scale);
    int x = (TFT_WIDTH - w) / 2;
    return (x < 0) ? 0 : x;
}

// Alien tipo A -- fila 0 (4 pts), sprite único (sin animación de cuerpo)
static void draw_alien_A(int x, int y, int anim, uint16_t color) {
    renderer_fill_rect(x+12, y+ 0,  8,  2, color);
    renderer_fill_rect(x+10, y+ 2, 14,  2, color);
    renderer_fill_rect(x+ 6, y+ 4, 20,  2, color);
    renderer_fill_rect(x+ 2, y+ 6, 28,  2, color);
    renderer_fill_rect(x+ 0, y+ 8,  8,  2, color);
    renderer_fill_rect(x+14, y+ 8,  6,  2, color);
    renderer_fill_rect(x+24, y+ 8,  8,  2, color);
    renderer_fill_rect(x+ 0, y+10, 32,  2, color);
    renderer_fill_rect(x+ 6, y+12,  8,  4, color);
    renderer_fill_rect(x+18, y+12,  8,  4, color);
    renderer_fill_rect(x+ 4, y+16,  4,  2, color);
    renderer_fill_rect(x+12, y+16,  8,  2, color);
    renderer_fill_rect(x+24, y+16,  6,  2, color);
    if (anim == 0) {
        renderer_fill_rect(x+ 0, y+18,  4,  2, color);
        renderer_fill_rect(x+ 8, y+18,  4,  2, color);
        renderer_fill_rect(x+20, y+18,  4,  2, color);
        renderer_fill_rect(x+28, y+18,  4,  2, color);
    } else {
        renderer_fill_rect(x+ 2, y+16,  4,  2, color);
        renderer_fill_rect(x+10, y+18,  4,  2, color);
        renderer_fill_rect(x+18, y+18,  4,  2, color);
        renderer_fill_rect(x+26, y+16,  4,  2, color);
    }
}

// Alien tipo B -- filas 1-2 (3 pts), 2 frames de animación de cuerpo
static void draw_alien_B(int x, int y, int anim, uint16_t color) {
    renderer_fill_rect(x+ 6, y+ 0,  2,  2, color);
    renderer_fill_rect(x+24, y+ 0,  2,  2, color);
    renderer_fill_rect(x+ 8, y+ 2,  4,  2, color);
    renderer_fill_rect(x+20, y+ 2,  4,  2, color);
    renderer_fill_rect(x+ 6, y+ 4, 20,  2, color);
    renderer_fill_rect(x+ 4, y+ 6, 24,  2, color);
    renderer_fill_rect(x+ 0, y+10, 32,  2, color);
    renderer_fill_rect(x+ 6, y+14,  4,  4, color);
    renderer_fill_rect(x+22, y+14,  4,  4, color);
    renderer_fill_rect(x+ 0, y+16,  2,  2, color);
    renderer_fill_rect(x+30, y+16,  2,  2, color);
    if (anim == 0) {
        renderer_fill_rect(x+ 2, y+ 8,  6,  2, color);
        renderer_fill_rect(x+12, y+ 8,  8,  2, color);
        renderer_fill_rect(x+24, y+ 8,  6,  2, color);
        renderer_fill_rect(x+ 0, y+12,  4,  4, color);
        renderer_fill_rect(x+ 6, y+12, 20,  2, color);
        renderer_fill_rect(x+28, y+12,  4,  4, color);
        renderer_fill_rect(x+ 8, y+18,  6,  2, color);
        renderer_fill_rect(x+18, y+18,  6,  2, color);
    } else {
        renderer_fill_rect(x+ 4, y+ 6,  6,  2, color);
        renderer_fill_rect(x+12, y+ 6, 16,  2, color);
        renderer_fill_rect(x+ 2, y+ 8,  8,  2, color);
        renderer_fill_rect(x+12, y+ 8,  8,  2, color);
        renderer_fill_rect(x+24, y+ 8,  6,  2, color);
        renderer_fill_rect(x+ 0, y+12,  4,  4, color);
        renderer_fill_rect(x+ 6, y+12, 20,  2, color);
        renderer_fill_rect(x+28, y+12,  4,  4, color);
        renderer_fill_rect(x+ 6, y+18,  6,  2, color);
        renderer_fill_rect(x+20, y+18,  6,  2, color);
    }
}

// Alien tipo C -- fila 3 (1 pt), alien grande
static void draw_alien_C(int x, int y, int anim, uint16_t color) {
    renderer_fill_rect(x+10, y+ 0, 12,  2, color);
    renderer_fill_rect(x+ 4, y+ 2, 24,  2, color);
    renderer_fill_rect(x+ 2, y+ 4, 30,  2, color);
    renderer_fill_rect(x+ 0, y+ 6, 32,  2, color);
    renderer_fill_rect(x+ 0, y+ 8,  8,  2, color);
    renderer_fill_rect(x+14, y+ 8,  6,  2, color);
    renderer_fill_rect(x+24, y+ 8,  8,  2, color);
    renderer_fill_rect(x+ 0, y+10, 32,  2, color);
    renderer_fill_rect(x+ 4, y+12, 10,  4, color);
    renderer_fill_rect(x+18, y+12, 10,  4, color);
    renderer_fill_rect(x+ 4, y+16,  6,  2, color);
    renderer_fill_rect(x+14, y+16,  4,  2, color);
    renderer_fill_rect(x+24, y+16,  4,  2, color);
    if (anim == 0) {
        renderer_fill_rect(x+ 6, y+18,  4,  2, color);
        renderer_fill_rect(x+22, y+18,  4,  2, color);
    } else {
        renderer_fill_rect(x+ 2, y+18,  4,  2, color);
        renderer_fill_rect(x+26, y+18,  4,  2, color);
    }
}

// Dibuja el sprite del alien de la fila r en (x, y) con la animación dada.
static void draw_alien_at(int r, int c, int x, int y, int anim) {
    (void)c;
    uint16_t color = (r == 0) ? COLOR_ALIEN_A : (r <= 2) ? COLOR_ALIEN_B : COLOR_ALIEN_C;

    if (r == 0)      draw_alien_A(x, y, anim, color);
    else if (r <= 2) draw_alien_B(x, y, anim, color);
    else             draw_alien_C(x, y, anim, color);
}

// Repinta solo los aliens vivos cuyo rectángulo solapa la zona dada
// (p. ej. tras borrar el rastro del platillo sobre la fila superior).
// Mucho más barato que redibujar la formación entera.
static void draw_aliens_in_rect(int rx, int ry, int rw, int rh) {
    for (int r = 0; r < ALIEN_ROWS; r++)
        for (int c = 0; c < ALIEN_COLS; c++) {
            const Alien *a = &aliens[r][c];
            if (!a->drawn) continue;   // solo lo que hay realmente en pantalla (puede ir por detrás de x,y)
            if (a->dx >= rx + rw || a->dx + ALIEN_W <= rx) continue;
            if (a->dy >= ry + rh || a->dy + ALIEN_H <= ry) continue;
            draw_alien_at(r, c, a->dx, a->dy, a->danim);
        }
}

static void draw_player(int x, int y) {
    renderer_fill_rect(x - 2, y - 6, 4, 6, COLOR_WHITE);                // cañón
    renderer_fill_rect(x - PLAYER_W/2, y, PLAYER_W, PLAYER_H, COLOR_WHITE);
}

static void draw_bunker_full(int b) {
    for (int row = 0; row < BUNKER_CELLS_Y; row++)
        for (int col = 0; col < BUNKER_CELLS_X; col++)
            if (bunkers[b].cells[row][col])
                renderer_fill_rect(
                    bunkers[b].x + col * BUNKER_CELL_W,
                    bunkers[b].y + row * BUNKER_CELL_H,
                    BUNKER_CELL_W, BUNKER_CELL_H, COLOR_GREEN);
}

static void draw_saucer(int x, int y) {
    renderer_fill_rect(x, y, SAUCER_W, SAUCER_H, COLOR_RED);
}

// ---------------------------------------------------------------------------
// Render incremental de la partida en curso (SI_PLAYING/SI_PLAYER_DEAD/
// SI_LEVEL_CLEAR). Cada elemento se borra/redibuja y transmite por
// separado -- ver cabecera del archivo sobre por qué (evita que el
// flush de un objeto se estire para envolver a otro lejano, como nos
// pasó en Pong).
// ---------------------------------------------------------------------------
static void draw_field_static(void) {
    renderer_clear(COLOR_BLACK);
    renderer_fill_rect(PLAY_X, PLAY_Y,          PLAY_W, 2, COLOR_WHITE);
    renderer_fill_rect(PLAY_X, PLAY_Y+PLAY_H-2, PLAY_W, 2, COLOR_WHITE);

    for (int b = 0; b < BUNKER_COUNT; b++) draw_bunker_full(b);

    reset_render_trace();
    field_needs_redraw = false;
    renderer_flush();
}

// Repinta bala/bombas/platillo (tal como están en pantalla) si solapan
// la zona dada: al borrar con negro el rectángulo de un alien pueden
// quedar pisados.
static void repaint_movers_in_rect(int rx, int ry, int rw, int rh) {
#define OVL(ax, ay, aw, ah) ((ax) < rx + rw && (ax) + (aw) > rx && (ay) < ry + rh && (ay) + (ah) > ry)
    if (prev_bullet_active && OVL(prev_bullet_x, prev_bullet_y, BULLET_W, BULLET_H))
        renderer_fill_rect(prev_bullet_x, prev_bullet_y, BULLET_W, BULLET_H, COLOR_YELLOW);
    for (int i = 0; i < MAX_BOMBS; i++)
        if (prev_bomb_active[i] && OVL(prev_bomb_x[i], prev_bomb_y[i], BOMB_W, BOMB_H))
            renderer_fill_rect(prev_bomb_x[i], prev_bomb_y[i], BOMB_W, BOMB_H, COLOR_RED);
    if (prev_saucer_active && OVL(prev_saucer_x, SAUCER_Y, SAUCER_W, SAUCER_H))
        draw_saucer(prev_saucer_x, SAUCER_Y);
#undef OVL
}

// Presupuesto de píxeles de formación por tick. Un tick del bucle dura
// ~10 ms y el SPI cuesta ~1 us/px, así que redibujar entera la formación
// de golpe (~17.000 px) se comía 2 ticks seguidos y congelaba disparos,
// nave y sonido. Ahora se reparte: como mucho ~una fila por tick (las
// filas con pocos aliens son baratas y caben varias).
#define FORM_BUDGET_PX      5000   // la primera fila de cada tick se hace siempre, aunque la supere

static inline bool alien_needs_draw(const Alien *a) {
    return a->alive
        ? (!a->drawn || a->x != a->dx || a->y != a->dy || a->anim != a->danim)
        : a->drawn;
}

// Rectángulo que hay que repintar para un alien: unión de la posición
// antigua (si estaba dibujado) y la nueva (si sigue vivo).
static void alien_union_rect(const Alien *a, int *x0, int *y0, int *x1, int *y1) {
    bool has = false;
    *x0 = *y0 = *x1 = *y1 = 0;
    if (a->drawn) {
        *x0 = a->dx; *y0 = a->dy; *x1 = a->dx + ALIEN_W; *y1 = a->dy + ALIEN_H;
        has = true;
    }
    if (a->alive) {
        if (!has) { *x0 = a->x; *y0 = a->y; *x1 = a->x + ALIEN_W; *y1 = a->y + ALIEN_H; }
        else {
            if (a->x < *x0) *x0 = a->x;
            if (a->y < *y0) *y0 = a->y;
            if (a->x + ALIEN_W > *x1) *x1 = a->x + ALIEN_W;
            if (a->y + ALIEN_H > *y1) *y1 = a->y + ALIEN_H;
        }
    }
}

// Píxeles que transmitirá el flush de esta fila (suma de las áreas de
// sus tramos de aliens contiguos que han cambiado). 0 = nada que hacer.
static int formation_row_cost(int r) {
    int cost = 0;
    bool run = false;
    int rx0 = 0, ry0 = 0, rx1 = 0, ry1 = 0;
    for (int c = 0; c <= ALIEN_COLS; c++) {
        const Alien *a = (c < ALIEN_COLS) ? &aliens[r][c] : NULL;
        if (a && alien_needs_draw(a)) {
            int x0, y0, x1, y1;
            alien_union_rect(a, &x0, &y0, &x1, &y1);
            if (!run) { rx0 = x0; ry0 = y0; rx1 = x1; ry1 = y1; run = true; }
            else {
                if (x0 < rx0) rx0 = x0;
                if (y0 < ry0) ry0 = y0;
                if (x1 > rx1) rx1 = x1;
                if (y1 > ry1) ry1 = y1;
            }
        } else if (run) {
            cost += (rx1 - rx0) * (ry1 - ry0);
            run = false;
        }
    }
    return cost;
}

// Redibuja la formación en TRAMOS de aliens contiguos que han cambiado,
// de abajo a arriba, con un flush por tramo (no uno por alien: cada
// flush tiene un coste fijo en el SPI; no uno por formación: transmitía
// ~22.000 px aunque quedasen 3 aliens). Y con presupuesto por tick: lo
// que no cabe queda pendiente (formation_dirty sigue activo) y se hace
// en los ticks siguientes -- la lógica (posiciones, colisiones) va
// siempre por delante; la pantalla se pone al día en 1-4 ticks.
//
// De ABAJO a ARRIBA: en un paso hacia abajo (10 px, con separación entre
// filas de solo 6) el alien de arriba invade la zona donde estaba el de
// debajo; si este ya se ha movido, no se pisan.
static void draw_formation_if_moved(void) {
    if (!formation_dirty) return;

    int budget = FORM_BUDGET_PX;
    int rows_done = 0;

    for (int r = ALIEN_ROWS - 1; r >= 0; r--) {
        int row_cost = formation_row_cost(r);
        if (row_cost == 0) continue;
        if (rows_done > 0 && row_cost > budget) break;   // el resto, en el próximo tick
        rows_done++;

        bool run = false;                    // hay un tramo abierto sin flushear
        int  rx0 = 0, ry0 = 0, rx1 = 0, ry1 = 0;

        for (int c = 0; c <= ALIEN_COLS; c++) {
            Alien *a = (c < ALIEN_COLS) ? &aliens[r][c] : NULL;

            if (a && alien_needs_draw(a)) {
                int x0, y0, x1, y1;
                alien_union_rect(a, &x0, &y0, &x1, &y1);

                renderer_fill_rect(x0, y0, x1 - x0, y1 - y0, COLOR_BLACK);
                // Restaurar los bunkers que sigan existiendo bajo la zona
                // borrada (los que pisan los aliens ya no están en el modelo).
                bunkers_rect(x0, y0, x1 - x0, y1 - y0, true);

                if (a->alive) {
                    draw_alien_at(r, c, a->x, a->y, a->anim);
                    a->dx = a->x; a->dy = a->y; a->danim = a->anim;
                    a->drawn = true;
                } else {
                    a->drawn = false;
                }

                repaint_movers_in_rect(x0, y0, x1 - x0, y1 - y0);

                if (!run) { rx0 = x0; ry0 = y0; rx1 = x1; ry1 = y1; run = true; }
                else {
                    if (x0 < rx0) rx0 = x0;
                    if (y0 < ry0) ry0 = y0;
                    if (x1 > rx1) rx1 = x1;
                    if (y1 > ry1) ry1 = y1;
                }
            } else if (run) {
                // Fin del tramo: un único flush para todos sus aliens.
                renderer_flush();
                budget -= (rx1 - rx0) * (ry1 - ry0);
                run = false;
            }
        }
    }

    // ¿Queda algún alien por poner al día en pantalla?
    formation_dirty = false;
    for (int r = 0; r < ALIEN_ROWS && !formation_dirty; r++)
        for (int c = 0; c < ALIEN_COLS; c++)
            if (alien_needs_draw(&aliens[r][c])) { formation_dirty = true; break; }
}

static void draw_player_if_moved(void) {
    if (px == prev_player_x && !player_dirty) return;
    if (prev_player_x >= 0)
        renderer_fill_rect(prev_player_x - PLAYER_W/2 - 2, PLAYER_Y - 6,
                            PLAYER_W + 4, PLAYER_H + 6, COLOR_BLACK);
    draw_player(px, PLAYER_Y);
    prev_player_x = px;
    player_dirty = false;
    renderer_flush();
}

static void erase_player(void) {
    if (prev_player_x < 0) return;
    renderer_fill_rect(prev_player_x - PLAYER_W/2 - 2, PLAYER_Y - 6,
                        PLAYER_W + 4, PLAYER_H + 6, COLOR_BLACK);
    prev_player_x = -1;
    renderer_flush();
}

static void draw_bullet_if_changed(void) {
    if (!bullet_active && !prev_bullet_active) return;

    if (prev_bullet_active) {
        renderer_fill_rect(prev_bullet_x, prev_bullet_y, BULLET_W, BULLET_H, COLOR_BLACK);
        bunkers_rect(prev_bullet_x, prev_bullet_y, BULLET_W, BULLET_H, true);
        if (prev_bullet_y + BULLET_H > PLAYER_Y - 6) player_dirty = true; // rozó el cañón
    }
    if (bullet_active)
        renderer_fill_rect(bx, by, BULLET_W, BULLET_H, COLOR_YELLOW);

    prev_bullet_active = bullet_active;
    prev_bullet_x = bx; prev_bullet_y = by;
    renderer_flush();
}

static void draw_bombs_if_changed(void) {
    bool any = false;
    for (int i = 0; i < MAX_BOMBS; i++) {
        bool moved = (bombs[i].active != prev_bomb_active[i]) ||
                     (bombs[i].active && (bombs[i].x != prev_bomb_x[i] || bombs[i].y != prev_bomb_y[i]));
        if (!moved) continue;

        if (prev_bomb_active[i]) {
            renderer_fill_rect(prev_bomb_x[i], prev_bomb_y[i], BOMB_W, BOMB_H, COLOR_BLACK);
            bunkers_rect(prev_bomb_x[i], prev_bomb_y[i], BOMB_W, BOMB_H, true);
            if (prev_bomb_y[i] + BOMB_H > PLAYER_Y - 6) player_dirty = true;
        }
        if (bombs[i].active)
            renderer_fill_rect(bombs[i].x, bombs[i].y, BOMB_W, BOMB_H, COLOR_RED);

        prev_bomb_active[i] = bombs[i].active;
        prev_bomb_x[i] = bombs[i].x; prev_bomb_y[i] = bombs[i].y;
        any = true;
    }
    if (any) renderer_flush();
}

static void draw_saucer_if_changed(void) {
    if (saucer.active == prev_saucer_active &&
        (!saucer.active || saucer.x == prev_saucer_x)) return;
    if (saucer.active && prev_saucer_active) {
        int moved = saucer.x - prev_saucer_x;
        if ((moved < 0 ? -moved : moved) < SAUCER_DRAW_STEP) return;   // aún no merece un flush
    }

    if (prev_saucer_active) {
        renderer_fill_rect(prev_saucer_x, SAUCER_Y, SAUCER_W, SAUCER_H, COLOR_BLACK);
        // Si el platillo cruza la fila superior de aliens, el borrado
        // les deja un hueco: se repintan solo los aliens que toca.
        draw_aliens_in_rect(prev_saucer_x, SAUCER_Y, SAUCER_W, SAUCER_H);
    }
    if (saucer.active)
        draw_saucer(saucer.x, SAUCER_Y);

    prev_saucer_active = saucer.active;
    prev_saucer_x = saucer.x;
    renderer_flush();
}

static void draw_hud_if_changed(void) {
    char buf[16];
    bool changed = false;

    if (score != prev_score) {
        renderer_fill_rect(PLAY_X+2, PLAY_Y+3, 90, 18, COLOR_BLACK);
        snprintf(buf, sizeof(buf), "%d", score);
        renderer_draw_text(PLAY_X+2, PLAY_Y+3, buf, COLOR_WHITE, COLOR_BLACK, 2);
        prev_score = score;
        changed = true;
    }
    if (lives != prev_lives) {
        renderer_fill_rect(PLAY_X+PLAY_W-56, PLAY_Y+3, 54, 18, COLOR_BLACK);
        snprintf(buf, sizeof(buf), "V:%d", lives);
        renderer_draw_text(PLAY_X+PLAY_W-56, PLAY_Y+3, buf, COLOR_WHITE, COLOR_BLACK, 2);
        prev_lives = lives;
        changed = true;
    }
    if (level != prev_level) {
        renderer_fill_rect(CX-40, PLAY_Y+3, 80, 18, COLOR_BLACK);
        snprintf(buf, sizeof(buf), "NIV %d", level);
        renderer_draw_text(centered_x(buf, 2), PLAY_Y+3, buf, COLOR_WHITE, COLOR_BLACK, 2);
        prev_level = level;
        changed = true;
    }
    if (changed) renderer_flush();
}

// Mensaje central/inferior parpadeante -- redibuja solo si el texto
// que toca mostrar ahora mismo es distinto del último dibujado (ver
// mismo patrón en pong.c).
static void update_bottom_message(const char *target, int y, int scale) {
    if (strcmp(target, prev_bottom_msg) == 0) return;

    renderer_fill_rect(0, y - 2, TFT_WIDTH, 16, COLOR_BLACK);
    if (target[0]) {
        renderer_draw_text(centered_x(target, scale), y, target, COLOR_WHITE, COLOR_BLACK, scale);
    }
    strncpy(prev_bottom_msg, target, sizeof(prev_bottom_msg) - 1);
    prev_bottom_msg[sizeof(prev_bottom_msg) - 1] = '\0';
    renderer_flush();
}

// Puntos del platillo al destruirlo: se muestran ~1s en el sitio del
// impacto. Igual que el resto, solo se redibuja al cambiar de estado
// (aparece una vez, desaparece una vez), no en cada tick.
static void draw_saucer_points_if_changed(void) {
    bool show = (saucer.explode_cnt > 0);
    if (show == prev_saucer_pts_shown) return;

    if (show) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", saucer.pts);
        int x = saucer.pts_display - (int)st7789_text_width(buf, 1) / 2;
        if (x < 0) x = 0;
        renderer_draw_text(x, SAUCER_Y, buf, COLOR_YELLOW, COLOR_BLACK, 1);
    } else {
        renderer_fill_rect(0, SAUCER_Y - 2, TFT_WIDTH, 12, COLOR_BLACK);
        // la franja borrada puede pisar la fila superior de aliens
        draw_aliens_in_rect(0, SAUCER_Y - 2, TFT_WIDTH, 12);
    }
    prev_saucer_pts_shown = show;
    renderer_flush();
}

static void draw_playing_frame(void) {
    if (!si_render) return;   // tick de recuperación: solo lógica, sin dibujar
    if (field_needs_redraw) draw_field_static();

    PROF(P_FORM,   draw_formation_if_moved());
    PROF(P_BULLET, draw_bullet_if_changed());
    PROF(P_BOMBS,  draw_bombs_if_changed());
    PROF(P_SAUCER, draw_saucer_if_changed());
    PROF(P_SAUCER, draw_saucer_points_if_changed());

    if (state == SI_PLAYING) {
        PROF(P_PLAYER, draw_player_if_moved());
    } else if (state == SI_PLAYER_DEAD) {
        bool bon = (blink / 20) % 2 == 0;
        if (bon) draw_player_if_moved(); else erase_player();
    }

    PROF(P_HUD, draw_hud_if_changed());

    bool bon = (blink / 20) % 2 == 0;

    const char *msg = "";
    if (state == SI_LEVEL_CLEAR && bon) msg = "NIVEL SUPERADO!";
    else if (demo && bon)               msg = "DEMO - PULSA PARA JUGAR";

    update_bottom_message(msg, CY, 2);
}

// ---------------------------------------------------------------------------
// Pantallas "estáticas" -- se redibujan enteras solo al entrar en el
// estado, como en pong.c
// ---------------------------------------------------------------------------
static void draw_select_screen(void) {
    renderer_clear(COLOR_BLACK);

    // ==========================================
    // 1. TÍTULO PRINCIPAL (Estilo Taito)
    // ==========================================
    renderer_draw_text(centered_x("SPACE INVADERS", 2), 20, "SPACE INVADERS", COLOR_GREEN, COLOR_BLACK, 2);
    
    // Subtítulo decorativo retro
    renderer_draw_text(centered_x("* TAITO RETRO ARCADE *", 1), 42, "* TAITO RETRO ARCADE *", COLOR_WHITE, COLOR_BLACK, 1);

    // ==========================================
    // 2. TABLA DE PUNTUACIONES / ALIENS
    // ==========================================
    int start_y = 75;
    int col_text_x = 110;

    // Fila superior: Mystery / OVNI especial
    // Dibujo simple de platillo volante
    int ufo_x = 75;
    int ufo_y = start_y;
    renderer_fill_rect(ufo_x + 4, ufo_y, 16, 2, COLOR_RED);
    renderer_fill_rect(ufo_x + 2, ufo_y + 2, 20, 4, COLOR_RED);
    renderer_fill_rect(ufo_x, ufo_y + 6, 24, 2, COLOR_RED);
    renderer_fill_rect(ufo_x + 4, ufo_y + 8, 2, 2, COLOR_RED);
    renderer_fill_rect(ufo_x + 10, ufo_y + 8, 4, 2, COLOR_RED);
    renderer_fill_rect(ufo_x + 18, ufo_y + 8, 2, 2, COLOR_RED);
    renderer_draw_text(col_text_x, ufo_y + 2, "= ? MYSTERY", COLOR_RED, COLOR_BLACK, 1);

    // Fila 2: Alien calamar (Tipo 1 - parte superior)
    int a1_x = 83;
    int a1_y = start_y + 22;
    renderer_fill_rect(a1_x + 2, a1_y, 8, 2, COLOR_WHITE);
    renderer_fill_rect(a1_x, a1_y + 2, 12, 6, COLOR_WHITE);
    renderer_fill_rect(a1_x + 2, a1_y + 8, 8, 2, COLOR_WHITE);
    renderer_draw_text(col_text_x, a1_y + 2, "= 30 POINTS", COLOR_WHITE, COLOR_BLACK, 1);

    // Fila 3: Alien cangrejo (Tipo 2 - medio)
    int a2_x = 83;
    int a2_y = start_y + 40;
    renderer_fill_rect(a2_x + 2, a2_y, 8, 6, COLOR_CYAN);
    renderer_fill_rect(a2_x, a2_y + 2, 12, 4, COLOR_CYAN);
    renderer_draw_text(col_text_x, a2_y + 2, "= 20 POINTS", COLOR_WHITE, COLOR_BLACK, 1);

    // Fila 4: Alien pulpo (Tipo 3 - inferior)
    int a3_x = 83;
    int a3_y = start_y + 58;
    renderer_fill_rect(a3_x + 2, a3_y, 8, 4, COLOR_GREEN);
    renderer_fill_rect(a3_x, a3_y + 4, 12, 4, COLOR_GREEN);
    renderer_draw_text(col_text_x, a3_y + 2, "= 10 POINTS", COLOR_WHITE, COLOR_BLACK, 1);

    // ==========================================
    // 3. INSTRUCCIONES DE CONTROL
    // ==========================================
    renderer_fill_rect(30, 155, SCREEN_W - 60, 1, COLOR_GREEN);
    
    renderer_draw_text(centered_x("STICK: MOVER NAVE", 1), 165, "STICK: MOVER NAVE", COLOR_WHITE, COLOR_BLACK, 1);
    renderer_draw_text(centered_x("BOTON A: DISPARAR", 1), 180, "BOTON A: DISPARAR", COLOR_WHITE, COLOR_BLACK, 1);

    // ==========================================
    // 4. MENSAJE DE INICIO PARPADEANTE
    // ==========================================
    if ((blink / 25) % 2 == 0) {
        renderer_draw_text(centered_x("PRESS BUTTON TO START", 1), 210, "PRESS BUTTON TO START", COLOR_GREEN, COLOR_BLACK, 1);
    }

    renderer_flush();
}


static void draw_over_screen(void) {
    renderer_clear(COLOR_BLACK);
    renderer_draw_text(centered_x("GAME OVER", 3), CY-30, "GAME OVER", COLOR_YELLOW, COLOR_BLACK, 3);
    char buf[24];
    snprintf(buf, sizeof(buf), "PUNTOS: %d", score);
    renderer_draw_text(centered_x(buf, 2), CY+8, buf, COLOR_WHITE, COLOR_BLACK, 2);
    if (!demo)
        renderer_draw_text(centered_x("PULSA PARA CONTINUAR", 1), CY+40,
                            "PULSA PARA CONTINUAR", COLOR_WHITE, COLOR_BLACK, 1);
    renderer_flush();
}

static void draw_scores_screen(void) {
    renderer_clear(COLOR_BLACK);
    highscores_draw(SI_GAME_ID, "SPACE INVADERS", 20);
    renderer_flush();
}

// ---------------------------------------------------------------------------
// Lógica de juego -- un tick
// ---------------------------------------------------------------------------
static void si_tick(void) {
    blink++;

    if (demo) {
        bool any = controls_menu_select() || controls_get_raw_delta_x(0) != 0;
        if (any || ++demo_ticks >= TICKS_S * 30) {
            g_done = true;
            return;
        }
    }

    switch (state) {

    // ------------------------------------------------------------------
    case SI_SELECT:
        if (controls_menu_select()) {
            game_start();
            sound_stop_menu_music();
            state = SI_PLAYING;
        }
        break;

    // ------------------------------------------------------------------
    case SI_PLAYING: {

        march_ticks_since++;

        // --- Mover jugador ---
        if (!demo) {
            int raw = controls_get_raw_delta_x(0);
            if (raw) {
                enc_acc += raw;
                int steps = enc_acc / 2; // 2 transiciones por paso, como Pong
                if (steps != 0) {
                    px = clamp(px + steps * PLAYER_SPD,
                               PLAY_X + PLAYER_W/2,
                               PLAY_X + PLAY_W - PLAYER_W/2);
                    enc_acc -= steps * 2;
                }
            }
        } else {
            // Demo: perseguir al alien vivo más bajo
            int target = CX;
            int lowest = PLAY_Y;
            for (int r = ALIEN_ROWS-1; r >= 0; r--)
                for (int c = 0; c < ALIEN_COLS; c++)
                    if (aliens[r][c].alive && aliens[r][c].y > lowest) {
                        lowest = aliens[r][c].y;
                        target = aliens[r][c].x + ALIEN_W/2;
                    }
            if (px < target - 2) px = clamp(px + PLAYER_SPD, PLAY_X + PLAYER_W/2, PLAY_X + PLAY_W - PLAYER_W/2);
            if (px > target + 2) px = clamp(px - PLAYER_SPD, PLAY_X + PLAYER_W/2, PLAY_X + PLAY_W - PLAYER_W/2);
        }

        // --- Disparar ---
        bool fire = !demo ? controls_menu_select() : (++demo_fire_cnt >= TICKS_S / 2);

        if (fire) {
            if (demo) demo_fire_cnt = 0;
            if (!bullet_active) {
                bullet_active = true;
                bx = px - BULLET_W / 2;
                by = PLAYER_Y - BULLET_H;
                sound_effect_laser_big();
            }
        }

        // --- Mover bala ---
        if (bullet_active) {
            by -= BULLET_SPD;
            if (by < PLAY_Y) {
                bullet_active = false;
            } else if (bullet_hits_bunker(bx, by, BULLET_W, BULLET_H, -1)) {
                bullet_active = false;
            } else {
                bool hit = false;
                for (int r = 0; r < ALIEN_ROWS && !hit; r++)
                    for (int c = 0; c < ALIEN_COLS && !hit; c++) {
                        Alien *a = &aliens[r][c];
                        if (!a->alive) continue;
                        if (bx + BULLET_W > a->x && bx < a->x + ALIEN_W &&
                            by < a->y + ALIEN_H && by + BULLET_H > a->y) {
                            a->alive = false;
                            formation_dirty = true;
                            score   += ALIEN_PTS[r];
                            falive--;
                            bullet_active = false;
                            sound_effect_select();
                            hit = true;
                        }
                    }
            }
        }

        // --- Paso de formación ---
        // Solo se da el paso si la pantalla ya está al día con el anterior
        // (formation_dirty == false): el dibujado va con presupuesto por
        // tick y puede tardar 1-4 ticks; así el desfase entre lógica y
        // pantalla es como mucho de UN paso (si no, los rectángulos de
        // borrado de aliens vecinos se pisarían entre sí). De paso, es el
        // límite natural de velocidad: la formación no puede ir más
        // deprisa de lo que la pantalla es capaz de pintarla.
        if (falive > 0 && !formation_dirty && time_reached_ms(next_formation_step)) {
            formation_step();
        }

        // --- Platillo volante ---
        if (!saucer.active && saucer.explode_cnt == 0 && time_reached_ms(next_saucer_spawn)) {
            next_saucer_spawn = make_timeout_ms(1000 * (8 + rand() % 15));
            saucer.dir = (rand() & 1) ? 1 : -1;
            saucer.x   = (saucer.dir > 0) ? PLAY_X - SAUCER_W : PLAY_X + PLAY_W;
            saucer.pts = SAUCER_MIN_PTS
                       + (rand() % ((SAUCER_MAX_PTS - SAUCER_MIN_PTS) / 50 + 1)) * 50;
            saucer.active = true;
            sound_siren_start();
        }
        if (saucer.active) {
            saucer.x += SAUCER_SPD * saucer.dir;
            if ((saucer.dir > 0 && saucer.x > PLAY_X + PLAY_W) ||
                (saucer.dir < 0 && saucer.x + SAUCER_W < PLAY_X)) {
                saucer.active = false;
                sound_siren_stop();
            }
            if (bullet_active &&
                bx + BULLET_W > saucer.x && bx < saucer.x + SAUCER_W &&
                by < SAUCER_Y + SAUCER_H && by + BULLET_H > SAUCER_Y) {
                bullet_active = false;
                score += saucer.pts;
                saucer.pts_display = saucer.x + SAUCER_W / 2;
                saucer.active      = false;
                saucer.explode_cnt = TICKS_S;
                sound_siren_stop();
                sound_effect_success();
            }
        }
        if (saucer.explode_cnt > 0) saucer.explode_cnt--;

   // --- Bombas enemigas (velocidad constante independiente de la formación) ---
        if (rand() % 30 == 0) alien_fire();

        if (time_reached_ms(next_bomb_step)) {
            next_bomb_step = make_timeout_ms(BOMB_INTERVAL_MS);
            
            for (int i = 0; i < MAX_BOMBS; i++) {
                if (!bombs[i].active) continue;
                bombs[i].y += BOMB_SPD;

                if (bombs[i].y > PLAY_Y + PLAY_H) {
                    bombs[i].active = false;
                    continue;
                }
                if (bullet_hits_bunker(bombs[i].x, bombs[i].y, BOMB_W, BOMB_H, +1)) {
                    bombs[i].active = false;
                    continue;
                }
                if (bombs[i].x + BOMB_W > px - PLAYER_W/2 &&
                    bombs[i].x < px + PLAYER_W/2 &&
                    bombs[i].y + BOMB_H > PLAYER_Y &&
                    bombs[i].y < PLAYER_Y + PLAYER_H) {
                    bombs[i].active = false;
                    lives--;
                    sound_effect_explosion();
                    sound_siren_stop();
                    bullet_active = false;
                    dead_cnt = 0;
                    state = (lives <= 0) ? SI_GAME_OVER : SI_PLAYER_DEAD;
                }
            }
        }


        // --- Invasores llegan al suelo ---
        if (formation_bottom() >= PLAYER_Y) {
            lives = 0;
            dead_cnt = 0;
            sound_siren_stop(); // por si el platillo seguía en pantalla
            state = SI_GAME_OVER;
        }

        // --- Nivel superado ---
        if (falive == 0) {
            clear_cnt = 0;
            sound_effect_success();
            state = SI_LEVEL_CLEAR;
        }

        if (state == SI_GAME_OVER) {
            clear_input_buffer();
            sound_effect_game_over();
            si_render = true;     // el último frame antes de cambiar de pantalla se dibuja siempre
            draw_playing_frame();
            draw_over_screen();
        } else {
            draw_playing_frame();
        }

        break;
    }

    // ------------------------------------------------------------------
    case SI_PLAYER_DEAD:
        if (++dead_cnt >= TICKS_S * 2) {
            for (int i = 0; i < MAX_BOMBS; i++) bombs[i].active = false;
            bullet_active = false;
            enc_acc = 0;
            saucer.active = false;
            saucer.explode_cnt = 0;
            state = SI_PLAYING;
        }
        draw_playing_frame();
        break;

    // ------------------------------------------------------------------
// ------------------------------------------------------------------
// ------------------------------------------------------------------
    case SI_LEVEL_CLEAR:
        if (++clear_cnt >= TICKS_S * 3) {
            level++;
            init_formation();
            init_bunkers(); // Regenera los bunkers por completo
            
            // CAMBIO: Hacemos que el avance por nivel sea más suave (ej: la mitad de una fila o un valor fijo más pequeño)
            // Antes multiplicaba por (ALIEN_H + ALIEN_PADY) completo. Ahora podemos usar una fracción o un paso menor, por ejemplo / 2.
            int extra = ((level - 1) * (ALIEN_H + ALIEN_PADY)) / 2; 
            
            // Límite estricto para que los aliens nunca bajen más allá de una distancia segura por encima de los bunkers
            // Dejamos un margen de seguridad (por ejemplo, 2 filas de separación respecto a los bunkers)
            int max_drop = (BUNKER_Y - ALIEN_START_Y) - (ALIEN_ROWS * (ALIEN_H + ALIEN_PADY)) - 10;
            if (max_drop < 0) max_drop = 0; // Protección por si acaso
            
            if (extra > max_drop) extra = max_drop;
            
            if (extra > 0)
                for (int r = 0; r < ALIEN_ROWS; r++)
                    for (int c = 0; c < ALIEN_COLS; c++)
                        aliens[r][c].y += extra;

            for (int i = 0; i < MAX_BOMBS; i++) bombs[i].active = false;
            bullet_active = false;
            enc_acc = 0;
            march_beat = 0;
            march_ticks_since = MARCH_MIN_TICKS;
            saucer.active = false;
            saucer.explode_cnt = 0;
            next_saucer_spawn = make_timeout_ms(1000 * (8 + rand() % 15));
            
            field_needs_redraw = true; // Fuerza el redibujado completo del campo y los nuevos bunkers
            state = SI_PLAYING;
        }
        draw_playing_frame();
        break;

    // ------------------------------------------------------------------
    case SI_GAME_OVER:
        if (++dead_cnt > TICKS_S) {
            clear_input_buffer();
            if (controls_menu_select()) {
                if (!demo && highscores_is_top(SI_GAME_ID, score)) {
                    highscores_enter(SI_GAME_ID, (uint32_t)score); // bloqueante
                }
                pause_cnt = 0; dead_cnt = 0;
                state = SI_SCORES;
                draw_scores_screen();
            }
            if (dead_cnt > TICKS_S * 8) g_done = true;
        }
        break;

    // ------------------------------------------------------------------
    case SI_SCORES:
        clear_input_buffer();
        if (++dead_cnt > TICKS_S * 8) g_done = true;
        if (controls_menu_select()) g_done = true;
        break;
    }
}

// ---------------------------------------------------------------------------
// API pública
// ---------------------------------------------------------------------------
void game_space_invaders_run(game_mode_t mode) {
    srand((unsigned)esp_timer_get_time());

    demo = (mode == GAME_MODE_DEMO);
    blink = 0;
    pause_cnt = 0;
    dead_cnt = 0;
    demo_ticks = 0;
    demo_fire_cnt = 0;
    g_done = false;
    field_needs_redraw = true;
    reset_render_trace();

    if (demo) {
        game_start();
        state = SI_PLAYING;
    } else {
        score = 0; lives = LIVES_MAX; level = 1;
        bullet_active = false;
        for (int i = 0; i < MAX_BOMBS; i++) bombs[i].active = false;
        enc_acc = 0;
        px = CX;
        init_formation();
        init_bunkers();
        state = SI_SELECT;
        draw_select_screen();
        sound_start_menu_music();
    }

    // Bucle a ritmo fijo con vTaskDelayUntil() -- ver nota de cabecera
    // del archivo y el mismo patrón en asteroids.c/scramble.c.
    //
    // Lógica y dibujado van DESACOPLADOS: la lógica avanza en pasos fijos
    // de tiempo real (step_us). Si un frame tarda más de lo previsto
    // porque el dibujado (SPI) es lento, en la siguiente vuelta se
    // ejecutan los ticks de lógica atrasados -- solo el último dibuja --
    // en vez de dejar que todo el juego (disparos, nave, bombas, sonido)
    // vaya a cámara lenta. Con dibujado lento se ven menos FPS, pero la
    // velocidad del juego es siempre la misma.
    const TickType_t period_ticks = pdMS_TO_TICKS(1000 / TARGET_FPS);
    const TickType_t period = period_ticks ? period_ticks : 1;
    const int64_t    step_us = (int64_t)period * portTICK_PERIOD_MS * 1000;
    TickType_t last_wake = xTaskGetTickCount();
    int64_t    next_logic_us = esp_timer_get_time();

#if SI_PROFILE
    uint32_t prof_next_print = now_ms() + 1000;
    int      prof_due_max = 0;
    printf("SI_PROFILE: periodo del bucle = %u ticks (%u ms)\n",
           (unsigned)period, (unsigned)(period * portTICK_PERIOD_MS));
#endif

    while (!g_done) {
#if SI_PROFILE
        int64_t work_t0 = esp_timer_get_time();
#endif
        controls_update();

        // Ticks de lógica que tocan ya (tolerancia de step/4 por el jitter
        // de despertar de vTaskDelayUntil).
        int64_t now_us = esp_timer_get_time();
        int due = 0;
        while (due < SI_MAX_CATCHUP && now_us + step_us / 4 >= next_logic_us) {
            next_logic_us += step_us;
            due++;
        }
        // Retraso excesivo (> SI_MAX_CATCHUP ticks): no acumular más, el
        // juego irá más lento antes que entrar en una espiral de recuperación.
        if (due == SI_MAX_CATCHUP && now_us > next_logic_us) next_logic_us = now_us;

        for (int k = 0; k < due && !g_done; k++) {
            si_render = (k == due - 1);
            PROF(P_TICK, si_tick());
        }
        si_render = true;
        PROF(P_SOUND, sound_update());
#if SI_PROFILE
        {
            uint32_t w = (uint32_t)(esp_timer_get_time() - work_t0);
            if (w > prof_max[P_WORK]) prof_max[P_WORK] = w;
            if (w > (uint32_t)step_us) prof_over++;
            if (due > prof_due_max) prof_due_max = due;
            if (time_reached_ms(prof_next_print)) {
                prof_next_print = now_ms() + 1000;
                printf("PROF(us max) saucer=%d | form=%u bul=%u bomb=%u sauc=%u play=%u hud=%u | "
                       "si_tick(total)=%u sound=%u work=%u | vueltas_lentas=%u ticks_por_vuelta_max=%d\n",
                       (int)saucer.active,
                       (unsigned)prof_max[P_FORM], (unsigned)prof_max[P_BULLET],
                       (unsigned)prof_max[P_BOMBS], (unsigned)prof_max[P_SAUCER],
                       (unsigned)prof_max[P_PLAYER], (unsigned)prof_max[P_HUD],
                       (unsigned)prof_max[P_TICK], (unsigned)prof_max[P_SOUND],
                       (unsigned)prof_max[P_WORK], (unsigned)prof_over, prof_due_max);
                for (int i = 0; i < P_N; i++) prof_max[i] = 0;
                prof_over = 0;
                prof_due_max = 0;
            }
        }
#endif
        vTaskDelayUntil(&last_wake, period);
    }

    highscores_flush();
}