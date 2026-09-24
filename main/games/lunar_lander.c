/**
 * lunar_lander.c -- portado de la versión previa (vídeo compuesto,
 * 768x576, callbacks de draw/tick a ~62Hz) a ArcadeColor. Mismo
 * concepto (física de descenso con inercia, terreno generado por
 * nivel, plataformas de distinta dificultad, combustible limitado,
 * maniobra de aterrizaje asistida, explosión al chocar), adaptado
 * siguiendo EXACTAMENTE los mismos principios que asteroids.c:
 *
 *  - Resolución: campo a pantalla casi completa (320x240 apaisada)
 *    en vez de 768x576. Radios, velocidades y anchuras de plataforma
 *    reescalados a mano (proporcional al área de juego, pero
 *    ajustados para que jueguen bien en un campo mucho más pequeño),
 *    no proporcionalmente exactos.
 *  - FÍSICA CON TIEMPO DELTA REAL (ver g_dt_scale, idéntico
 *    mecanismo que asteroids.c): el original asumía un tick fijo de
 *    ~16ms a 62Hz porque su bucle de dibujo/tick era independiente
 *    del coste de refresco. Aquí, como en el resto de juegos de
 *    ArcadeColor, el coste de refrescar por SPI varía según cuánto
 *    haya que dibujar, así que gravedad, empuje y fricción/posición
 *    se escalan por el tiempo real transcurrido en vez de asumir
 *    "una unidad de tiempo fija" por iteración.
 *  - Controles: con un único jugador y 3 pines de entrada por jugador
 *    (encoder + 2 botones) en vez de 4 botones + 2 switches del
 *    original, el mapeo queda:
 *      * controls_get_raw_delta(0)      -> girar la nave (como el
 *        encoder que ya usaba el original para el ángulo)
 *      * BTN_J1_A (mantenido)           -> empuje SUAVE
 *      * BTN_J1_B (mantenido)           -> empuje FUERTE
 *      * BTN_ENC1_SW (flanco)           -> maniobra de aterrizaje:
 *        endereza la nave (ángulo 0) y anula su velocidad horizontal
 *        al instante, con un breve impulso correctivo. Disponible en
 *        cualquier momento del vuelo (con combustible), sin más
 *        condiciones -- una versión anterior la restringía a "casi
 *        vertical y ya casi sin velocidad horizontal", que es
 *        precisamente lo que la maniobra tiene que CONSEGUIR, así
 *        que apenas llegaba a activarse nunca.
 *    Juego de un solo jugador (como el original); game_mode_t se
 *    admite por uniformidad con el resto de juegos, pero solo se
 *    distingue demo vs partida normal.
 *  - CÁMARA DE ZOOM (recuperada del original): al bajar de cierta
 *    altitud sobre el terreno la vista se amplía x2, centrada en la
 *    nave (ver world_to_screen/update_zoom), igual que hacía el
 *    original para dar precisión en el aterrizaje. Con render
 *    incremental esto tiene un coste que el resto del juego no
 *    paga: mientras el zoom está activo, la cámara se mueve cada
 *    tick, así que "borrar la caja anterior" ya no es una operación
 *    válida (la transformación cambió) -- por eso, SOLO durante el
 *    zoom, se hace un redibujado COMPLETO del área de juego en cada
 *    tick (ver draw_zoomed_frame), exactamente como hacía el
 *    renderer de vídeo compuesto original en todo momento. Es más
 *    lento (menos fluido) que el resto del juego, pero solo ocurre
 *    cerca del suelo, que es precisamente cuando conviene ir despacio.
 *  - RENDER INCREMENTAL, no redibujado completo cada frame (a
 *    diferencia del original, que reconstruía toda la escena en
 *    cada llamada a ll_draw() porque su renderer de vídeo compuesto
 *    no tenía coste de transmisión por SPI). Aquí:
 *      * El terreno (relleno sólido bajo la línea de superficie) se
 *        dibuja UNA vez por nivel (draw_field_static), igual que el
 *        borde de campo en asteroids.c.
 *      * La nave y las partículas de explosión SÍ se mueven sobre
 *        el terreno, así que borrar su caja delimitadora con negro
 *        dejaría "agujeros" en el suelo. Por eso, al borrar, se
 *        restaura primero el terreno bajo esa caja (ver
 *        erase_box_over_terrain) antes de pintar la nueva posición --
 *        mismo principio que erase_box() en asteroids.c pero con un
 *        paso extra porque aquí sí hay fondo estático que proteger.
 *      * La nave se dibuja VECTORIAL (líneas), no como el hexágono
 *        relleno con "ventana rotativa" del original (que pintaba
 *        pixel a pixel -- inasumible por SPI). Incluso a menor
 *        escala reconocible: cabina, antena, dos patas y llama de
 *        motor, con el mismo estilo de línea Bresenham 2x2 que la
 *        nave de asteroids.c.
 *      * Las plataformas de aterrizaje se señalan coloreando ese
 *        tramo de terreno (COLOR_PAD) en vez de con postes/luces
 *        parpadeantes/texto de estrellas del original -- esos
 *        elementos estaban DENTRO del área de vuelo y, con
 *        redibujado incremental, se habrían borrado en cuanto la
 *        nave pasase por encima sin un mecanismo propio de
 *        restauración. La dificultad de cada plataforma se sigue
 *        comunicando por su anchura (más estrecha = más difícil,
 *        igual que en el original) y por el multiplicador de
 *        puntos que aparece en el mensaje de aterrizaje.
 *  - Sonido: igual que asteroids.c, se reutilizan los efectos
 *    genéricos de sound.h en vez de los SFX_* específicos del
 *    original (que no existen aquí):
 *      SFX_THRUST    -> sound_effect_move()   (con cooldown, como el original)
 *      SFX_LEVEL_UP  -> sound_effect_success()
 *      SFX_CRASH     -> sound_effect_explosion()
 *      maniobra ok   -> sound_effect_select()
 *      game over     -> sound_effect_game_over()
 *    Música de menú (sound_start/stop_menu_music) durante LL_READY,
 *    igual que AS_SELECT en asteroids.c.
 *  - Sin hs_input/LL_ENTER_NAME: highscores_enter() bloqueante,
 *    exactamente como en asteroids.c (y como pong.c/space_invaders.c).
 *  - Bucle propio: game_lunar_lander_run(mode) con su propio bucle
 *    while(!g_done) { controls_update(); ll_tick(); sound_update();
 *    sleep_ms(8); }, nada de callbacks de draw/tick registrados.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "lunar_lander.h"
#include "renderer.h"
#include "controls.h"
#include "highscores.h"
#include "sound.h"

// ---------------------------------------------------------------------------
// Área de juego -- literales fijos (no TFT_WIDTH/TFT_HEIGHT, que son
// variables en tiempo de ejecución -- ver el mismo comentario en
// asteroids.c). Rotación fija a 320x240.
// ---------------------------------------------------------------------------
#define SCREEN_W 320
#define SCREEN_H 240

#define PLAY_X   4
#define PLAY_Y   3
#define PLAY_W   (SCREEN_W - 2 * PLAY_X)   // 312
#define PLAY_H   (SCREEN_H - 2 * PLAY_Y)   // 234
#define CX       (PLAY_X + PLAY_W / 2)
#define CY       (PLAY_Y + PLAY_H / 2)

#define TICKS_S  60   // referencia nominal SOLO para calcular ms (ver g_dt_scale) -- no se cuentan ticks reales

// ---------------------------------------------------------------------------
// Física: punto fijo Q8.8 (igual que el original y que asteroids.c)
// ---------------------------------------------------------------------------
#define FP          256
#define PX2FP(px)   ((int32_t)(px) << 8)
#define FP2PX(fp)   ((int)((fp) >> 8))

// GRAVITY/THRUST_* son incrementos "por tick nominal" en unidades FP,
// NO reescalados espacialmente (ver comentario largo en el original:
// GRAVITY=2 ≈ 0.008 px/tick² real). Se mantienen idénticos al
// original; lo que sí cambia es que ahora se multiplican por
// g_dt_scale (ver más abajo) para no depender de la duración real
// del tick.
#define GRAVITY           2
#define THRUST_SOFT        5    // empuje suave  (BTN_J1_A mantenido)
#define THRUST_HARD       14    // empuje fuerte (BTN_J1_B mantenido)

#define ROT_SPEED          3    // grados por paso de encoder (evento, sin dt)

// Velocidades máximas y umbrales de aterrizaje -- reescalados a mano
// al nuevo área de juego (312x234 frente a 600x400 del original,
// ver cabecera del archivo). LAND_VY_MAX/LAND_VX_MAX se dejan como
// umbrales absolutos, igual de exigentes que en el original.
#define MAX_VX       (32 * FP)
#define MAX_VY       (45 * FP)
#define LAND_VY_MAX  (FP / 2)   // vy < 2 px/tick para aterrizar
#define LAND_VX_MAX  0            // vx debe ser exactamente 0

#define SHIP_RADIUS 10   // para colisión con el terreno (foot_y = y + SHIP_RADIUS)

/*
 * BTN_J1_A/BTN_J1_B/BTN_ENC1_SW ya vienen definidos en controls.h
 * (índices dentro de button_pins[], NO números de pin GPIO -- ese
 * era el bug de la versión anterior: se usaba PIN_ENC1_SW, un número
 * de GPIO, donde controls_button_pressed()/down() esperan un índice
 * 0-5). No se redefinen aquí -- se usan directamente los de
 * controls.h.
 */

// angle_to_sprite: convierte ángulo continuo 0-359 a una de 16
// posiciones discretas -- usado para elegir el vector de empuje fijo
// (thrust_vx/vy) y para comprobar "nave vertical" al aterrizar.
// Idéntico al original.
#define LANDER_N_ANGLES 16
static int angle_to_sprite(int angle) {
    angle = ((angle % 360) + 360) % 360;
    return (angle * LANDER_N_ANGLES + 180) / 360 % LANDER_N_ANGLES;
}

/* Vectores de empuje fijos para cada una de las 16 orientaciones.
 * Escala: 127 = componente máxima. Idénticos al original -- el
 * empuje siempre coincide exactamente con la orientación dibujada.
 * sprite[0]=0°(12h)  sprite[4]=90°(3h)  sprite[8]=180°(6h)  sprite[12]=270°(9h) */
static const int8_t thrust_vx[16] = {
      0,  54,  99, 122, 127, 123,  99,  56,   0, -54, -99,-122,-127,-123, -99, -56
};
static const int8_t thrust_vy[16] = {
   -127,-123, -99, -56,   0,  54,  99, 122, 127, 123,  99,  56,   0, -54, -99,-122
};

// ---------------------------------------------------------------------------
// Tiempo delta real -- ver cabecera del archivo. Idéntico mecanismo
// que asteroids.c: g_dt_scale es un factor Q8.8 (256 = "un tick
// nominal de 16ms"), y tanto la aceleración (gravedad/empuje) como
// la integración de posición se multiplican por él.
// ---------------------------------------------------------------------------
static absolute_time_t last_tick_time;
static int32_t g_dt_scale = FP;

static void update_dt_scale(void) {
    int64_t elapsed_us = absolute_time_diff_us(last_tick_time, get_absolute_time());
    last_tick_time = get_absolute_time();

    int32_t elapsed_ms = (int32_t)(elapsed_us / 1000);
    if (elapsed_ms < 1)  elapsed_ms = 1;
    if (elapsed_ms > 50) elapsed_ms = 50; // limita saltos tras una pausa larga (p.ej. highscores_enter())

    g_dt_scale = elapsed_ms * FP / 16;   // 16ms = referencia "1 tick nominal"
}

// ---------------------------------------------------------------------------
// Terreno -- generado por nivel, igual estructura que el original
// (segmentos + hasta 3 plataformas), reescalado al área de juego
// menor y con menos segmentos (más que suficiente resolución para
// 312px de ancho).
// ---------------------------------------------------------------------------
#define TERRAIN_SEGS  14
#define TERRAIN_PTS   (TERRAIN_SEGS + 1)
#define TERRAIN_Y_MIN (PLAY_Y + PLAY_H * 55 / 100)
#define TERRAIN_Y_MAX (PLAY_Y + PLAY_H - 10)

#define MAX_PADS  3
typedef struct { int x0, x1, y, width, score_mult, fuel_bonus; } LandPad;
static LandPad pads[MAX_PADS];
static int     n_pads;

static int terrain_x[TERRAIN_PTS];
static int terrain_y[TERRAIN_PTS];

#define FUEL_L1         1500
#define FUEL_STEP        100
#define FUEL_MIN         400
#define FUEL_COST_SOFT     1    // combustible por tick nominal, empuje suave
#define FUEL_COST_HARD     3    // combustible por tick nominal, empuje fuerte

static int ll_clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int ll_abs(int v) { return v < 0 ? -v : v; }
static int rnd(int n) { return n > 0 ? rand() % n : 0; }

// ---------------------------------------------------------------------------
// Estado de la nave / partida
// ---------------------------------------------------------------------------
#define BOOM_PARTS 14
typedef struct { int32_t x, y, vx, vy; int life; bool active; } BoomPart;
static BoomPart boom[BOOM_PARTS];

typedef enum { LL_READY, LL_PLAYING, LL_LANDED, LL_CRASHED, LL_GAME_OVER, LL_SCORES } LLState;

static LLState  state;
static int32_t  ship_x, ship_y, ship_vx, ship_vy;
static int      ship_angle;
static int      fuel;
static bool     engine_on;
static int      engine_thrust;   // 0, THRUST_SOFT o THRUST_HARD
static int      exhaust_anim;
static int      maneuver_ticks;  // ticks restantes de impulso de maniobra de aterrizaje
#define MANEUVER_DURATION   6    // ~0.1s a 60 ticks/s nominales

// ---------------------------------------------------------------------------
// Cámara / zoom -- recuperado del original: al bajar de cierta
// altitud sobre el terreno, la vista se amplía x2 centrada en la
// nave, para dar precisión en el tramo final de aterrizaje.
// Histéresis (ENTER/EXIT) para evitar parpadeo al entrar/salir justo
// en el umbral. Umbrales reescalados del original (100/170 sobre
// PLAY_H=400) al nuevo PLAY_H=234.
//
// Coste de SPI: a diferencia del resto del juego (render incremental
// con borrado por caja), MIENTRAS zoom_active está activo se hace un
// redibujado COMPLETO del área de juego cada tick (ver
// draw_zoomed_frame), igual que hacía el renderer de vídeo compuesto
// original -- porque la cámara se mueve cada tick y "borrar la caja
// anterior" ya no es válido bajo una transformación que cambió. Es
// más lento (el tramo final de aterrizaje va notablemente menos
// fluido), pero solo ocurre cerca del suelo, que es precisamente
// cuando el jugador quiere ir despacio y con precisión.
// ---------------------------------------------------------------------------
#define ZOOM_ALT_ENTER  55
#define ZOOM_ALT_EXIT   95
static bool zoom_active = false;
static bool field_needs_redraw = true;   // declarado aquí (no junto al resto de trazas de render, más abajo) porque update_zoom() lo necesita

static int     score, lives, level, blink, pause_ticks, demo_ticks, game_ticks, landed_pad;
static int     enc_acc;
static bool    demo;
static bool    g_done;

// fuel_start() depende de "level", declarado justo arriba -- se
// define aquí (tras el bloque de estado), no junto al resto de
// utilidades, precisamente para evitar el problema de orden de
// declaración que tenía el original.
static int fuel_start(void) { int f = FUEL_L1 - (level - 1) * FUEL_STEP; return f < FUEL_MIN ? FUEL_MIN : f; }

// ---------------------------------------------------------------------------
// Dibujo vectorial -- líneas por Bresenham (mismo estilo que asteroids.c)
// ---------------------------------------------------------------------------
static const int16_t SIN_TAB[32] = {
      0,  50,  98, 142, 181, 213, 237, 251,
    256, 251, 237, 213, 181, 142,  98,  50,
      0, -50, -98,-142,-181,-213,-237,-251,
   -256,-251,-237,-213,-181,-142, -98, -50
};
#define SINV(a)  SIN_TAB[(int)((a) * 32 / 360) & 31]
#define COSV(a)  SIN_TAB[((int)((a) * 32 / 360) + 8) & 31]

static void rot(int cx, int cy, int rx, int ry, int a, int *ox, int *oy) {
    *ox = cx + (rx * COSV(a) - ry * SINV(a)) / FP;
    *oy = cy + (rx * SINV(a) + ry * COSV(a)) / FP;
}

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
// Terreno -- generación (idéntica lógica al original, reescalada)
// ---------------------------------------------------------------------------
static void gen_terrain(void) {
    int seg_w = PLAY_W / TERRAIN_SEGS;
    for (int i = 0; i < TERRAIN_PTS; i++) terrain_x[i] = PLAY_X + i * seg_w;
    terrain_x[TERRAIN_PTS-1] = PLAY_X + PLAY_W;

    int lv = level < 1 ? 1 : level;
    int max_slope = 28 + (lv-1)*6;   // reescalado (original 55 sobre PLAY_H=400 -> ~55*234/400≈32, ajustado a mano)
    if (max_slope > 48) max_slope = 48;
    int flat_segs = (lv <= 2) ? 1 : 0;

    int y_range = TERRAIN_Y_MAX - TERRAIN_Y_MIN;
    int y_bias  = y_range * (lv-1) / 8;
    if (y_bias > y_range - 20) y_bias = y_range - 20;
    if (y_bias < 0) y_bias = 0;

    for (int i = 0; i < TERRAIN_PTS; i++)
        terrain_y[i] = TERRAIN_Y_MIN + y_bias/2 + rnd(y_range - y_bias);

    n_pads = (lv >= 3) ? 2 : 3;
    int pad_widths[3]      = {30, 20, 12};   // reescalado de {60,38,22} sobre PLAY_W=600
    int pad_mults[3]       = {1, 2, 4};
    int pad_fuel_bonus[3]  = {100, 200, 500};
    int zone_size = (TERRAIN_SEGS-4) / n_pads;
    if (zone_size < 1) zone_size = 1;

    for (int p = 0; p < n_pads; p++) {
        int pi = p;
        int pw = pad_widths[pi];
        if (lv >= 2 && pi == 0) pw = pad_widths[1];
        if (lv >= 3 && pi == 0) pw = pad_widths[2];

        int seg = 2 + p*zone_size + rnd(zone_size);
        if (seg >= TERRAIN_SEGS-2) seg = TERRAIN_SEGS-3;

        int pad_y;
        if (lv <= 2) {
            pad_y = TERRAIN_Y_MIN+20 + rnd(y_range-40);
        } else {
            int hard_band = y_range/3;
            if (p%2==0) pad_y = TERRAIN_Y_MIN+6  + rnd(hard_band);
            else        pad_y = TERRAIN_Y_MAX-6  - rnd(hard_band);
        }
        pad_y = ll_clamp(pad_y, TERRAIN_Y_MIN+3, TERRAIN_Y_MAX-3);

        int orig_cx = (terrain_x[seg]+terrain_x[seg+1])/2;
        terrain_x[seg]   = orig_cx - pw/2;
        terrain_x[seg+1] = orig_cx + pw/2;
        terrain_y[seg]   = pad_y;
        terrain_y[seg+1] = pad_y;
        if (flat_segs >= 1) {
            if (seg>0)             terrain_y[seg-1] = pad_y;
            if (seg+2<TERRAIN_PTS) terrain_y[seg+2] = pad_y;
        }

        pads[p].x0 = terrain_x[seg]; pads[p].x1 = terrain_x[seg+1];
        pads[p].y  = pad_y; pads[p].width = pw;
        pads[p].score_mult = pad_mults[pi]; pads[p].fuel_bonus = pad_fuel_bonus[pi];
    }

    for (int i = 1; i < TERRAIN_PTS-1; i++) {
        bool is_pad = false;
        for (int p = 0; p < n_pads; p++)
            if (terrain_x[i]==pads[p].x0 || terrain_x[i]==pads[p].x1) is_pad = true;
        if (is_pad) continue;
        int dy_l = terrain_y[i]-terrain_y[i-1];
        int dy_r = terrain_y[i+1]-terrain_y[i];
        if (ll_abs(dy_l) > max_slope) terrain_y[i]   = terrain_y[i-1] + (dy_l>0?max_slope:-max_slope);
        if (ll_abs(dy_r) > max_slope) terrain_y[i+1] = terrain_y[i]   + (dy_r>0?max_slope:-max_slope);
        terrain_y[i] = ll_clamp(terrain_y[i], TERRAIN_Y_MIN, TERRAIN_Y_MAX);
    }

    if (lv >= 3) {
        int n_peaks = 2 + (lv-3);
        if (n_peaks > 4) n_peaks = 4;
        for (int k = 0; k < n_peaks; k++) {
            int tries = 0;
            while (tries++ < 20) {
                int i = 2 + rnd(TERRAIN_SEGS-3);
                bool is_pad = false;
                for (int p = 0; p < n_pads; p++)
                    if (terrain_x[i]==pads[p].x0 || terrain_x[i]==pads[p].x1 ||
                        (i>0 && terrain_x[i-1]==pads[p].x0) ||
                        (i<TERRAIN_SEGS && terrain_x[i+1]==pads[p].x1)) is_pad = true;
                if (is_pad) continue;
                terrain_y[i] = TERRAIN_Y_MIN + rnd(y_range/3);
                break;
            }
        }
    }
}

static int terrain_y_at(int px) {
    for (int i = 0; i < TERRAIN_SEGS; i++) {
        if (px >= terrain_x[i] && px <= terrain_x[i+1]) {
            int dx = terrain_x[i+1]-terrain_x[i];
            if (dx == 0) return terrain_y[i];
            int t = (px-terrain_x[i]) * 256 / dx;
            return terrain_y[i] + (t*(terrain_y[i+1]-terrain_y[i]))/256;
        }
    }
    return TERRAIN_Y_MAX;
}

/*
 * Transforma una coordenada de MUNDO (px de pantalla en vista
 * normal) a coordenada de PANTALLA, aplicando la cámara de zoom si
 * está activa. Sin zoom es la identidad -- así todo el código de
 * dibujo del modo zoom (terreno/nave/partículas) puede reutilizar
 * las mismas coordenadas de mundo que el resto del juego sin
 * bifurcar la lógica de física.
 *
 * La nave NO se escala con el zoom (se dibuja al mismo tamaño,
 * igual que en el original) -- solo se amplían las distancias al
 * punto donde está la nave, dando sensación de acercamiento.
 */
#define ZOOM_FACTOR 2
static void world_to_screen(int wx, int wy, int *sx, int *sy) {
    if (!zoom_active) { *sx = wx; *sy = wy; return; }
    int spx = FP2PX(ship_x), spy = FP2PX(ship_y);
    *sx = CX + (wx - spx) * ZOOM_FACTOR;
    *sy = CY + (wy - spy) * ZOOM_FACTOR;
}

/*
 * Activa/desactiva el zoom según la altitud sobre el terreno, con
 * histéresis (ENTER/EXIT) para que no parpadee justo en el umbral.
 * Al SALIR del zoom, se fuerza field_needs_redraw para que la vista
 * normal (estática, incremental) se reconstruya desde cero -- la
 * última imagen dibujada en pantalla era la vista ampliada, que ya
 * no es válida como referencia para el borrado incremental normal.
 */
static void update_zoom(void) {
    int sx = FP2PX(ship_x), sy = FP2PX(ship_y);
    int gy = terrain_y_at(sx);
    int dist = gy - (sy + SHIP_RADIUS);
    if (dist < 0) dist = 0;

    bool was = zoom_active;
    if (!zoom_active && dist < ZOOM_ALT_ENTER) zoom_active = true;
    if ( zoom_active && dist > ZOOM_ALT_EXIT)  zoom_active = false;
    if (was && !zoom_active) field_needs_redraw = true;
}

// COLOR_PAD marca visualmente cada plataforma coloreando ESE tramo de
// terreno (en vez de postes/luces/texto flotante, ver cabecera del
// archivo -- esos elementos quedaban dentro del área de vuelo y no
// sobrevivirían al redibujado incremental sin lógica de restauración
// propia). La dificultad se sigue viendo por la anchura del tramo.
static bool is_pad_col(int x) {
    for (int p = 0; p < n_pads; p++)
        if (x >= pads[p].x0 && x < pads[p].x1) return true;
    return false;
}

#define COLOR_TERRAIN 0x8410   // gris medio (RGB565)
#define COLOR_PAD     COLOR_GREEN
#define COLOR_SHIP    COLOR_CYAN
#define COLOR_FLAME   COLOR_YELLOW
#define COLOR_BOOM    COLOR_YELLOW

static void terrain_col_fill(int x, uint16_t override_color, bool use_override) {
    int gy = terrain_y_at(x);
    int top = gy, bottom = PLAY_Y+PLAY_H;
    if (bottom <= top) return;
    uint16_t c = use_override ? override_color : (is_pad_col(x) ? COLOR_PAD : COLOR_TERRAIN);
    renderer_fill_rect(x, top, 1, bottom-top, c);
}

/*
 * Borra una caja (nave, partícula...) que puede estar total o
 * parcialmente sobre el cielo (negro) y/o sobre el terreno sólido.
 *
 * BUG corregido aquí: la versión anterior solo "redibujaba terreno"
 * desde el nivel del suelo hacia abajo, pero nunca pintaba de negro
 * la parte de la caja que cae en el cielo -- que es donde la nave
 * pasa la mayor parte del vuelo. Resultado: la posición anterior de
 * la nave nunca se borraba de verdad y se veía como un rastro.
 *
 * Ahora: 1) se pinta TODA la caja de negro (borra el cielo), y
 *        2) se repinta el terreno SOLO en la franja de la caja que
 *           cae por debajo de la línea de superficie en cada
 *           columna (para no dejar agujeros negros en el suelo
 *           cuando el objeto vuela bajo o aterriza).
 */
static void erase_box_over_terrain(int x, int y, int w, int h) {
    if (x < PLAY_X) { w += (x - PLAY_X); x = PLAY_X; }
    if (y < PLAY_Y) { h += (y - PLAY_Y); y = PLAY_Y; }
    if (w <= 0 || h <= 0) return;
    if (x + w > PLAY_X+PLAY_W) w = PLAY_X+PLAY_W - x;
    if (y + h > PLAY_Y+PLAY_H) h = PLAY_Y+PLAY_H - y;
    if (w <= 0 || h <= 0) return;

    renderer_fill_rect(x, y, w, h, COLOR_BLACK);

    for (int xs = x; xs < x + w; xs++) {
        int gy = terrain_y_at(xs);
        int top = gy < y ? y : gy;
        int bottom = y + h;
        if (bottom > top) {
            uint16_t c = is_pad_col(xs) ? COLOR_PAD : COLOR_TERRAIN;
            renderer_fill_rect(xs, top, 1, bottom-top, c);
        }
    }
}

// ---------------------------------------------------------------------------
// Colisión con el terreno / plataformas
// ---------------------------------------------------------------------------
static bool ship_on_ground(void) {
    int sx = FP2PX(ship_x), sy = FP2PX(ship_y);
    int foot_y = sy + SHIP_RADIUS;
    return foot_y >= terrain_y_at(sx);
}
static int landing_pad_index(void) {
    int sx = FP2PX(ship_x);
    for (int p = 0; p < n_pads; p++) if (sx >= pads[p].x0 && sx <= pads[p].x1) return p;
    return -1;
}
static bool is_safe_landing(int pi) {
    if (pi < 0) return false;
    if (ll_abs(ship_vy) > LAND_VY_MAX) return false;
    if (ship_vx != LAND_VX_MAX) return false;
    if (angle_to_sprite(ship_angle) != 0) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Spawns
// ---------------------------------------------------------------------------
static void spawn_ship(void) {
    ship_x = PX2FP(PLAY_X + PLAY_W/4); ship_y = PX2FP(PLAY_Y + 25);
    ship_vx = (1+rnd(2)) * FP; ship_vy = 0; ship_angle = 0;
    engine_on = false; engine_thrust = 0; exhaust_anim = 0; maneuver_ticks = 0;
}

static void spawn_boom(int cx, int cy) {
    for (int i = 0; i < BOOM_PARTS; i++) {
        boom[i].x = PX2FP(cx); boom[i].y = PX2FP(cy);
        boom[i].vx = (rnd(10)-5) * FP; boom[i].vy = (rnd(8)-6) * FP;
        boom[i].life = 14 + rnd(16); boom[i].active = true;
    }
}

static void update_boom(void) {
    for (int i = 0; i < BOOM_PARTS; i++) {
        if (!boom[i].active) continue;
        boom[i].vy += GRAVITY * g_dt_scale / FP;
        boom[i].x  += boom[i].vx * g_dt_scale / FP;
        boom[i].y  += boom[i].vy * g_dt_scale / FP;
        if (--boom[i].life <= 0) boom[i].active = false;
        int px = FP2PX(boom[i].x), py = FP2PX(boom[i].y);
        if (py > PLAY_Y+PLAY_H || px < PLAY_X || px > PLAY_X+PLAY_W) boom[i].active = false;
    }
}

static void level_init(void) {
    gen_terrain();
    spawn_ship();
    for (int i = 0; i < BOOM_PARTS; i++) boom[i].active = false;
    game_ticks = 0;
}
static void game_start(void) {
    score = 0; lives = 3; level = 1; blink = 0; enc_acc = 0;
    fuel = fuel_start();
    level_init();
}

// ---------------------------------------------------------------------------
// Dibujo -- nave vectorial (ver cabecera del archivo: sustituye al
// hexágono relleno con "ventana rotativa" del original, que pintaba
// pixel a pixel -- inasumible por SPI). Geometría local (y negativo
// = arriba):
//   Antena:      (0,-13) -> (0,-8)
//   Cabina:      hexágono (-5,-8)(5,-8)(7,0)(5,7)(-5,7)(-7,0)
//   Pata izq:    (-7,0)  -> (-12,11)   pie: (-12,11)->(-9,11)
//   Pata der:    ( 7,0)  -> ( 12,11)   pie: ( 12,11)->( 9,11)
//   Llama motor: (-3,7)->(0,7+len)  y  (3,7)->(0,7+len)
// ---------------------------------------------------------------------------
#define SHIP_BBOX_R 17

/*
 * scale: 1 en el juego (tamaño normal). La pantalla de inicio usa un
 * valor mayor para un icono grande y reconocible -- se aplica a la
 * geometría local ANTES de rotar, la línea sigue siendo del mismo
 * grosor (2px) a cualquier escala.
 */
static void draw_ship_at(int cx, int cy, int angle, bool engine, uint16_t color, int scale) {
    int hx[6], hy[6];
    static const int8_t vxl[6] = { -5,  5,  7,  5, -5, -7 };
    static const int8_t vyl[6] = { -8, -8,  0,  7,  7,  0 };
    for (int i = 0; i < 6; i++) rot(cx, cy, vxl[i]*scale, vyl[i]*scale, angle, &hx[i], &hy[i]);
    for (int i = 0; i < 6; i++) line(hx[i], hy[i], hx[(i+1)%6], hy[(i+1)%6], color);

    int ax0,ay0,ax1,ay1;
    rot(cx,cy, 0*scale,-13*scale, angle, &ax0,&ay0);
    rot(cx,cy, 0*scale, -8*scale, angle, &ax1,&ay1);
    line(ax0,ay0,ax1,ay1, color);

    int p1x,p1y,p2x,p2y,p3x,p3y;
    rot(cx,cy,-7*scale,0,      angle,&p1x,&p1y);
    rot(cx,cy,-12*scale,11*scale,angle,&p2x,&p2y);
    rot(cx,cy,-9*scale,11*scale, angle,&p3x,&p3y);
    line(p1x,p1y,p2x,p2y, color);
    line(p2x,p2y,p3x,p3y, color);

    rot(cx,cy, 7*scale,0,      angle,&p1x,&p1y);
    rot(cx,cy, 12*scale,11*scale,angle,&p2x,&p2y);
    rot(cx,cy, 9*scale,11*scale, angle,&p3x,&p3y);
    line(p1x,p1y,p2x,p2y, color);
    line(p2x,p2y,p3x,p3y, color);

    if (engine && (blink%4)<3) {
        int fl = (5 + (exhaust_anim%3)*3) * scale;
        int f1x,f1y,f2x,f2y,fmx,fmy;
        rot(cx,cy,-3*scale,7*scale, angle,&f1x,&f1y);
        rot(cx,cy, 3*scale,7*scale, angle,&f2x,&f2y);
        rot(cx,cy, 0,7*scale+fl, angle,&fmx,&fmy);
        line(f1x,f1y,fmx,fmy, COLOR_FLAME);
        line(f2x,f2y,fmx,fmy, COLOR_FLAME);
    }
}

// ---------------------------------------------------------------------------
// Render incremental -- mismo patrón que asteroids.c: se guarda la
// posición previa de cada elemento y solo se borra+redibuja si algo
// cambió. A diferencia de asteroids.c, borrar aquí implica primero
// restaurar el terreno bajo la caja (ver erase_box_over_terrain), porque
// el fondo no es un campo vacío sino terreno sólido.
// ---------------------------------------------------------------------------
static int   prev_ship_x = -1000, prev_ship_y = -1000;
static bool  prev_ship_alive = false;
static int   prev_boom_x[BOOM_PARTS], prev_boom_y[BOOM_PARTS];
static bool  prev_boom_active[BOOM_PARTS];
static int   prev_score = -1, prev_lives = -1, prev_level_hud = -1, prev_fuel = -1;
static int   prev_vx_disp = -1, prev_vy_disp = -1;
static char  prev_center_msg[32] = "";
static char  prev_bottom_msg[40] = "";

static void reset_render_trace(void) {
    prev_ship_alive = false;
    for (int i = 0; i < BOOM_PARTS; i++) prev_boom_active[i] = false;
    prev_score = prev_lives = prev_level_hud = prev_fuel = -1;
    prev_vx_disp = prev_vy_disp = -1;
    prev_center_msg[0] = '\0';
    prev_bottom_msg[0] = '\0';
}

static void draw_field_static(void) {
    renderer_clear(COLOR_BLACK);
    for (int x = PLAY_X; x < PLAY_X+PLAY_W; x++) terrain_col_fill(x, 0, false);
    reset_render_trace();
    field_needs_redraw = false;
    renderer_flush();
}

static void draw_ship_if_moved(bool alive) {
    int cx = FP2PX(ship_x), cy = FP2PX(ship_y);
    if (!alive && !prev_ship_alive) return;

    if (prev_ship_alive)
        erase_box_over_terrain(prev_ship_x-SHIP_BBOX_R, prev_ship_y-SHIP_BBOX_R, SHIP_BBOX_R*2, SHIP_BBOX_R*2);
    if (alive)
        draw_ship_at(cx, cy, ship_angle, engine_on, COLOR_SHIP, 1);

    prev_ship_x = cx; prev_ship_y = cy; prev_ship_alive = alive;
    renderer_flush();
}

static void draw_boom_if_moved(void) {
    bool any = false;
    for (int i = 0; i < BOOM_PARTS; i++) {
        int x = FP2PX(boom[i].x), y = FP2PX(boom[i].y);
        int sz = (boom[i].life>15) ? 4 : (boom[i].life>8) ? 3 : 2;
        bool show = boom[i].active;
        if (!show && !prev_boom_active[i]) continue;

        if (prev_boom_active[i])
            erase_box_over_terrain(prev_boom_x[i]-2, prev_boom_y[i]-2, 5, 5);
        if (show)
            renderer_fill_rect(x-sz/2, y-sz/2, sz, sz, COLOR_BOOM);

        prev_boom_x[i] = x; prev_boom_y[i] = y; prev_boom_active[i] = show;
        any = true;
    }
    if (any) renderer_flush();
}

// ---------------------------------------------------------------------------
// Modo ZOOM -- redibujado COMPLETO del área de juego cada tick (ver
// comentario largo junto a ZOOM_ALT_ENTER/EXIT). Terreno, partículas
// y nave se transforman con world_to_screen() y se pintan en orden
// fijo (terreno -> partículas -> nave -> HUD encima de todo),
// exactamente como hacía el renderer del original en CADA frame del
// juego -- aquí solo durante el tramo final de aterrizaje.
// ---------------------------------------------------------------------------
static void draw_terrain_zoomed(void) {
    int floor_sx, floor_sy;
    world_to_screen(PLAY_X, PLAY_Y+PLAY_H, &floor_sx, &floor_sy);
    (void)floor_sx;
    floor_sy = ll_clamp(floor_sy, PLAY_Y, PLAY_Y+PLAY_H);

    for (int i = 0; i < TERRAIN_SEGS; i++) {
        int x0w = terrain_x[i],   y0w = terrain_y[i];
        int x1w = terrain_x[i+1], y1w = terrain_y[i+1];
        int x0s, y0s, x1s, y1s;
        world_to_screen(x0w, y0w, &x0s, &y0s);
        world_to_screen(x1w, y1w, &x1s, &y1s);
        if (x0s > x1s) { int t=x0s;x0s=x1s;x1s=t; t=y0s;y0s=y1s;y1s=t; }
        if (x1s <= PLAY_X || x0s >= PLAY_X+PLAY_W) continue;

        bool is_pad = false;
        for (int p = 0; p < n_pads; p++)
            if (x0w == pads[p].x0 && x1w == pads[p].x1) is_pad = true;
        uint16_t col = is_pad ? COLOR_PAD : COLOR_TERRAIN;

        int dx = x1s - x0s;
        int xs_start = x0s < PLAY_X ? PLAY_X : x0s;
        int xs_end   = x1s > PLAY_X+PLAY_W ? PLAY_X+PLAY_W : x1s;
        if (dx <= 0) {
            int ys = ll_clamp(y0s, PLAY_Y, floor_sy);
            int h = floor_sy - ys;
            if (h > 0) renderer_fill_rect(ll_clamp(xs_start, PLAY_X, PLAY_X+PLAY_W-1), ys, 1, h, col);
            continue;
        }
        for (int xs = xs_start; xs < xs_end; xs++) {
            int t = (xs - x0s) * 256 / dx;
            t = ll_clamp(t, 0, 256);
            int ys = y0s + (t*(y1s-y0s))/256;
            ys = ll_clamp(ys, PLAY_Y, floor_sy);
            int h = floor_sy - ys;
            if (h > 0) renderer_fill_rect(xs, ys, 1, h, col);
        }
    }
}

static void draw_boom_zoomed(void) {
    for (int i = 0; i < BOOM_PARTS; i++) {
        if (!boom[i].active) continue;
        int sx, sy;
        world_to_screen(FP2PX(boom[i].x), FP2PX(boom[i].y), &sx, &sy);
        int sz = (boom[i].life>15) ? 4 : (boom[i].life>8) ? 3 : 2;
        if (sx >= PLAY_X && sx < PLAY_X+PLAY_W && sy >= PLAY_Y && sy < PLAY_Y+PLAY_H)
            renderer_fill_rect(sx-sz/2, sy-sz/2, sz, sz, COLOR_BOOM);
    }
}

// ---------------------------------------------------------------------------
// HUD -- 3 columnas en la franja superior (que queda libre de
// terreno, ver TERRAIN_Y_MIN), solo redibuja lo que cambia, mismo
// patrón que draw_hud_if_changed() en asteroids.c.
// ---------------------------------------------------------------------------
static absolute_time_t next_hud_refresh;
static absolute_time_t ready_input_ok_time;   // ver LL_READY en ll_tick()

static void draw_hud_if_changed(bool force_all) {
    char buf[24];
    bool changed = false;
    bool force = force_all || time_reached(next_hud_refresh);
    if (time_reached(next_hud_refresh)) next_hud_refresh = make_timeout_time_ms(700);

    if (score != prev_score || force) {
        renderer_fill_rect(PLAY_X+2, PLAY_Y+3, 90, 14, COLOR_BLACK);
        snprintf(buf, sizeof(buf), "%d", score);
        renderer_draw_text(PLAY_X+2, PLAY_Y+3, buf, COLOR_WHITE, COLOR_BLACK, 2);
        prev_score = score; changed = true;
    }
    if (lives != prev_lives || force) {
        renderer_fill_rect(PLAY_X+2, PLAY_Y+20, 90, 10, COLOR_BLACK);
        snprintf(buf, sizeof(buf), "VIDAS %d", lives);
        renderer_draw_text(PLAY_X+2, PLAY_Y+20, buf, COLOR_WHITE, COLOR_BLACK, 1);
        prev_lives = lives; changed = true;
    }
    if (level != prev_level_hud || force) {
        renderer_fill_rect(CX-40, PLAY_Y+3, 80, 14, COLOR_BLACK);
        snprintf(buf, sizeof(buf), "NIV %d", level);
        renderer_draw_text(centered_x(buf, 2), PLAY_Y+3, buf, COLOR_WHITE, COLOR_BLACK, 2);
        prev_level_hud = level; changed = true;
    }
    if (fuel != prev_fuel || force) {
        int bar_w = 120, bar_x = CX-bar_w/2, bar_y = PLAY_Y+20, bar_h = 10;
        renderer_fill_rect(bar_x, bar_y, bar_w, bar_h, COLOR_BLACK);
        renderer_fill_rect(bar_x, bar_y, bar_w, 1, COLOR_WHITE);
        renderer_fill_rect(bar_x, bar_y+bar_h-1, bar_w, 1, COLOR_WHITE);
        renderer_fill_rect(bar_x, bar_y, 1, bar_h, COLOR_WHITE);
        renderer_fill_rect(bar_x+bar_w-1, bar_y, 1, bar_h, COLOR_WHITE);
        int fs = fuel_start(); if (fs<=0) fs=1;
        int filled = fuel*(bar_w-2)/fs;
        filled = ll_clamp(filled, 0, bar_w-2);
        if (filled>0) renderer_fill_rect(bar_x+1, bar_y+1, filled, bar_h-2, COLOR_GREEN);
        prev_fuel = fuel; changed = true;
    }

    // HUD velocidades x e y
    int vx_disp = FP2PX(ship_vx* 100) ;
    int vy_disp = FP2PX(ship_vy* 100) ;
    
    if (vx_disp != prev_vx_disp || force) {
        // Ampliamos el ancho del rectángulo de borrado de 90 a 105 para alojar texto más largo
        renderer_fill_rect(PLAY_X+PLAY_W-105, PLAY_Y+3, 105, 14, COLOR_BLACK);
        snprintf(buf, sizeof(buf), "HX %3d", vx_disp); // %3d asegura los 3 dígitos
        int x = PLAY_X+PLAY_W-2-(int)st7789_text_width(buf, 2);
        uint16_t c = (ship_vx != 0) ? COLOR_YELLOW : COLOR_WHITE;
        renderer_draw_text(x, PLAY_Y+3, buf, c, COLOR_BLACK, 2);
        prev_vx_disp = vx_disp; changed = true;
    }
    
    if (vy_disp != prev_vy_disp || force) {
        renderer_fill_rect(PLAY_X+PLAY_W-105, PLAY_Y+20, 105, 10, COLOR_BLACK);
        snprintf(buf, sizeof(buf), "VY %3d", vy_disp); // %3d asegura los 3 dígitos
        uint16_t c = (vy_disp > FP2PX(LAND_VY_MAX)) ? COLOR_YELLOW : COLOR_WHITE;
        renderer_draw_text(PLAY_X+PLAY_W-2-(int)st7789_text_width(buf,2), PLAY_Y+20, buf, c, COLOR_BLACK, 2);
        prev_vy_disp = vy_disp; changed = true;
    }
    
    if (changed) renderer_flush();
}

static void update_center_message(const char *target, uint16_t color, int scale) {
    if (strcmp(target, prev_center_msg) == 0) return;
    renderer_fill_rect(0, CY-16, TFT_WIDTH, 32, COLOR_BLACK);
    if (target[0]) renderer_draw_text(centered_x(target, scale), CY-10, target, color, COLOR_BLACK, scale);
    strncpy(prev_center_msg, target, sizeof(prev_center_msg)-1);
    prev_center_msg[sizeof(prev_center_msg)-1] = '\0';
    renderer_flush();
}
static void update_bottom_message(const char *target, int scale) {
    if (strcmp(target, prev_bottom_msg) == 0) return;
    renderer_fill_rect(0, PLAY_Y+PLAY_H-18, TFT_WIDTH, 16, COLOR_BLACK);
    if (target[0]) renderer_draw_text(centered_x(target, scale), PLAY_Y+PLAY_H-16, target, COLOR_WHITE, COLOR_BLACK, scale);
    strncpy(prev_bottom_msg, target, sizeof(prev_bottom_msg)-1);
    prev_bottom_msg[sizeof(prev_bottom_msg)-1] = '\0';
    renderer_flush();
}

static void draw_zoomed_frame(void) {
    renderer_fill_rect(PLAY_X, PLAY_Y, PLAY_W, PLAY_H, COLOR_BLACK);
    draw_terrain_zoomed();
    draw_boom_zoomed();

    bool ship_alive = (state == LL_PLAYING || state == LL_LANDED);
    if (ship_alive) {
        int sx, sy;
        world_to_screen(FP2PX(ship_x), FP2PX(ship_y), &sx, &sy);
        draw_ship_at(sx, sy, ship_angle, engine_on, COLOR_SHIP, 1);
    }
    draw_hud_if_changed(true);
    renderer_flush();
}

static void draw_playing_frame(void) {
    if (zoom_active) {
        draw_zoomed_frame();
        // El frame completo del modo zoom ya limpió y redibujó TODO el
        // área de juego, incluida la franja de los mensajes -- fuerza a
        // que se repinten aunque el texto no haya cambiado respecto al
        // tick anterior, o quedarían "borrados" por el clear de arriba.
        prev_center_msg[0] = '\0';
        prev_bottom_msg[0] = '\0';
    } else {
        if (field_needs_redraw) draw_field_static();
        bool ship_alive = (state == LL_PLAYING || state == LL_LANDED);
        draw_boom_if_moved();
        draw_ship_if_moved(ship_alive);
        draw_hud_if_changed(false);
    }

    bool bon = (blink/15)%2==0;
    const char *center = "";
    int center_scale = 3;
    if (state == LL_LANDED && bon)      center = "ATERRIZAJE!";
    else if (state == LL_CRASHED && bon) center = "CRASH!";
    else if (state == LL_GAME_OVER)      center = "GAME OVER";
    update_center_message(center, COLOR_YELLOW, center_scale);

    char bottom[40] = "";
    if (state == LL_LANDED) {
        int pts = fuel*pads[landed_pad].score_mult + level*100;
        snprintf(bottom, sizeof(bottom), "+%d PTS (x%d)  +%d FUEL", pts, pads[landed_pad].score_mult, pads[landed_pad].fuel_bonus);
    } else if (state == LL_GAME_OVER && bon) {
        snprintf(bottom, sizeof(bottom), "PULSA PARA CONTINUAR");
    } else if (demo && bon) {
        snprintf(bottom, sizeof(bottom), "DEMO - PULSA PARA JUGAR");
    } else if (zoom_active && engine_on) {
        snprintf(bottom, sizeof(bottom), "%s - ZOOM x2", engine_thrust==THRUST_HARD ? "MOTOR MAX" : "MOTOR");
    } else if (zoom_active) {
        snprintf(bottom, sizeof(bottom), "ZOOM x2");
    } else if (engine_on) {
        snprintf(bottom, sizeof(bottom), engine_thrust==THRUST_HARD ? "MOTOR MAX" : "MOTOR");
    }
    update_bottom_message(bottom, 1);
}

// ---------------------------------------------------------------------------
// Pantallas "estáticas" -- redibujadas enteras solo al entrar en el
// estado, como en asteroids.c
// ---------------------------------------------------------------------------
static void draw_ready_screen(void) {
    renderer_clear(COLOR_BLACK);

    renderer_draw_text(centered_x("LUNAR LANDER", 3), 4, "LUNAR LANDER", COLOR_CYAN, COLOR_BLACK, 3);

    // Icono grande de la nave (misma geometría vectorial que en el
    // juego, ampliada x3), con la llama del motor encendida, apoyada
    // sobre una pequeña franja que hace de "suelo" para reforzar la
    // temática de aterrizaje.
    draw_ship_at(CX, 78, 0, true, COLOR_SHIP, 3);
    renderer_fill_rect(CX-45, 114, 90, 3, COLOR_TERRAIN);

    const char *l1 = "GIRA: ORIENTAR NAVE";
    const char *l2 = "A: SUAVE   B: FUERTE";
    const char *l3 = "ENCODER: MANIOBRA";
    const char *l4 = "MENOS ANCHO = MAS PUNTOS";
    const char *l5 = "PULSA PARA JUGAR";

    renderer_draw_text(centered_x(l1, 2), 126, l1, COLOR_WHITE,  COLOR_BLACK, 2);
    renderer_draw_text(centered_x(l2, 2), 148, l2, COLOR_WHITE,  COLOR_BLACK, 2);
    renderer_draw_text(centered_x(l3, 2), 170, l3, COLOR_WHITE,  COLOR_BLACK, 2);
    renderer_draw_text(centered_x(l4, 1), 194, l4, COLOR_GREEN,  COLOR_BLACK, 1);
    renderer_draw_text(centered_x(l5, 2), 212, l5, COLOR_YELLOW, COLOR_BLACK, 2);

    prev_bottom_msg[0] = '\0';
    prev_center_msg[0] = '\0';
    renderer_flush();
}

static void draw_scores_screen(void) {
    renderer_clear(COLOR_BLACK);
    highscores_draw(LL_GAME_ID, "LUNAR LANDER", 20);
    renderer_flush();
}

// ---------------------------------------------------------------------------
// IA de la demo -- adaptada del original (sin cámara de zoom, las
// coordenadas de mundo ya son coordenadas de pantalla directamente)
// ---------------------------------------------------------------------------
static void demo_ai(void) {
    int pad_cx = (pads[0].x0+pads[0].x1)/2;
    int sx = FP2PX(ship_x), vx = FP2PX(ship_vx), vy = FP2PX(ship_vy);
    int err_x = pad_cx - sx;
    int target = 0;
    int pd_x = err_x - vx*6;
    if (pd_x > 12) target = 18; else if (pd_x < -12) target = 342;
    int da = (target - ship_angle + 360) % 360; if (da > 180) da -= 360;
    if (da > 1) ship_angle = (ship_angle + ROT_SPEED + 360) % 360;
    else if (da < -1) ship_angle = (ship_angle - ROT_SPEED + 360) % 360;
    int dist_y = pads[0].y - FP2PX(ship_y);
    engine_on = fuel>0 && (vy>4 || (dist_y>30 && vy>1));
    engine_thrust = THRUST_SOFT;
}

// ---------------------------------------------------------------------------
// Tick principal
// ---------------------------------------------------------------------------
static void ll_tick(void) {
    blink++;
    update_dt_scale();

    if (demo) {
        bool any = controls_menu_select() || controls_get_raw_delta(0) != 0
                || controls_button_down(BTN_J1_A) || controls_button_down(BTN_J1_B);
        if (any || ++demo_ticks >= TICKS_S * 30) { g_done = true; return; }
    }

    switch (state) {

    // ------------------------------------------------------------------
    case LL_READY:
        // Ignora pulsaciones durante un breve instante al entrar en la
        // pantalla de inicio -- evita que una pulsación residual del
        // botón usado para SELECCIONAR este juego en el menú anterior
        // salte inmediatamente a la partida antes de que la pantalla
        // llegue a verse.
        if (time_reached(ready_input_ok_time) && controls_menu_select()) {
            sound_stop_menu_music();
            game_start();
            reset_render_trace();
            field_needs_redraw = true;
            state = LL_PLAYING;
        }
        break;

    // ------------------------------------------------------------------
    case LL_PLAYING: {
        game_ticks++; exhaust_anim++;

        if (demo) {
            demo_ai();
        } else {
            int d = controls_get_raw_delta(0);
            if (d) {
                enc_acc += d;
                int steps = enc_acc/2;
                if (steps) { ship_angle = (ship_angle + steps*ROT_SPEED + 360) % 360; enc_acc -= steps*2; }
            }

            /*
             * Corregido: antes exigía que la nave YA estuviera casi
                Maniobra de aterrizaje solo si se está a baja altura y la velocidad es baja
             */
             if (controls_button_pressed(BTN_ENC1_SW) && fuel > 0) {
             int vx_disp = ll_abs(FP2PX(ship_vx));
             int vy_disp = ll_abs(FP2PX(ship_vy));
    
             if (zoom_active && vx_disp <= 2 && vy_disp <= 15 && angle_to_sprite(ship_angle) == 0) {
               ship_angle = 0; 
               ship_vx = 0;
               maneuver_ticks = MANEUVER_DURATION;
               sound_effect_select();
             } else {
               sound_effect_lose_point();
             }
}

            bool soft = controls_button_down(BTN_J1_A);
            bool hard = controls_button_down(BTN_J1_B);
            engine_on = soft || hard;
            engine_thrust = hard ? THRUST_HARD : THRUST_SOFT;
        }

        ship_vy += GRAVITY * g_dt_scale / FP;

        if (maneuver_ticks > 0 && fuel > 0) {
            int spr = angle_to_sprite(ship_angle);
            ship_vx += (int)thrust_vx[spr] * THRUST_HARD / 127 * g_dt_scale / FP;
            ship_vy += (int)thrust_vy[spr] * THRUST_HARD / 127 * g_dt_scale / FP;
            int cost = (FUEL_COST_HARD * g_dt_scale) / FP; if (cost<1) cost=1;
            fuel -= cost; if (fuel<0) fuel=0;
            engine_on = true; engine_thrust = THRUST_HARD;
            maneuver_ticks--;
        } else if (engine_on && fuel > 0) {
            int spr = angle_to_sprite(ship_angle);
            ship_vx += (int)thrust_vx[spr] * engine_thrust / 127 * g_dt_scale / FP;
            ship_vy += (int)thrust_vy[spr] * engine_thrust / 127 * g_dt_scale / FP;
            int base_cost = (engine_thrust==THRUST_HARD) ? FUEL_COST_HARD : FUEL_COST_SOFT;
            int cost = (base_cost * g_dt_scale) / FP; if (cost<1) cost=1;
            fuel -= cost; if (fuel<0) fuel=0;
            // Cooldown de sonido: máx una llamada cada ~12 ticks nominales,
            // igual que el original, para no saturar el canal de efectos
            // con el botón mantenido.
            if ((exhaust_anim%12)==0) sound_effect_thrust();
        } else { engine_on = false; engine_thrust = 0; }

        ship_vx = ll_clamp(ship_vx, -MAX_VX, MAX_VX);
        ship_vy = ll_clamp(ship_vy, -MAX_VY, MAX_VY);
        ship_x += ship_vx * g_dt_scale / FP;
        ship_y += ship_vy * g_dt_scale / FP;

        int sxp = FP2PX(ship_x);
        if (sxp < PLAY_X)          ship_x = PX2FP(PLAY_X+PLAY_W-1);
        if (sxp > PLAY_X+PLAY_W)   ship_x = PX2FP(PLAY_X+1);
        if (FP2PX(ship_y) < PLAY_Y) { ship_y = PX2FP(PLAY_Y); ship_vy = 0; }

        update_zoom();

        if (ship_on_ground()) {
            int pi = landing_pad_index();
            if (is_safe_landing(pi)) {
                landed_pad = pi;
                int pts = fuel*pads[pi].score_mult + level*100;
                if (pts<0) pts=0;
                score += pts;
                fuel = ll_clamp(fuel+pads[pi].fuel_bonus, 0, fuel_start());
                sound_effect_success();
                zoom_active = false; field_needs_redraw = true;
                pause_ticks = 0; state = LL_LANDED;
            } else {
                spawn_boom(FP2PX(ship_x), FP2PX(ship_y));
                sound_effect_explosion();
                lives--;
                fuel += 200; if (fuel>1500) fuel=1500;
                zoom_active = false; field_needs_redraw = true;
                pause_ticks = 0; state = LL_CRASHED;
            }
        }
        draw_playing_frame();
        break;
    }

    // ------------------------------------------------------------------
    case LL_LANDED:
        if (++pause_ticks >= TICKS_S*3) { level++; level_init(); enc_acc=0; field_needs_redraw=true; state=LL_PLAYING; }
        draw_playing_frame();
        break;

    // ------------------------------------------------------------------
    case LL_CRASHED:
        update_boom();
        if (++pause_ticks >= TICKS_S*2) {
            if (lives <= 0) { pause_ticks = 0; state = LL_GAME_OVER; }
            else { spawn_ship(); enc_acc = 0; state = LL_PLAYING; }
        }
        draw_playing_frame();
        break;

    // ------------------------------------------------------------------
    case LL_GAME_OVER:
        if (++pause_ticks > TICKS_S) {
            if (controls_menu_select() || pause_ticks > TICKS_S*8) {
                if (!demo && highscores_is_top(LL_GAME_ID, (uint32_t)score)) {
                    highscores_enter(LL_GAME_ID, (uint32_t)score); // bloqueante
                }
                pause_ticks = 0;
                state = LL_SCORES;
                draw_scores_screen();
            }
        }
        break;

    // ------------------------------------------------------------------
    case LL_SCORES:
        if (++pause_ticks > TICKS_S*8) g_done = true;
        if (controls_menu_select()) g_done = true;
        break;
    }
}

// ---------------------------------------------------------------------------
// API pública
// ---------------------------------------------------------------------------
void game_lunar_lander_run(game_mode_t mode) {
    srand(time_us_32());

    demo = (mode == GAME_MODE_DEMO);
    blink = 0; demo_ticks = 0; enc_acc = 0; g_done = false;
    field_needs_redraw = true;
    for (int i = 0; i < BOOM_PARTS; i++) boom[i].active = false;
    reset_render_trace();
    last_tick_time = get_absolute_time();

    if (demo) {
        level = 1; lives = 1; score = 0;
        fuel = fuel_start();
        level_init();
        state = LL_PLAYING;
    } else {
        state = LL_READY;
        draw_ready_screen();
        sound_start_menu_music();
        ready_input_ok_time = make_timeout_time_ms(300);
    }

    while (!g_done) {
        controls_update();
        ll_tick();
        sound_update();
        sleep_ms(8);
    }

    highscores_flush();
}