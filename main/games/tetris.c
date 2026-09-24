/**
 * tetris.c -- portado de ArcadePi, mismo concepto (7 piezas SRS,
 * wall-kicks simplificados, tabla de velocidad por nivel clásica),
 * adaptado a ArcadeColor. Ver los mismos principios de adaptación
 * que en asteroids.c/breakout.c:
 *
 *  - Resolución: 320x240 fija (literales, no TFT_WIDTH/TFT_HEIGHT,
 *    igual que asteroids.c/breakout.c).
 *  - Controles: el original leía variables globales de hardware
 *    (enc1_count, btn1_pressed...) directamente. Aquí se usa
 *    controls.h como en el resto de juegos:
 *      giro=controls_get_raw_delta() (mover columna),
 *      BTN_J*_A (flanco)=rotar, BTN_J*_B (mantenido)=caida suave,
 *      ENC*_SW (flanco)=caida instantanea ("hard drop") -- mismo
 *      papel que el hyperdrive de asteroids.c (accion especial en
 *      el boton del propio encoder).
 *  - OJO -- bug encontrado en controls.c: controls_button_pressed()/
 *    controls_button_down() tratan su argumento como INDICE dentro
 *    de button_pins[] (0..5), no como el numero de pin GPIO. Los
 *    PIN_BTN_J1_A/J1_B/J2_A/J2_B (0,1,2,3) coinciden por casualidad
 *    con su indice, pero PIN_ENC1_SW/PIN_ENC2_SW (8 y 12) NO -- por
 *    eso este archivo define sus propios BTN_IDX_* con el indice
 *    correcto en vez de reutilizar los PIN_* de controls.h para los
 *    botones de los encoders. (Esto probablemente significa que el
 *    hyperdrive de asteroids.c, que llama a
 *    controls_button_pressed(PIN_ENC1_SW), nunca se dispara -- fuera
 *    del alcance de este cambio, pero merece revisarse aparte.)
 *  - Sin pausa (SW) ni "ghost piece": no existen en el resto de
 *    juegos de este proyecto y se ha preferido mantener el mismo
 *    nivel de sencillez en vez de anadir mecanicas nuevas.
 *  - Sin musica en partida (el resto de sound.h no tiene un canal de
 *    musica de juego "enchufable" como el MUSIC_TETRIS del
 *    original): igual que asteroids.c/breakout.c, musica de menu
 *    solo durante la seleccion, silencio + efectos durante la
 *    partida.
 *  - Sin hs_input/TT_ENTER_NAME: highscores_enter() bloqueante, como
 *    en los otros juegos.
 *  - Menu de seleccion rediseñado: ahora es un cursor de 3 opciones
 *    (1 JUGADOR / 2 JUGADORES / MUSICA ON-OFF) navegable con el
 *    encoder, en vez del toggle directo 1P<->2P de antes. La tercera
 *    opcion alterna music_enabled, que solo controla la musica de
 *    partida (sound_start_tetris_music) -- la musica de menu no se ve
 *    afectada. Debajo del titulo se dibuja ademas una fila decorativa
 *    de 7 bloques de color (uno por cada tetrimino), puramente visual.
 *  - Union de J2 a mitad de partida: en modo 1 jugador, pulsar el
 *    switch del encoder de J2 (BTN_IDX_ENC2_SW, el mismo boton que
 *    usa J2 para su hard drop) activa a J2 sin tocar el progreso de
 *    J1 -- ver activate_player2_midgame(). J1 sigue jugando su
 *    partida en curso; J2 simplemente arranca su tablero desde cero
 *    en el lado que antes mostraba "PULSA PARA JUGAR".
 *  - Render incremental: el tablero de cada jugador se compara
 *    celda a celda contra el ultimo fotograma dibujado (como el
 *    borrado dirigido de bricks en breakout.c) en vez de reescribir
 *    pieza a pieza -- mas simple y evita tener que rastrear a mano
 *    la posicion anterior de la pieza activa.
 *  - Bucle propio: game_tetris_run(mode), igual que
 *    game_asteroids_run(mode).
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "tetris.h"
#include "renderer.h"
#include "st7789.h"
#include "controls.h"
#include "highscores.h"
#include "sound.h"

// ---------------------------------------------------------------------------
// Area de juego -- literales fijos, igual que asteroids.c/breakout.c
// ---------------------------------------------------------------------------
#define SCREEN_W 320
#define SCREEN_H 240

#define PLAY_X   4
#define PLAY_Y   3
#define PLAY_W   (SCREEN_W - 2 * PLAY_X)   // 312
#define PLAY_H   (SCREEN_H - 2 * PLAY_Y)   // 234
#define CX       (PLAY_X + PLAY_W / 2)

#define TICKS_S  60   // referencia nominal para contar iteraciones del bucle
                       // (pantallas de pausa/game over/scores), igual que en
                       // asteroids.c/breakout.c -- NO se usa para la caida de
                       // las piezas, que usa tiempo real (ver mas abajo).

// ---------------------------------------------------------------------------
// Botones -- ver nota larga en la cabecera del archivo sobre por que
// estos son INDICES (0..5) y no los PIN_* de controls.h.
// ---------------------------------------------------------------------------
#define BTN_IDX_J1_A     0
#define BTN_IDX_J1_B     1
#define BTN_IDX_J2_A     2
#define BTN_IDX_J2_B     3
#define BTN_IDX_ENC1_SW  4
#define BTN_IDX_ENC2_SW  5

// ---------------------------------------------------------------------------
// Tablero -- 10x20 visibles + 2 filas ocultas arriba para que las
// piezas puedan aparecer sin recortarse (convencion estandar).
//
// Layout nuevo: los DOS tableros (J1 y J2) se ven siempre, uno a cada
// lado de la pantalla, y ocupan casi todo el alto (240 px). El hueco
// que queda en medio se usa como columna de HUD: titulo "TETRIS" y,
// debajo, el marcador de cada jugador (etiqueta, puntuacion, nivel y
// recuadro con la siguiente pieza). En 1 jugador, el area de J2 (a la
// derecha) muestra "PULSA PARA JUGAR" en vez de un tablero vacio.
// ---------------------------------------------------------------------------
#define BOARD_COLS      10
#define VISIBLE_ROWS    20
#define HIDDEN_ROWS      2
#define BOARD_ROWS      (VISIBLE_ROWS + HIDDEN_ROWS)
#define CELL_SIZE        10                                // era 8: tablero mas grande
#define BOARD_W         (BOARD_COLS   * CELL_SIZE)         // 100
#define BOARD_H         (VISIBLE_ROWS * CELL_SIZE)         // 200
#define BOARD_MARGIN     6                                  // hueco a los bordes izq/dcha
#define BOARD_Y          18                                 // 18..218 de 240 -> pegado arriba/abajo

// Columna central entre J1 (izquierda) y J2 (derecha).
#define CENTER_X        (BOARD_MARGIN + BOARD_W)                    // 106
#define CENTER_W        (SCREEN_W - 2 * (BOARD_MARGIN + BOARD_W))   // 108
#define CENTER_CX       (CENTER_X + CENTER_W / 2)                   // 160 (== centro pantalla)

#define COLOR_P0  COLOR_CYAN
#define COLOR_P1  COLOR_YELLOW

// ---------------------------------------------------------------------------
// HUD de la columna central. Los tamanos de fila son aproximados para
// una fuente tipo 5x7 -- si en la pantalla real no queda pixel-perfect,
// basta con retocar estas constantes (todo lo demas se recalcula solo).
// ---------------------------------------------------------------------------
#define TITLE_Y            BOARD_Y                // "TETRIS", escala 2

#define HUD_TAG_H           14   // "J1"/"J2", escala 2
#define HUD_SCORE_H         14   // puntuacion, escala 2
#define HUD_LEVEL_H          9   // "NIV n" / "FIN", escala 1
#define HUD_NEXTLBL_H        9   // "SIGUIENTE", escala 1
#define HUD_ROW_GAP           2
#define HUD_NEXT_BOX         32   // recuadro con la siguiente pieza
#define NEXT_BOX_PAD          2
#define NEXT_SUBCELL        ((HUD_NEXT_BOX - 2 * NEXT_BOX_PAD) / 4)   // 7

#define HUD_BLOCK_H  (HUD_TAG_H + HUD_ROW_GAP + HUD_SCORE_H + HUD_ROW_GAP + \
                       HUD_LEVEL_H + HUD_ROW_GAP + HUD_NEXTLBL_H + HUD_ROW_GAP + \
                       HUD_NEXT_BOX)                                  // 86

#define HUD1_Y   (TITLE_Y + 18)                // marcador de J1
#define HUD2_Y   (HUD1_Y + HUD_BLOCK_H + 4)    // marcador de J2, debajo del de J1

#define HUD_TAG_OFF         0
#define HUD_SCORE_OFF      (HUD_TAG_OFF     + HUD_TAG_H     + HUD_ROW_GAP)
#define HUD_LEVEL_OFF      (HUD_SCORE_OFF   + HUD_SCORE_H   + HUD_ROW_GAP)
#define HUD_NEXTLBL_OFF    (HUD_LEVEL_OFF   + HUD_LEVEL_H   + HUD_ROW_GAP)
#define HUD_BOX_OFF        (HUD_NEXTLBL_OFF + HUD_NEXTLBL_H + HUD_ROW_GAP)

#define BOTTOM_MSG_Y       (BOARD_Y + BOARD_H + 6)   // debajo de los tableros

// ---------------------------------------------------------------------------
// Piezas Tetrimino (SRS), wall kicks simplificados -- igual que el original
// ---------------------------------------------------------------------------
#define NUM_PIECES  7

static const uint8_t PIECE_DATA[NUM_PIECES][4][4] = {
    // 0: I
    { {0,0xF,0,0}, {0x2,0x2,0x2,0x2}, {0,0,0xF,0}, {0x4,0x4,0x4,0x4} },
    // 1: O
    { {0x6,0x6,0,0}, {0x6,0x6,0,0}, {0x6,0x6,0,0}, {0x6,0x6,0,0} },
    // 2: T
    { {0x0,0xE,0x4,0}, {0x4,0x6,0x4,0}, {0x4,0xE,0x0,0}, {0x4,0xC,0x4,0} },
    // 3: S
    { {0,0x6,0xC,0}, {0x4,0x6,0x2,0}, {0,0x6,0xC,0}, {0x4,0x6,0x2,0} },
    // 4: Z
    { {0,0xC,0x6,0}, {0x2,0x6,0x4,0}, {0,0xC,0x6,0}, {0x2,0x6,0x4,0} },
    // 5: J
    { {0x8,0xE,0,0}, {0x6,0x4,0x4,0}, {0x0,0xE,0x2,0}, {0x4,0x4,0xC,0} },
    // 6: L
    { {0x2,0xE,0,0}, {0x4,0x4,0x6,0}, {0x0,0xE,0x8,0}, {0xC,0x4,0x4,0} },
};

// Un color solido distinto por pieza -- I,O,T,S,Z,J,L (orden clasico)
static const uint16_t PIECE_COLORS[NUM_PIECES] = {
    COLOR_CYAN, COLOR_YELLOW, COLOR_MAGENTA, COLOR_GREEN,
    COLOR_RED, COLOR_BLUE, COLOR_WHITE
};

static const int KICK_X[] = { 0,  1, -1,  2, -2 };
static const int KICK_Y[] = { 0,  0,  0, -1, -1 };
#define NUM_KICKS 5

// Intervalo de caida por nivel, en ms reales (tabla clasica NES,
// convertida de "ticks a 16ms" del original a milisegundos directos)
static const int LEVEL_MS[20] = {
    768, 688, 608, 528, 448, 368, 288, 208, 128,  96,
     80,  80,  80,  64,  64,  64,  48,  48,  48,  32
};
#define SOFT_DROP_MS   50
#define LOCK_DELAY_MS 400

// ---------------------------------------------------------------------------
// Estado por jugador
// ---------------------------------------------------------------------------
typedef struct {
    int8_t   board[BOARD_ROWS][BOARD_COLS];  // 0=vacio, si no 1..7 = pieza+1
    int      piece, rotation, px, py;
    int      next_piece;
    uint32_t score;
    int      lines, level;
    bool     active;       // false = este jugador ya perdio (tablero lleno)
    bool     fast_drop;    // BTN_B mantenido
    bool     locking;      // la pieza esta tocando algo debajo, contando el retardo de bloqueo
    int      fall_accum_ms, lock_accum_ms;
    int      move_accum;   // acumulador de encoder crudo (4 = una columna)

    // Rastro de render: ultimo tablero (con pieza incluida) dibujado,
    // para redibujar solo las celdas que han cambiado.
    int8_t   visual_prev[VISIBLE_ROWS][BOARD_COLS];
} PlayerState;

static PlayerState pl[2];
static int  board_x[2];
static int  num_players;
static bool demo;
static int  demo_ai_timer[2];

typedef enum { TT_SELECT, TT_PLAYING, TT_GAME_OVER, TT_SCORES } TtSt;
static TtSt state;

static int  blink;
static int  menu_enc_acc;
static int  menu_sel = 0;          // 0=1 JUGADOR, 1=2 JUGADORES, 2=MUSICA ON/OFF
static bool music_enabled = true;  // solo afecta a la musica DURANTE la partida
static int  pause_ticks;
static int  demo_ticks;
static bool g_done;
static bool board_static_drawn;
static absolute_time_t last_tick_time;

// Rastro de los mensajes de texto (mismo patron que asteroids.c)
static int      prev_score_hud[2] = { -1, -1 };
static int      prev_level_hud[2] = { -1, -1 };
static int      prev_next_hud[2]  = { -1, -1 };
static bool     prev_active_hud[2] = { true, true };
static char     prev_center_msg[24] = "";
static char     prev_bottom_msg[40] = "";

static int rnd(int n) { return n > 0 ? rand() % n : 0; }

static int centered_x(const char *text, int scale) {
    int w = (int)st7789_text_width(text, (uint8_t)scale);
    int x = (TFT_WIDTH - w) / 2;
    return (x < 0) ? 0 : x;
}

// ---------------------------------------------------------------------------
// Helpers de pieza
// ---------------------------------------------------------------------------
static bool piece_cell(int piece, int rot, int row, int col) {
    if (row < 0 || row > 3 || col < 0 || col > 3) return false;
    return (PIECE_DATA[piece][rot][row] >> (3 - col)) & 1;
}

static bool piece_fits(const PlayerState *p, int piece, int rot, int px, int py) {
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (!piece_cell(piece, rot, r, c)) continue;
            int br = py + r, bc = px + c;
            if (bc < 0 || bc >= BOARD_COLS) return false;
            if (br >= BOARD_ROWS) return false;
            if (br < 0) continue;
            if (p->board[br][bc]) return false;
        }
    }
    return true;
}

static int random_piece(void) { return rand() % NUM_PIECES; }

static void spawn_piece(PlayerState *p) {
    p->piece      = p->next_piece;
    p->next_piece = random_piece();
    p->rotation   = 0;
    p->px         = 3;
    p->py         = 0;    // fila 0 = primera fila oculta
    p->fall_accum_ms = 0;
    p->lock_accum_ms = 0;
    p->locking       = false;
    p->fast_drop     = false;
}

static int score_for_lines(int lines, int level) {
    static const int base[] = { 0, 100, 300, 500, 800 };
    if (lines > 4) lines = 4;
    return base[lines] * (level + 1);
}

static void update_level(PlayerState *p) {
    p->level = p->lines / 10;
    if (p->level > 19) p->level = 19;
}

// Fija la pieza activa en el tablero y limpia lineas completas.
// Devuelve el numero de lineas eliminadas.
static int lock_piece(PlayerState *p) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            if (!piece_cell(p->piece, p->rotation, r, c)) continue;
            int br = p->py + r, bc = p->px + c;
            if (br < 0 || br >= BOARD_ROWS) continue;
            p->board[br][bc] = (int8_t)(p->piece + 1);
        }
    int lines = 0;
    for (int r = BOARD_ROWS - 1; r >= 0; r--) {
        bool full = true;
        for (int c = 0; c < BOARD_COLS; c++) if (!p->board[r][c]) { full = false; break; }
        if (full) {
            lines++;
            for (int rr = r; rr > 0; rr--)
                memcpy(p->board[rr], p->board[rr-1], BOARD_COLS);
            memset(p->board[0], 0, BOARD_COLS);
            r++;
        }
    }
    return lines;
}

static void lock_and_spawn(PlayerState *p) {
    int lines = lock_piece(p);
    if (lines > 0) {
        p->lines += lines;
        p->score += (uint32_t)score_for_lines(lines, p->level);
        update_level(p);
        if (lines >= 4) sound_effect_victory();
        else             sound_effect_success();
    } else {
        sound_effect_select();
    }
    spawn_piece(p);
    if (!piece_fits(p, p->piece, p->rotation, p->px, p->py)) {
        p->active = false;
    }
}

static bool try_move(PlayerState *p, int dx) {
    if (!piece_fits(p, p->piece, p->rotation, p->px + dx, p->py)) return false;
    p->px += dx;
    sound_effect_move();
    return true;
}

static void try_rotate(PlayerState *p) {
    int nr = (p->rotation + 1) & 3;
    for (int k = 0; k < NUM_KICKS; k++) {
        if (piece_fits(p, p->piece, nr, p->px + KICK_X[k], p->py + KICK_Y[k])) {
            p->rotation = nr;
            p->px += KICK_X[k];
            p->py += KICK_Y[k];
            sound_effect_move();
            return;
        }
    }
}

static void hard_drop(PlayerState *p) {
    while (piece_fits(p, p->piece, p->rotation, p->px, p->py + 1)) p->py++;
    sound_effect_shoot();
    lock_and_spawn(p);
}

// ---------------------------------------------------------------------------
// IA de demo -- misma heuristica que el original (minimiza altura
// maxima + huecos probando cada columna/rotacion), adaptada a
// piece_fits() de este archivo.
// ---------------------------------------------------------------------------
static void ai_tick(PlayerState *p, int idx) {
    if (!p->active) return;
    demo_ai_timer[idx]++;
    if (demo_ai_timer[idx] < 6) return;
    demo_ai_timer[idx] = 0;

    int best_rot = p->rotation, best_col = p->px, best_score = 32767;
    for (int rot = 0; rot < 4; rot++) {
        for (int col = -1; col < BOARD_COLS; col++) {
            if (!piece_fits(p, p->piece, rot, col, p->py)) continue;
            int drop_row = p->py;
            while (piece_fits(p, p->piece, rot, col, drop_row + 1)) drop_row++;
            int8_t tmp[BOARD_ROWS][BOARD_COLS];
            memcpy(tmp, p->board, sizeof(tmp));
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++) {
                    if (!piece_cell(p->piece, rot, r, c)) continue;
                    int br = drop_row + r, bc = col + c;
                    if (br >= 0 && br < BOARD_ROWS && bc >= 0 && bc < BOARD_COLS) tmp[br][bc] = 1;
                }
            int max_h = 0, holes = 0;
            for (int c = 0; c < BOARD_COLS; c++) {
                int top = BOARD_ROWS;
                for (int r = 0; r < BOARD_ROWS; r++) if (tmp[r][c]) { top = r; break; }
                int h = BOARD_ROWS - top;
                if (h > max_h) max_h = h;
                for (int r = top + 1; r < BOARD_ROWS; r++) if (!tmp[r][c]) holes++;
            }
            int sc = max_h * 4 + holes * 8;
            if (sc < best_score) { best_score = sc; best_rot = rot; best_col = col; }
        }
    }
    if (p->px < best_col)      { if (piece_fits(p, p->piece, p->rotation, p->px + 1, p->py)) p->px++; }
    else if (p->px > best_col) { if (piece_fits(p, p->piece, p->rotation, p->px - 1, p->py)) p->px--; }
    if (p->rotation != best_rot) {
        int nr = (p->rotation + 1) & 3;
        if (piece_fits(p, p->piece, nr, p->px, p->py)) p->rotation = nr;
    }
    if (p->px == best_col && p->rotation == best_rot) p->fast_drop = true;
}

// ---------------------------------------------------------------------------
// Entrada humana
// ---------------------------------------------------------------------------
static void handle_player_input(PlayerState *p, int idx) {
    if (!p->active) return;

    int btn_a  = idx == 0 ? BTN_IDX_J1_A    : BTN_IDX_J2_A;
    int btn_b  = idx == 0 ? BTN_IDX_J1_B    : BTN_IDX_J2_B;
    int enc_sw = idx == 0 ? BTN_IDX_ENC1_SW : BTN_IDX_ENC2_SW;

    int d = controls_get_raw_delta(idx);
    p->move_accum += d;
    while (p->move_accum >= 4) { p->move_accum -= 4; try_move(p, +1); }
    while (p->move_accum <= -4) { p->move_accum += 4; try_move(p, -1); }

    if (controls_button_pressed(btn_a)) try_rotate(p);
    p->fast_drop = controls_button_down(btn_b);
    if (controls_button_pressed(enc_sw)) hard_drop(p);
}

// ---------------------------------------------------------------------------
// Gravedad + retardo de bloqueo -- tiempo real (ms), no ticks, para
// que la velocidad no dependa de cuanto tarde el SPI en dibujar ese
// fotograma (mismo motivo que g_dt_scale en asteroids.c, aplicado
// aqui de forma mas simple porque no hay posiciones continuas).
// ---------------------------------------------------------------------------
static void player_gravity_tick(PlayerState *p, int elapsed_ms) {
    if (!p->active) return;

    bool can_fall = piece_fits(p, p->piece, p->rotation, p->px, p->py + 1);
    if (can_fall) {
        p->locking = false;
        p->lock_accum_ms = 0;
        int interval = p->fast_drop ? SOFT_DROP_MS : LEVEL_MS[p->level];
        p->fall_accum_ms += elapsed_ms;
        if (p->fall_accum_ms >= interval) {
            p->fall_accum_ms -= interval;
            p->py++;
        }
    } else {
        p->fall_accum_ms = 0;
        p->locking = true;
        p->lock_accum_ms += elapsed_ms;
        if (p->lock_accum_ms >= LOCK_DELAY_MS) {
            lock_and_spawn(p);
        }
    }
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------
// Texto centrado dentro de un rectangulo arbitrario (no necesariamente
// el centro de la pantalla) -- se usa para el aviso de J2 en 1 jugador,
// que cae dentro del area del tablero derecho, no en la columna central.
static void draw_text_centered_in(int rx, int rw, int y, const char *text, uint16_t color, int scale) {
    int w = (int)st7789_text_width(text, (uint8_t)scale);
    int x = rx + (rw - w) / 2;
    if (x < rx) x = rx;
    renderer_draw_text(x, y, text, color, COLOR_BLACK, scale);
}

// Aviso estatico "PULSA PARA JUGAR" que ocupa el area de J2 cuando la
// partida es de 1 jugador (en vez de dejar el tablero vacio).
static void draw_p2_placeholder(void) {
    int bx = board_x[1];
    int cy = BOARD_Y + BOARD_H / 2;
    draw_text_centered_in(bx, BOARD_W, cy - 26, "PULSA",  COLOR_WHITE, 2);
    draw_text_centered_in(bx, BOARD_W, cy - 4,  "PARA",   COLOR_WHITE, 2);
    draw_text_centered_in(bx, BOARD_W, cy + 18, "JUGAR",  COLOR_WHITE, 2);
}

static void draw_field_static(void) {
    renderer_clear(COLOR_BLACK);

    // Los dos tableros se dibujan siempre, jueguen o no los dos.
    for (int p = 0; p < 2; p++) {
        int bx = board_x[p];
        renderer_fill_rect(bx-1,        BOARD_Y-1,        BOARD_W+2, 1, COLOR_WHITE);
        renderer_fill_rect(bx-1,        BOARD_Y+BOARD_H,  BOARD_W+2, 1, COLOR_WHITE);
        renderer_fill_rect(bx-1,        BOARD_Y-1,        1, BOARD_H+2, COLOR_WHITE);
        renderer_fill_rect(bx+BOARD_W,  BOARD_Y-1,        1, BOARD_H+2, COLOR_WHITE);
        memset(pl[p].visual_prev, -1, sizeof(pl[p].visual_prev));
    }

    // Columna central: titulo + separadores.
    renderer_draw_text(centered_x("TETRIS", 2), TITLE_Y, "TETRIS", COLOR_CYAN, COLOR_BLACK, 2);
    renderer_fill_rect(CENTER_X, HUD1_Y - 4, CENTER_W, 1, COLOR_WHITE);
    renderer_fill_rect(CENTER_X, HUD1_Y + HUD_BLOCK_H + 2, CENTER_W, 1, COLOR_WHITE);

    // Etiquetas fijas de cada jugador (no cambian durante la partida).
    renderer_draw_text(centered_x("J1", 2), HUD1_Y + HUD_TAG_OFF, "J1", COLOR_P0, COLOR_BLACK, 2);
    renderer_draw_text(centered_x("SIGUIENTE", 1), HUD1_Y + HUD_NEXTLBL_OFF, "SIGUIENTE", COLOR_WHITE, COLOR_BLACK, 1);

    if (num_players == 2) {
        renderer_draw_text(centered_x("J2", 2), HUD2_Y + HUD_TAG_OFF, "J2", COLOR_P1, COLOR_BLACK, 2);
        renderer_draw_text(centered_x("SIGUIENTE", 1), HUD2_Y + HUD_NEXTLBL_OFF, "SIGUIENTE", COLOR_WHITE, COLOR_BLACK, 1);
    } else {
        draw_p2_placeholder();
    }

    prev_score_hud[0] = prev_score_hud[1] = -1;
    prev_level_hud[0] = prev_level_hud[1] = -1;
    prev_next_hud[0]  = prev_next_hud[1]  = -1;
    prev_active_hud[0] = prev_active_hud[1] = true;
    prev_center_msg[0] = '\0';
    prev_bottom_msg[0] = '\0';
    board_static_drawn = true;
    renderer_flush();
}

// Redibuja solo las celdas del tablero (fijas + pieza activa) que han
// cambiado desde el ultimo fotograma -- evita tener que rastrear a
// mano la posicion anterior de la pieza.
static void draw_board_if_changed(PlayerState *p, int idx) {
    int bx = board_x[idx];
    int8_t visual[VISIBLE_ROWS][BOARD_COLS];

    for (int r = 0; r < VISIBLE_ROWS; r++)
        for (int c = 0; c < BOARD_COLS; c++)
            visual[r][c] = p->board[r + HIDDEN_ROWS][c];

    if (p->active) {
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) {
                if (!piece_cell(p->piece, p->rotation, r, c)) continue;
                int br = p->py + r - HIDDEN_ROWS, bc = p->px + c;
                if (br < 0 || br >= VISIBLE_ROWS || bc < 0 || bc >= BOARD_COLS) continue;
                visual[br][bc] = (int8_t)(p->piece + 1);
            }
    }

    bool changed = false;
    for (int r = 0; r < VISIBLE_ROWS; r++) {
        for (int c = 0; c < BOARD_COLS; c++) {
            if (visual[r][c] == p->visual_prev[r][c]) continue;
            int sx = bx + c * CELL_SIZE, sy = BOARD_Y + r * CELL_SIZE;
            uint16_t color = visual[r][c] ? PIECE_COLORS[visual[r][c]-1] : COLOR_BLACK;
            renderer_fill_rect(sx, sy, CELL_SIZE, CELL_SIZE, color);
            p->visual_prev[r][c] = visual[r][c];
            changed = true;
        }
    }
    if (changed) renderer_flush();
}

static void draw_next_preview(int piece, int box_x, int box_y) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            renderer_fill_rect(box_x + NEXT_BOX_PAD + c*NEXT_SUBCELL,
                                box_y + NEXT_BOX_PAD + r*NEXT_SUBCELL,
                                NEXT_SUBCELL, NEXT_SUBCELL,
                                piece_cell(piece, 0, r, c) ? PIECE_COLORS[piece] : COLOR_BLACK);
}

// HUD de un jugador, dibujado en la columna central (no en su tablero):
// puntuacion, nivel y recuadro con la siguiente pieza.
static void draw_hud_if_changed(int idx) {
    PlayerState *p = &pl[idx];
    int hy = (idx == 0) ? HUD1_Y : HUD2_Y;
    uint16_t color = idx == 0 ? COLOR_P0 : COLOR_P1;
    char buf[16];
    bool changed = false;

    if (p->active != prev_active_hud[idx] || prev_next_hud[idx] != p->next_piece) {
        int box_x = CENTER_CX - HUD_NEXT_BOX/2;
        int box_y = hy + HUD_BOX_OFF;
        renderer_fill_rect(box_x-1, box_y-1,            HUD_NEXT_BOX+2, 1, COLOR_WHITE);
        renderer_fill_rect(box_x-1, box_y+HUD_NEXT_BOX,  HUD_NEXT_BOX+2, 1, COLOR_WHITE);
        renderer_fill_rect(box_x-1, box_y-1,             1, HUD_NEXT_BOX+2, COLOR_WHITE);
        renderer_fill_rect(box_x+HUD_NEXT_BOX, box_y-1,  1, HUD_NEXT_BOX+2, COLOR_WHITE);
        renderer_fill_rect(box_x, box_y, HUD_NEXT_BOX, HUD_NEXT_BOX, COLOR_BLACK);
        if (p->active) draw_next_preview(p->next_piece, box_x, box_y);
        prev_next_hud[idx] = p->next_piece;
        changed = true;
    }
    if ((int)p->score != prev_score_hud[idx] || p->active != prev_active_hud[idx]) {
        renderer_fill_rect(CENTER_X, hy + HUD_SCORE_OFF, CENTER_W, HUD_SCORE_H, COLOR_BLACK);
        snprintf(buf, sizeof(buf), "%u", p->score);
        renderer_draw_text(centered_x(buf, 2), hy + HUD_SCORE_OFF, buf, color, COLOR_BLACK, 2);
        prev_score_hud[idx] = (int)p->score;
        changed = true;
    }
    if (p->level != prev_level_hud[idx] || p->active != prev_active_hud[idx]) {
        renderer_fill_rect(CENTER_X, hy + HUD_LEVEL_OFF, CENTER_W, HUD_LEVEL_H, COLOR_BLACK);
        if (p->active) snprintf(buf, sizeof(buf), "NIV %d", p->level);
        else            snprintf(buf, sizeof(buf), "FIN");
        renderer_draw_text(centered_x(buf, 1), hy + HUD_LEVEL_OFF, buf, COLOR_WHITE, COLOR_BLACK, 1);
        prev_level_hud[idx] = p->level;
        changed = true;
    }
    prev_active_hud[idx] = p->active;
    if (changed) renderer_flush();
}

static void update_center_message(const char *target, uint16_t color, int scale) {
    if (strcmp(target, prev_center_msg) == 0) return;
    renderer_fill_rect(0, BOARD_Y+BOARD_H/2-16, TFT_WIDTH, 32, COLOR_BLACK);
    if (target[0]) {
        renderer_draw_text(centered_x(target, scale), BOARD_Y+BOARD_H/2-10, target, color, COLOR_BLACK, scale);
    }
    strncpy(prev_center_msg, target, sizeof(prev_center_msg) - 1);
    prev_center_msg[sizeof(prev_center_msg) - 1] = '\0';
    renderer_flush();
}

static void update_bottom_message(const char *target, int scale) {
    if (strcmp(target, prev_bottom_msg) == 0) return;
    renderer_fill_rect(0, BOTTOM_MSG_Y - 2, TFT_WIDTH, 16, COLOR_BLACK);
    if (target[0]) {
        renderer_draw_text(centered_x(target, scale), BOTTOM_MSG_Y, target, COLOR_WHITE, COLOR_BLACK, scale);
    }
    strncpy(prev_bottom_msg, target, sizeof(prev_bottom_msg) - 1);
    prev_bottom_msg[sizeof(prev_bottom_msg) - 1] = '\0';
    renderer_flush();
}

static void draw_playing_frame(void) {
    if (!board_static_drawn) draw_field_static();

    for (int p = 0; p < num_players; p++) {
        draw_board_if_changed(&pl[p], p);
        draw_hud_if_changed(p);
    }

    bool bon = (blink / 15) % 2 == 0;
    const char *center = "";
    if (state == TT_GAME_OVER) center = "GAME OVER";
    update_center_message(center, COLOR_YELLOW, 3);

    const char *bottom = "";
    if (state == TT_GAME_OVER && bon) bottom = "PULSA PARA CONTINUAR";
    else if (demo && bon)             bottom = "DEMO - PULSA PARA JUGAR";
    update_bottom_message(bottom, 1);
}

// Fila decorativa de mini-bloques bajo el titulo, uno por cada
// tetrimino (mismos colores que PIECE_COLORS) -- puramente visual.
static void draw_menu_deco(int y) {
    const int block = 12;
    const int gap   = 4;
    int total_w = NUM_PIECES * block + (NUM_PIECES - 1) * gap;
    int x = (SCREEN_W - total_w) / 2;
    for (int i = 0; i < NUM_PIECES; i++) {
        renderer_fill_rect(x + i * (block + gap), y, block, block, PIECE_COLORS[i]);
    }
}

// Una linea del menu de seleccion: resaltada con flechas ">  <" y en
// amarillo si es la opcion actualmente elegida por el cursor.
static void draw_menu_item(const char *text, int y, bool selected) {
    char buf[32];
    if (selected) snprintf(buf, sizeof(buf), "> %s <", text);
    else          snprintf(buf, sizeof(buf), "  %s  ", text);
    uint16_t color = selected ? COLOR_YELLOW : COLOR_WHITE;
    renderer_draw_text(centered_x(buf, 2), y, buf, color, COLOR_BLACK, 2);
}

static void draw_select_screen(void) {
    renderer_clear(COLOR_BLACK);

    renderer_draw_text(centered_x("TETRIS", 3), PLAY_Y + 8, "TETRIS", COLOR_CYAN, COLOR_BLACK, 3);
    draw_menu_deco(PLAY_Y + 40);

    char music_label[24];
    snprintf(music_label, sizeof(music_label), "MUSICA: %s", music_enabled ? "ON" : "OFF");

    draw_menu_item("1 JUGADOR",    PLAY_Y + 70,  menu_sel == 0);
    draw_menu_item("2 JUGADORES",  PLAY_Y + 96,  menu_sel == 1);
    draw_menu_item(music_label,    PLAY_Y + 122, menu_sel == 2);

    renderer_draw_text(centered_x("GIRA PARA ELEGIR - PULSA PARA CONFIRMAR", 1), PLAY_Y+150,
                        "GIRA PARA ELEGIR - PULSA PARA CONFIRMAR", COLOR_WHITE, COLOR_BLACK, 1);
    renderer_draw_text(centered_x("GIRO=MOVER  BOTON A=ROTAR  BOTON B=CAIDA  CLIC GIRO=CAIDA RAPIDA", 1), PLAY_Y+165,
                        "GIRO=MOVER  BOTON A=ROTAR  BOTON B=CAIDA  CLIC GIRO=CAIDA RAPIDA", COLOR_WHITE, COLOR_BLACK, 1);
    prev_center_msg[0] = '\0';
    prev_bottom_msg[0] = '\0';
    renderer_flush();
}

static void draw_scores_screen(void) {
    renderer_clear(COLOR_BLACK);
    highscores_draw(TT_GAME_ID, "TETRIS", 20);
    renderer_flush();
}

// ---------------------------------------------------------------------------
// Arranque de partida
// ---------------------------------------------------------------------------
static void player_reset(PlayerState *p) {
    memset(p->board, 0, sizeof(p->board));
    memset(p->visual_prev, -1, sizeof(p->visual_prev));
    p->score = 0;
    p->lines = 0;
    p->level = 0;
    p->active = true;
    p->fast_drop = false;
    p->locking = false;
    p->fall_accum_ms = p->lock_accum_ms = 0;
    p->move_accum = 0;
    p->next_piece = random_piece();
    spawn_piece(p);
}

static void start_game(void) {
    // Los dos huecos de tablero (J1 izquierda, J2 derecha) son fijos ahora:
    // ambos se ven siempre, se juegue con 1 o 2 jugadores.
    board_x[0] = BOARD_MARGIN;
    board_x[1] = SCREEN_W - BOARD_MARGIN - BOARD_W;
    for (int p = 0; p < 2; p++) {
        if (p < num_players) player_reset(&pl[p]);
        else                 pl[p].active = false;
        demo_ai_timer[p] = 0;
    }
    board_static_drawn = false;
    blink = 0;
}

// Union de J2 a mitad de partida: si la partida era de 1 jugador y se
// pulsa el switch del encoder de J2 (mismo boton que su hard drop),
// J2 se suma sin reiniciar nada de J1 -- su marcador/tablero/nivel
// siguen intactos, solo se activa el segundo jugador y arranca su
// propio tablero desde cero, como si hubiera empezado con el.
static void activate_player2_midgame(void) {
    num_players = 2;
    player_reset(&pl[1]);
    demo_ai_timer[1] = 0;

    // draw_field_static() no dibujo las etiquetas de J2 porque cuando
    // se llamo la partida era de 1 jugador (solo puso el aviso
    // "PULSA PARA JUGAR" en su tablero) -- las anadimos ahora. El
    // propio tablero de J2 (que borra ese aviso) y su HUD (caja de
    // siguiente pieza, puntuacion, nivel) se pintan solos en la
    // siguiente llamada a draw_playing_frame(), que ya recorre hasta
    // num_players-1 y detecta todo como "cambiado" por ser la primera
    // vez que se dibuja ese indice.
    renderer_draw_text(centered_x("J2", 2), HUD2_Y + HUD_TAG_OFF, "J2", COLOR_P1, COLOR_BLACK, 2);
    renderer_draw_text(centered_x("SIGUIENTE", 1), HUD2_Y + HUD_NEXTLBL_OFF, "SIGUIENTE", COLOR_WHITE, COLOR_BLACK, 1);
    renderer_flush();

    sound_effect_select();
}

// ---------------------------------------------------------------------------
// Tick principal
// ---------------------------------------------------------------------------
static void tt_tick(void) {
    blink++;

    if (demo) {
        bool any = controls_menu_select() || controls_get_raw_delta(0) != 0;
        if (any || ++demo_ticks >= TICKS_S * 40) {
            g_done = true;
            return;
        }
    }

    int64_t elapsed_us = absolute_time_diff_us(last_tick_time, get_absolute_time());
    last_tick_time = get_absolute_time();
    int elapsed_ms = (int)(elapsed_us / 1000);
    if (elapsed_ms < 1)  elapsed_ms = 1;
    if (elapsed_ms > 50) elapsed_ms = 50; // limita saltos tras una pausa larga (highscores_enter())

    switch (state) {

    case TT_SELECT: {
        int d = controls_get_raw_delta(0);
        if (d) {
            menu_enc_acc += d;
            if (menu_enc_acc >= 2)  { menu_sel = (menu_sel + 1) % 3; menu_enc_acc = 0; sound_effect_move(); draw_select_screen(); }
            if (menu_enc_acc <= -2) { menu_sel = (menu_sel + 2) % 3; menu_enc_acc = 0; sound_effect_move(); draw_select_screen(); }
        }
        if (controls_menu_select()) {
            if (menu_sel == 2) {
                // No inicia partida: solo alterna la musica in-game y redibuja.
                music_enabled = !music_enabled;
                sound_effect_select();
                draw_select_screen();
            } else {
                num_players = (menu_sel == 0) ? 1 : 2;
                sound_stop_menu_music();
                start_game();
                state = TT_PLAYING;
                if (music_enabled) sound_start_tetris_music();
            }
        }
        break;
    }

    case TT_PLAYING: {
        if (demo) {
            for (int p = 0; p < num_players; p++) ai_tick(&pl[p], p);
        } else {
            if (num_players == 1 && controls_button_pressed(BTN_IDX_ENC2_SW)) {
                activate_player2_midgame();
            }
            for (int p = 0; p < num_players; p++) handle_player_input(&pl[p], p);
        }
        for (int p = 0; p < num_players; p++) player_gravity_tick(&pl[p], elapsed_ms);

        bool any_active = false;
        for (int p = 0; p < num_players; p++) if (pl[p].active) any_active = true;

        if (!any_active) {
            sound_stop_tetris_music();
            sound_effect_game_over();
            draw_playing_frame();
            for (int p = 0; p < num_players; p++) {
                if (!demo && highscores_is_top(TT_GAME_ID, pl[p].score)) {
                    highscores_enter(TT_GAME_ID, pl[p].score); // bloqueante
                }
            }
            pause_ticks = 0;
            state = TT_GAME_OVER;
        }

        draw_playing_frame();
        break;
    }

    case TT_GAME_OVER:
        if (++pause_ticks > TICKS_S) {
            if (controls_menu_select() || pause_ticks > TICKS_S*8) {
                pause_ticks = 0;
                state = TT_SCORES;
                draw_scores_screen();
            }
        }
        break;

    case TT_SCORES:
        if (++pause_ticks > TICKS_S*8) g_done = true;
        if (controls_menu_select()) g_done = true;
        break;
    }
}

// ---------------------------------------------------------------------------
// API publica
// ---------------------------------------------------------------------------
void game_tetris_run(game_mode_t mode) {
    srand(time_us_32());

    demo = (mode == GAME_MODE_DEMO);
    num_players = (mode == GAME_MODE_2P) ? 2 : 1;
    if (demo) num_players = 1; // igual que asteroids.c: demo siempre 1 jugador visible
    blink = 0;
    demo_ticks = 0;
    menu_enc_acc = 0;
    g_done = false;
    last_tick_time = get_absolute_time();

    if (demo) {
        start_game();
        state = TT_PLAYING;
        sound_start_tetris_music();
    } else {
        menu_sel = (num_players == 2) ? 1 : 0;
        state = TT_SELECT;
        draw_select_screen();
        sound_start_menu_music();
    }

    while (!g_done) {
        controls_update();
        tt_tick();
        sound_update();
        sleep_ms(8);
    }

    // Por si se sale a mitad de partida (p.ej. demo interrumpida por
    // el usuario): no dejar la música de Tetris sonando de fondo al
    // volver al menú. Si ya se había parado (game over normal), esto
    // no hace nada raro -- igual que sound_stop_pacman_intro().
    sound_stop_tetris_music();

    highscores_flush();
}