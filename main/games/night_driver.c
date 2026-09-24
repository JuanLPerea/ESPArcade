/**
 * night_driver.c -- port de Night Driver (Atari, 1976 / versión ArcadePi)
 * al motor de ArcadeColor (Pico + ST7789 320x240), siguiendo el mismo
 * patrón que scramble.c: bucle propio dentro de game_night_driver_run(),
 * controls_update()/controls_get_raw_delta()/controls_button_*, sonido
 * vía sound_effect_*(), y highscores.h con highscores_enter() bloqueante
 * (sin estado propio para pedir iniciales).
 *
 * ---------------------------------------------------------------------
 * ESCALADO 768x576 -> 320x240
 * ---------------------------------------------------------------------
 * El nightdriver.c original (ArcadePi) se diseñó para un lienzo de
 * 768x576. 320/768 == 240/576 == 5/12 exactamente, así que la relación
 * de aspecto 4:3 se conserva y toda magnitud en píxeles del original
 * se reescala por el MISMO factor 5/12 -- geometría de carretera,
 * salpicadero, límites de giro del volante y amplitud de las curvas.
 * Así se conserva el equilibrio de diseño original entre "cuánto gira
 * el volante" y "cuánto curva la carretera" (ver STEER_GAIN/STEER_LIMIT
 * más abajo). Las duraciones en ticks (rectas, curvas, cuenta atrás)
 * NO se reescalan: son de por sí independientes del tamaño de pantalla,
 * y la diferencia TICKS_S 62->60 es insignificante para el "feel".
 *
 * ---------------------------------------------------------------------
 * SUPOSICIONES (avísame si alguna no encaja con el resto del proyecto)
 * ---------------------------------------------------------------------
 * 1) Volante = encoder 1 (J1), leído con controls_get_raw_delta(0) --
 *    es justo la API "sin filtrar, pensada para control analógico
 *    continuo" que describe el comentario de controls.c, ideal para
 *    un volante. BTN_J1_A = acelerador, BTN_J1_B = freno (mantenidos,
 *    con controls_button_down -- no son pulsaciones de un solo golpe).
 * 2) BTN_ENC2_SW (el pulsador del encoder 2, libre porque este juego
 *    es de 1 jugador) lo uso como "salir al menú" en cualquier momento,
 *    igual que el SW1 dedicado del ArcadePi original. Si el resto de
 *    juegos de ArcadeColor usa otro botón para esto, dímelo y lo cambio.
 * 3) Sonido: no existe sound_set_music(MUSIC_NIGHTDRIVER) ni
 *    sound_set_engine_rpm() en el motor nuevo, así que no hay zumbido
 *    de motor continuo -- uso sound_effect_explosion() en choque/fin
 *    de tiempo, sound_effect_success() en bonus de etapa, y la música
 *    de menú (sound_start_menu_music/stop) solo en la pantalla de
 *    título, igual que scramble.c.
 * 4) HUD: el original tenía 4 cuadrantes con SCORE/SPEED/KM/TIME a
 *    768px de ancho. A 320px no entran cómodos los "0000/9999 KM" de
 *    barra de progreso Y el resto, así que sustituyo la columna KM por
 *    "STAGE n" + "% hasta la siguiente etapa" -- mismo dato (progreso
 *    hacia el bonus), más compacto.
 *
 * ---------------------------------------------------------------------
 * AJUSTE DE DIFICULTAD (2ª pasada, tras probarlo: "muy fácil, poco
 * arcade")
 * ---------------------------------------------------------------------
 * El primer pase escalaba TODAS las magnitudes en píxeles por el mismo
 * 5/12, incluido el margen entre el ancho de la carretera y el ancho
 * del coche -- y ese margen creció desproporcionadamente respecto a lo
 * que realmente hace falta desviarse con el volante para notarlo, así
 * que salirse de la carretera era casi imposible. Cambios:
 *   - ROAD_HW_NEAR y CAR_HALF_WIDTH ya NO se derivan del original por
 *     escala; se fijan directamente para que haga falta bastante menos
 *     desviación para chocar.
 *   - Probabilidad/amplitud de curva más altas desde el stage 1, y la
 *     curva "se asienta" más rápido (menos ticks de suavizado), para
 *     que haga falta corregir con el volante de forma más continua.
 *   - Aceleración y frenada más secas (menos ticks hasta velocidad
 *     máxima / hasta parar).
 *   - Choque más "caro": más ticks de penalización de tiempo, más
 *     partículas, y un flash rojo en el borde de la pantalla durante
 *     los primeros frames tras el impacto.
 * Sigue siendo un primer ajuste a ojo -- si en la placa real sigue sin
 * sentirse bien, lo más fácil de tocar primero es ROAD_HW_NEAR (ancho
 * de carretera) y el divisor "/5" de road_curve_vel (velocidad de
 * asentamiento de la curva).
 *
 * ---------------------------------------------------------------------
 * 3ª pasada (feedback: capó invertido, debía moverse todo junto, deriva
 * parado, carretera más corta que antes)
 * ---------------------------------------------------------------------
 *   - draw_dashboard() ya no inclina solo el borde superior del capó:
 *     capó + volante + espejos se trasladan juntos como un único bloque
 *     (variable "lean", aplicada de una vez a "dx" al principio de la
 *     función). El signo de "lean" y el de rot_step (giro del aro del
 *     volante) van ahora en el mismo sentido -- si en la placa real ves
 *     que el coche se inclina al lado contrario de como gira realmente
 *     el volante físico, es un único signo global que invertir (basta
 *     con cambiar el signo de STEER_GAIN o el de "enc" al leerlo).
 *   - update_curve_and_posts() ya no aplica steer_vel a los postes si
 *     road_speed==0: antes la carretera "derivaba" aunque estuvieras
 *     parado, porque el giro del volante se restaba a los postes
 *     igualmente. El volante sigue pudiendo girar visualmente en el
 *     salpicadero estando parado (es cosmético), pero ya no mueve nada.
 *   - DASH_H (alto reservado para el salpicadero) baja de ~49 a 26px, y
 *     BASE_Y/ROAD_RANGE pasan a derivarse de DASH_H en vez de al revés
 *     -- la carretera vuelve a dibujarse bastante más abajo (ROAD_RANGE
 *     131->154px) a costa de un salpicadero más fino.
 *
 * ---------------------------------------------------------------------
 * 4ª pasada (feedback: la carretera se veía tapada por el recuadro del
 * salpicadero, el coche debía rotar en perspectiva no solo trasladarse,
 * y más velocidad punta)
 * ---------------------------------------------------------------------
 *   - draw_road() recorta ahora el cuadrado de cada poste para que no
 *     invada la franja del salpicadero (ni la línea de horizonte por
 *     arriba): los postes más cercanos (los más grandes) asomaban un
 *     par de píxeles dentro de esa franja y draw_dashboard() los
 *     tapaba con su fondo negro al dibujarse después en el mismo frame.
 *   - draw_dashboard() separa dos efectos: "lean" (todo el bloque --
 *     volante, espejos, centro del capó -- se traslada junto) y "skew"
 *     (el morro del capó se sesga entre su borde hacia el horizonte y
 *     su borde pegado al salpicadero, dando sensación de giro/rotación
 *     en perspectiva, no solo desplazamiento).
 *   - SPEED_MAX sube de PX2FP(5) a PX2FP(8). SPEED_ACCEL/BRAKE/DRAG
 *     pasan a expresarse como SPEED_MAX/ticks en vez de constantes fijas,
 *     así que el TIEMPO hasta velocidad máxima (~1.5s) y hasta frenar
 *     (~1.1s) no cambia aunque cambie SPEED_MAX -- solo cambia lo rápido
 *     que se ve pasar la carretera.
 *
 * ---------------------------------------------------------------------
 * 5ª pasada (arrancaba en curva -- ya arreglado en init_posts; y ahora:
 * curvas más cortas y variadas, para exigir corrección continua)
 * ---------------------------------------------------------------------
 *   - next_curve_segment() ya no alterna en una secuencia fija (recta/
 *     chicane según fase par, izda/dcha según fase impar): esa secuencia
 *     era predecible en cuanto le pillabas el patrón. Ahora cada tramo
 *     se sortea de forma independiente (tipo Y signo al azar), así que
 *     puede haber dos curvas seguidas al mismo lado, una recta cortita,
 *     etc. Se eliminó curve_phase (ya no hace falta).
 *   - Los tramos duran bastante menos ticks (STRAIGHT/CURVE/CHICANE
 *     *_MIN/MAX, todos reducidos a ~1/3 de lo que eran) para que el
 *     volante haga falta de forma prácticamente continua.
 *
 * ---------------------------------------------------------------------
 * 6ª pasada: sonido de motor + derrape
 * ---------------------------------------------------------------------
 * Se añadieron a sound.h/sound.c (no existían): sound_engine_set_speed()
 * (canal 2, tono continuo que sube con road_speed -- comparte canal con
 * la sirena y la música de menú, gana quien se active el último) y
 * sound_skid_start()/stop() (canal 1, ruido continuo cuando
 * road_speed>60% de SPEED_MAX Y |steer_angle|>60% de STEER_LIMIT a la
 * vez). Se llaman una vez por tick desde ND_PLAYING, y se paran
 * (sound_engine_stop()/sound_skid_stop()) en el resto de estados y al
 * salir del juego. Si el derrape salta demasiado a menudo o casi nunca,
 * los umbrales "3/5" de ambas condiciones son lo primero a tocar.
 *
 * ---------------------------------------------------------------------
 * 7ª pasada: reset del volante al empezar/tras chocar + banner de meta
 * ---------------------------------------------------------------------
 *   - controls_get_raw_delta() acumula movimiento desde la última
 *     lectura; en título/choque/game over/marcador nadie lo leía, así
 *     que lo girado en esos momentos se aplicaba de golpe al volver a
 *     ND_PLAYING (aunque steer_angle ya se pusiera a 0 a mano). Ahora
 *     nd_tick() lo vacía cada tick mientras state != ND_PLAYING.
 *   - Banner de "META" a cuadros: finish_post es un Post más (mismo
 *     y_fp/cx_fp que posts[], actualizado igual en
 *     update_curve_and_posts(), así que seguirá la curva de la
 *     carretera) pero se dibuja como una franja de lado a lado en vez
 *     de dos marcas en los bordes, y no "envuelve" al llegar abajo --
 *     desaparece sola. Se dispara al subir de stage (mismo momento que
 *     stage_bonus_flash) y va acompañado de un rótulo "META" grande en
 *     pantalla mientras dura el flash (~3s).
 *
 * ---------------------------------------------------------------------
 * 8ª pasada: pantalla de título rediseñada
 * ---------------------------------------------------------------------
 *   - ND_TITLE ya no es solo texto: draw_title_road() dibuja una
 *     carreterita en perspectiva (horizonte + bordes convergentes +
 *     marcas de carril que se agrandan) bajo el título, con el mismo
 *     espíritu visual que draw_road() pero totalmente estática -- no
 *     toca posts[]/horizon_cx_fp, así que no hace falta haber llamado
 *     a game_init()/init_posts() para pintarla.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "pico/stdlib.h"

#include "night_driver.h"
#include "renderer.h"
#include "controls.h"
#include "highscores.h"
#include "sound.h"

// ---------------------------------------------------------------------------
// Pantalla / punto fijo
// ---------------------------------------------------------------------------
#define SCREEN_W 320
#define SCREEN_H 240

#define PLAY_X 4
#define PLAY_Y 3
#define PLAY_W (SCREEN_W - 2*PLAY_X)
#define PLAY_H (SCREEN_H - 2*PLAY_Y)
#define CX (PLAY_X + PLAY_W/2)
#define CY (PLAY_Y + PLAY_H/2)

#define TICKS_S 60   // referencia nominal de ticks/segundo (no rítmica, ver scramble.c)

#define FP 256
#define PX2FP(px)  ((int32_t)(px) << 8)
#define FP2PX(fp)  ((int)((fp) >> 8))

// PXQ(px): convierte un valor en píxeles del DISEÑO ORIGINAL (768x576) a
// FP Q8 ya reescalado a 320x240. Multiplica por 5 ANTES de dividir entre
// 12 (todo en dominio FP, que ya trae ×256) para no perder precisión con
// enteros pequeños -- p.ej. PXQ(2) da 213 (0.83px), no 0.
#define PXQ(px)     (PX2FP(px) * 5 / 12)
// SCALEPX(px): igual pero para una magnitud entera en píxeles (no FP),
// p.ej. anchos de carretera/salpicadero.
#define SCALEPX(px) ((px) * 5 / 12)

static int rnd(int n)              { return n > 0 ? rand() % n : 0; }
static int clampi(int v,int lo,int hi){ return v < lo ? lo : (v > hi ? hi : v); }

static const int16_t SIN32[32] = {
      0,  50,  98, 142, 181, 213, 237, 251,
    256, 251, 237, 213, 181, 142,  98,  50,
      0, -50, -98,-142,-181,-213,-237,-251,
   -256,-251,-237,-213,-181,-142, -98, -50
};
#define SINV(a) SIN32[(a)&31]
#define COSV(a) SIN32[((a)+8)&31]

// ---------------------------------------------------------------------------
// Carretera / salpicadero (ver derivación del escalado en la cabecera)
// ---------------------------------------------------------------------------
#define HORIZ_Y     (PLAY_Y + SCALEPX(130))    // 57  -- horizonte
// DASH_H fija cuánto salpicadero reservamos abajo; BASE_Y y ROAD_RANGE
// se derivan de eso (antes era al revés: BASE_Y venía de escalar el
// original y el salpicadero se quedaba con lo que sobraba -- daba una
// franja de carretera más corta de lo que gustaba).
#define DASH_H      26
#define BASE_Y      ((PLAY_Y + PLAY_H) - DASH_H)   // 211 -- borde del salpicadero
#define ROAD_RANGE  (BASE_Y - HORIZ_Y)               // 154 -- carretera visible

#define ROAD_HW_FAR   SCALEPX(10)    // 4  -- semiancho de carretera en el horizonte
#define ROAD_HW_NEAR  80             // semiancho de carretera abajo del todo -- ver
                                      // nota de ajuste de dificultad en la cabecera
                                      // (antes SCALEPX(300)=125, daba un margen enorme
                                      // antes de poder salirte de la carretera)

#define NUM_POSTS 10   // más postes = sensación de movimiento más fluida/rápida

#define DASH_W  SCALEPX(96)                          // 40
#define DASH_X  (CX - DASH_W/2)
#define DASH_Y  BASE_Y

// Ancho del coche a efectos de colisión -- independiente de DASH_W, se ajusta
// directamente para calibrar el margen de error (ver nota en la cabecera).
#define CAR_HALF_WIDTH 9
#define CAR_HIT_L (CX - CAR_HALF_WIDTH)
#define CAR_HIT_R (CX + CAR_HALF_WIDTH)

// ---------------------------------------------------------------------------
// Velocidad
// ---------------------------------------------------------------------------
// Los divisores están expresados en ticks (tiempo), no en píxeles, para
// que sean independientes de SPEED_MAX: si subes SPEED_MAX, el tiempo
// hasta alcanzarla se mantiene igual (solo cambia lo rápido que se ve
// pasar la carretera, no el "tacto" de acelerar/frenar).
#define SPEED_MAX    PX2FP(8)        // antes PX2FP(5) -- más sensación de velocidad
#define TICKS_TO_MAX       90        // ~1.5s hasta velocidad máxima
#define TICKS_TO_STOP_BRAKE 67       // ~1.1s de máxima a parado frenando
#define TICKS_TO_STOP_DRAG 192       // ~3.2s de máxima a parado sin frenar
#define SPEED_ACCEL  (SPEED_MAX / TICKS_TO_MAX)
#define SPEED_BRAKE  (SPEED_MAX / TICKS_TO_STOP_BRAKE)
#define SPEED_DRAG   (SPEED_MAX / TICKS_TO_STOP_DRAG)

// ---------------------------------------------------------------------------
// Volante -- ver nota (1) más arriba sobre controls_get_raw_delta(0)
// ---------------------------------------------------------------------------
// Ganancia por transición cruda de cuadratura (4 transiciones = 1 clic
// físico del encoder). Subida ~2.75x respecto al primer pase junto con
// STEER_LIMIT (ver más abajo) -- antes, en curvas cerradas de stages
// altos, el volante se quedaba corto: el "contravalor" máximo que podía
// aportar el volante (STEER_LIMIT/9) no llegaba a compensar la curva
// más cerrada posible, así que el coche no giraba lo suficiente por
// mucho que lo intentaras.
#define STEER_GAIN   880
// Tope de giro. PXQ(220) en vez de PXQ(80): con las curvas más cerradas
// del ajuste de dificultad, STEER_LIMIT/9 (el "contravalor" máximo del
// volante, ver update_curve_and_posts) ahora supera con margen la curva
// más cerrada posible en el stage más difícil.
#define STEER_LIMIT  PXQ(220)

// ---------------------------------------------------------------------------
// Duraciones de tramo de carretera (en ticks -- no se reescalan, ver cabecera)
// ---------------------------------------------------------------------------
#define STRAIGHT_MIN    22
#define STRAIGHT_MAX    45
#define CURVE_SOFT_MIN  22
#define CURVE_SOFT_MAX  40
#define CURVE_HARD_MIN  16
#define CURVE_HARD_MAX  32
#define CHICANE_MIN     14
#define CHICANE_MAX     26

// ---------------------------------------------------------------------------
// Etapas / cuenta atrás
// ---------------------------------------------------------------------------
#define COUNTDOWN_START (60 * TICKS_S)
#define COUNTDOWN_BONUS (60 * TICKS_S)
#define KM_BASE  1875000UL   // ver derivación en la cabecera (orig. 4 500 000 * 5/12)

// ---------------------------------------------------------------------------
// Partículas de explosión
// ---------------------------------------------------------------------------
#define MAX_PARTS 24
#define PART_LIFE 40

typedef struct {
    int32_t x, y, vx, vy;
    int life, life_max;
    bool active;
} Particle;

static Particle parts[MAX_PARTS];

static void spawn_explosion(int sx, int sy, int n) {
    int spawned = 0;
    for (int i = 0; i < MAX_PARTS && spawned < n; i++) {
        if (parts[i].active) continue;
        parts[i].active  = true;
        parts[i].x = PX2FP(sx);
        parts[i].y = PX2FP(sy);
        parts[i].life = PART_LIFE + rnd(PART_LIFE/2);
        parts[i].life_max = parts[i].life;
        int a = rnd(32);
        int spd = PXQ(1)/2 + rnd(PXQ(3));
        parts[i].vx = spd * COSV(a) / FP;
        parts[i].vy = spd * SINV(a) / FP;
        spawned++;
    }
}

static void update_particles(void) {
    for (int i = 0; i < MAX_PARTS; i++) {
        if (!parts[i].active) continue;
        parts[i].x += parts[i].vx;
        parts[i].y += parts[i].vy;
        parts[i].vx = parts[i].vx * 245 / 256;
        parts[i].vy = parts[i].vy * 245 / 256;
        if (--parts[i].life <= 0) parts[i].active = false;
    }
}

static void draw_particles(void) {
    for (int i = 0; i < MAX_PARTS; i++) {
        if (!parts[i].active) continue;
        int px = FP2PX(parts[i].x);
        int py = FP2PX(parts[i].y);
        int age_pct = parts[i].life * 100 / parts[i].life_max;
        if (age_pct < 30 && (parts[i].life & 1)) continue; // parpadeo al apagarse
        if (px >= PLAY_X && px < PLAY_X+PLAY_W && py >= PLAY_Y && py < PLAY_Y+PLAY_H)
            renderer_fill_rect(px, py, 2, 2, COLOR_YELLOW);
    }
}

// ---------------------------------------------------------------------------
// Estado del juego
// ---------------------------------------------------------------------------
typedef struct {
    int32_t y_fp;   // profundidad (Q8) -- HORIZ_Y..BASE_Y
    int32_t cx_fp;  // posición horizontal del poste en ese frame (Q8)
} Post;

typedef enum {
    ND_TITLE,
    ND_PLAYING,
    ND_CRASH,
    ND_GAME_OVER,
    ND_SCORES
} nd_state_t;

static nd_state_t state;
static bool demo;
static bool g_done;
static int  blink;
static int  demo_ticks;
static int  pause_ticks;

static uint32_t player_score;
static uint32_t play_ticks;
static int32_t  road_speed;
static int32_t  top_speed;

static Post posts[NUM_POSTS];
static int32_t road_curve_vel;
static int32_t horizon_cx_fp;
static int32_t steer_angle;
static int32_t curve_target;
static int     curve_ticks_left;

// Banner de "META" a cuadros: se comporta como un poste más (misma
// profundidad/curva que posts[]), pero dibuja una franja a cuadros de
// lado a lado de la carretera en vez de dos postes en los bordes.
// Aparece al pasar de stage y desaparece sola al cruzar el salpicadero.
static Post finish_post;
static bool finish_line_active;

static int      stage;
static int32_t  countdown_ticks;
static uint32_t km_acc_fp;
static uint32_t km_stage_target;
static bool     stage_bonus_flash;
static int      stage_bonus_ticks;
static int      crash_flash_ticks;   // frames de flash rojo justo tras el choque

// ---------------------------------------------------------------------------
// Utilidades de dibujo
// ---------------------------------------------------------------------------
static void line(int x0, int y0, int x1, int y1, uint16_t color) {
    int dx = x1 - x0; if (dx < 0) dx = -dx;
    int dy = y1 - y0; if (dy < 0) dy = -dy;
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        if (x0 >= PLAY_X && x0 < PLAY_X+PLAY_W && y0 >= PLAY_Y && y0 < PLAY_Y+PLAY_H)
            renderer_fill_rect(x0, y0, 1, 1, color);
        if (x0 == x1 && y0 == y1) break;
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
// Carretera: postes + generador de curvas
// ---------------------------------------------------------------------------
static void init_posts(void) {
    int32_t spacing_fp = PX2FP(ROAD_RANGE) / NUM_POSTS;
    horizon_cx_fp    = PX2FP(CX);
    road_curve_vel   = 0;
    steer_angle      = 0;
    curve_target     = 0;
    // Recta de cortesía al empezar la partida (igual que tras un choque,
    // ver "curve_ticks_left = 120" en nd_tick): antes esto era "= 1", así
    // que en el primerísimo tick ya se generaba un tramo de CURVA real --
    // el juego arrancaba literalmente dentro de una curva sin dar tiempo
    // a nada, y se sentía como si "derivase" solo.
    curve_ticks_left = TICKS_S * 2;
    for (int i = 0; i < NUM_POSTS; i++) {
        posts[i].y_fp  = PX2FP(HORIZ_Y) + (int32_t)i * spacing_fp;
        posts[i].cx_fp = PX2FP(CX);
    }
}

// Elige el siguiente tramo de carretera totalmente al azar (tipo Y
// dirección), en vez de alternar en una secuencia fija recta/izda/
// recta/dcha como antes -- con la secuencia fija, en cuanto aprendías el
// patrón podías anticipar hacia dónde venía la siguiente curva. Ahora
// cada tramo es independiente: puede haber dos curvas seguidas hacia el
// mismo lado, una recta corta, una chicane... Los tramos también duran
// menos ticks que antes, así que hace falta corregir con el volante de
// forma mucho más continua.
static void next_curve_segment(void) {
    int s = clampi(stage - 1, 0, 7);
    int sign = rnd(2) ? 1 : -1;

    int straight_prob = clampi(16 - s*2, 4, 16);   // cada vez menos recta según sube el stage
    int chicane_prob   = 20;
    int hard_prob       = clampi(28 + s*4, 28, 56);
    // el resto (100 - straight - chicane - hard) es curva suave

    int r = rnd(100);

    if (r < straight_prob) {
        curve_target     = 0;
        curve_ticks_left = STRAIGHT_MIN + rnd(STRAIGHT_MAX - STRAIGHT_MIN);
    } else if (r < straight_prob + chicane_prob) {
        curve_target     = PXQ(3 + rnd(3)) * sign;
        curve_ticks_left = CHICANE_MIN + rnd(CHICANE_MAX - CHICANE_MIN);
    } else if (r < straight_prob + chicane_prob + hard_prob) {
        curve_target     = PXQ(7 + rnd(5 + s)) * sign;
        curve_ticks_left = CURVE_HARD_MIN + rnd(CURVE_HARD_MAX - CURVE_HARD_MIN);
    } else {
        curve_target     = PXQ(3 + rnd(4)) * sign;
        curve_ticks_left = CURVE_SOFT_MIN + rnd(CURVE_SOFT_MAX - CURVE_SOFT_MIN);
    }
}

static void update_curve_and_posts(void) {
    if (--curve_ticks_left <= 0) next_curve_segment();

    if (road_speed == 0) {
        road_curve_vel = 0;
    } else {
        // Cuanto más rápido vas, más "cierra" la curva percibida (igual que el original).
        int spd_px  = FP2PX(road_speed) + 1;
        int spd_cap = FP2PX(SPEED_MAX) + 1;
        if (spd_px > spd_cap) spd_px = spd_cap;
        int32_t scaled_target = curve_target * spd_px / spd_cap;

        road_curve_vel += (scaled_target - road_curve_vel) / 5;  // antes /8 -- la curva
                                                                   // "entra" más rápido,
                                                                   // hay que reaccionar antes

        horizon_cx_fp += road_curve_vel;
        int32_t hlim = PX2FP(PLAY_W/2 - ROAD_HW_FAR*2);
        if (horizon_cx_fp > PX2FP(CX)+hlim) horizon_cx_fp = PX2FP(CX)+hlim;
        if (horizon_cx_fp < PX2FP(CX)-hlim) horizon_cx_fp = PX2FP(CX)-hlim;
    }

    // steer_vel solo se aplica si el coche se mueve -- si está parado, el
    // volante puede girar (cosmético, ver draw_dashboard) pero la
    // carretera no debe desplazarse: antes "derivaba" aunque road_speed
    // fuera 0, porque steer_vel se restaba a los postes igualmente.
    int32_t steer_vel = (road_speed > 0) ? (steer_angle / 9) : 0;

    for (int i = 0; i < NUM_POSTS; i++) {
        posts[i].y_fp += road_speed;

        int t = (FP2PX(posts[i].y_fp) - HORIZ_Y) * 256 / ROAD_RANGE;
        if (t < 0) t = 0;
        if (t > 256) t = 256;

        posts[i].cx_fp += (road_curve_vel - steer_vel) * t / 256;

        if (posts[i].y_fp >= PX2FP(BASE_Y)) {
            posts[i].y_fp  -= PX2FP(ROAD_RANGE);
            posts[i].cx_fp  = horizon_cx_fp;
        }
    }

    // El banner de meta se mueve igual que un poste, pero sin
    // "envolver" al llegar abajo -- simplemente desaparece.
    if (finish_line_active) {
        finish_post.y_fp += road_speed;

        int t = (FP2PX(finish_post.y_fp) - HORIZ_Y) * 256 / ROAD_RANGE;
        if (t < 0) t = 0;
        if (t > 256) t = 256;

        finish_post.cx_fp += (road_curve_vel - steer_vel) * t / 256;

        if (finish_post.y_fp >= PX2FP(BASE_Y)) {
            finish_line_active = false;
        }
    }
}

static bool check_offroad(void) {
    int closest_y = HORIZ_Y;
    int32_t closest_cx = PX2FP(CX);

    for (int i = 0; i < NUM_POSTS; i++) {
        int y = FP2PX(posts[i].y_fp);
        if (y < HORIZ_Y || y >= BASE_Y) continue;
        if (y > closest_y) { closest_y = y; closest_cx = posts[i].cx_fp; }
    }
    if (closest_y == HORIZ_Y) return false;

    int t  = (closest_y - HORIZ_Y) * 256 / ROAD_RANGE;
    if (t > 256) t = 256;
    int t2 = t*t/256;
    int hw = ROAD_HW_FAR + (ROAD_HW_NEAR - ROAD_HW_FAR) * t2 / 256;
    int cx = FP2PX(closest_cx);

    int road_l = cx - hw, road_r = cx + hw;
    if (road_l >= CAR_HIT_L) return true;   // el coche se sale por la izquierda
    if (road_r <= CAR_HIT_R) return true;   // el coche se sale por la derecha
    return false;
}

static void draw_road(void) {
    // Repinta toda la franja de carretera cada frame (evita estelas de
    // postes que ya se movieron -- el framebuffer del renderer no se
    // borra solo).
    renderer_fill_rect(PLAY_X, HORIZ_Y, PLAY_W, ROAD_RANGE, COLOR_BLACK);
    renderer_fill_rect(PLAY_X, HORIZ_Y, PLAY_W, 1, COLOR_WHITE);

    for (int i = 0; i < NUM_POSTS; i++) {
        int y = FP2PX(posts[i].y_fp);
        if (y < HORIZ_Y || y >= BASE_Y) continue;

        int t  = (y - HORIZ_Y) * 256 / ROAD_RANGE;
        if (t < 0) t = 0;
        if (t > 256) t = 256;
        int t2 = t*t/256;

        int sz = 1 + t2*6/256;             // ~1px lejos .. ~7px cerca
        if (sz < 2) sz = 2;

        int cx = FP2PX(posts[i].cx_fp);
        int hw = ROAD_HW_FAR + (ROAD_HW_NEAR - ROAD_HW_FAR) * t2 / 256;

        int plx = cx - hw - sz - 1;
        int prx = cx + hw + 1;

        // Recorta el cuadrado del poste para que no invada ni la línea del
        // horizonte por arriba ni la franja del salpicadero por abajo --
        // sin esto, los postes más cercanos (los más grandes) asomaban un
        // poco dentro de la zona del salpicadero y draw_dashboard() los
        // tapaba con su fondo negro al dibujarse después.
        int ry = y - sz/2;
        int rh = sz;
        if (ry < HORIZ_Y+1)      { rh -= (HORIZ_Y+1 - ry); ry = HORIZ_Y+1; }
        if (ry + rh > BASE_Y)    { rh = BASE_Y - ry; }
        if (rh <= 0) continue;

        if (plx >= PLAY_X && plx+sz < PLAY_X+PLAY_W)
            renderer_fill_rect(plx, ry, sz, rh, COLOR_WHITE);
        if (prx >= PLAY_X && prx+sz < PLAY_X+PLAY_W)
            renderer_fill_rect(prx, ry, sz, rh, COLOR_WHITE);
    }

    // Banner de "META" a cuadros, de lado a lado de la carretera --
    // mismo cálculo de perspectiva (t, t2, hw) que los postes de arriba,
    // solo que en vez de dos marcas en los bordes es una franja entera.
    if (finish_line_active) {
        int y = FP2PX(finish_post.y_fp);
        if (y >= HORIZ_Y && y < BASE_Y) {
            int t = (y - HORIZ_Y) * 256 / ROAD_RANGE;
            if (t < 0) t = 0;
            if (t > 256) t = 256;
            int t2 = t*t/256;

            int barh = 2 + t2*5/256;           // ~2px lejos .. ~7px cerca
            int cx   = FP2PX(finish_post.cx_fp);
            int hw   = ROAD_HW_FAR + (ROAD_HW_NEAR - ROAD_HW_FAR) * t2 / 256;

            int ry = y - barh/2;
            int rh = barh;
            if (ry < HORIZ_Y+1)   { rh -= (HORIZ_Y+1 - ry); ry = HORIZ_Y+1; }
            if (ry + rh > BASE_Y) { rh = BASE_Y - ry; }

            if (rh > 0) {
                // Blanco/amarillo alternando (negro sería invisible sobre
                // el fondo de la carretera) -- ancho de cuadro creciente
                // con la perspectiva, igual que el resto de la carretera.
                int sq = 3 + t2*8/256;
                if (sq < 3) sq = 3;
                int l = cx - hw, r = cx + hw;
                if (l < PLAY_X) l = PLAY_X;
                if (r > PLAY_X+PLAY_W) r = PLAY_X+PLAY_W;
                int idx = 0;
                for (int x = l; x < r; x += sq, idx++) {
                    int w = sq;
                    if (x + w > r) w = r - x;
                    renderer_fill_rect(x, ry, w, rh, (idx & 1) ? COLOR_YELLOW : COLOR_WHITE);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Salpicadero + volante (vista interior del coche)
// ---------------------------------------------------------------------------
static void draw_dashboard(void) {
    int dy = DASH_Y, dw = DASH_W;

    renderer_fill_rect(PLAY_X, dy, PLAY_W, DASH_H, COLOR_BLACK);

    // Todo el bloque (volante, espejos, centro del capó) se traslada
    // hacia el lado por el que se gira ("lean"), y ADEMÁS el morro del
    // capó se sesga entre su borde lejano y su borde cercano ("skew"),
    // para que dé la sensación de girar/apuntar en perspectiva y no solo
    // de desplazarse en bloque.
    int lean = clampi((int)((int64_t)steer_angle * 16 / STEER_LIMIT), -16, 16);
    int skew = clampi((int)((int64_t)steer_angle * 11 / STEER_LIMIT), -11, 11);
    int dx = DASH_X + lean;
    int bot = dy + DASH_H;

    int top_x = dx + skew;   // borde lejano del capó (hacia el horizonte)
    int bot_x = dx - skew;   // borde cercano (pegado al salpicadero) -- pivota al contrario

    int bot_l = bot_x - dw/3, bot_r = bot_x + dw + dw/3;

    // Capó en perspectiva trapezoidal
    renderer_fill_rect(top_x, dy, dw, 2, COLOR_WHITE);
    line(top_x,      dy, bot_l, bot, COLOR_WHITE);
    line(top_x + dw, dy, bot_r, bot, COLOR_WHITE);
    renderer_fill_rect(bot_l, bot-2, bot_r-bot_l, 2, COLOR_WHITE);
    line(top_x + dw/2, dy+2, bot_x + dw/2, dy + dw/4, COLOR_WHITE);

    // Volante -- aro achatado (elipse) + radios, en el lado izquierdo del capó
    int wx = dx + dw/4;
    int wy = dy + DASH_H/3;
    int wr = clampi(dw/4, 3, 8);

    for (int a = 0; a < 8; a++) {
        int a0 = a*4, a1 = (a+1)*4;
        int x0 = wx + wr*COSV(a0)/FP, y0 = wy + (wr*SINV(a0)/FP)*2/3;
        int x1 = wx + wr*COSV(a1)/FP, y1 = wy + (wr*SINV(a1)/FP)*2/3;
        line(x0, y0, x1, y1, COLOR_WHITE);
    }

    int rot_step = (int)(steer_angle * 6 / STEER_LIMIT);
    rot_step = clampi(rot_step, -6, 6);

    int a_top = (0  - rot_step + 32) & 31, a_bot = (16 - rot_step + 32) & 31;
    int a_rgt = (8  - rot_step + 32) & 31, a_lft = (24 - rot_step + 32) & 31;

    int xt = wx + wr*COSV(a_top)/FP, yt = wy + (wr*SINV(a_top)/FP)*2/3;
    int xb = wx + wr*COSV(a_bot)/FP, yb = wy + (wr*SINV(a_bot)/FP)*2/3;
    int xr = wx + wr*COSV(a_rgt)/FP, yr = wy + (wr*SINV(a_rgt)/FP)*2/3;
    int xl = wx + wr*COSV(a_lft)/FP, yl = wy + (wr*SINV(a_lft)/FP)*2/3;

    line(xt, yt, wx, wy, COLOR_WHITE);
    line(xb, yb, wx, wy, COLOR_WHITE);
    line(xr, yr, wx, wy, COLOR_WHITE);
    line(xl, yl, wx, wy, COLOR_WHITE);
    renderer_fill_rect(wx-1, wy-1, 2, 2, COLOR_WHITE);

    // Retrovisores
    line(dx-1, dy+2, dx-6, dy+4, COLOR_WHITE);
    renderer_fill_rect(dx-10, dy+3, 5, 3, COLOR_WHITE);

    line(dx+dw+1, dy+2, dx+dw+6, dy+4, COLOR_WHITE);
    renderer_fill_rect(dx+dw+5, dy+3, 5, 3, COLOR_WHITE);
}

// ---------------------------------------------------------------------------
// HUD -- ver nota (4) en la cabecera sobre el cambio KM -> STAGE/%
// ---------------------------------------------------------------------------
static void draw_hud(void) {
    char buf[24];

    renderer_fill_rect(PLAY_X, PLAY_Y, PLAY_W, HORIZ_Y - PLAY_Y - 1, COLOR_BLACK);

    int spd_kmh = clampi((int)(road_speed * 250 / SPEED_MAX), 0, 250);
    int top_kmh = clampi((int)(top_speed  * 250 / SPEED_MAX), 0, 250);

    uint32_t km_pct = km_stage_target ? (km_acc_fp * 100) / km_stage_target : 0;
    if (km_pct > 100) km_pct = 100;

    int col_w = PLAY_W / 4;
    int x0 = PLAY_X + 2, x1 = x0+col_w, x2 = x0+col_w*2, x3 = x0+col_w*3;
    int y_lbl = PLAY_Y + 2, y_val = PLAY_Y + 10, y_sub = PLAY_Y + 26;

    renderer_draw_text(x0, y_lbl, "SCORE", COLOR_WHITE, COLOR_BLACK, 1);
    snprintf(buf, sizeof(buf), "%06lu", (unsigned long)player_score);
    renderer_draw_text(x0, y_val, buf, COLOR_WHITE, COLOR_BLACK, 2);

    renderer_draw_text(x1, y_lbl, "SPEED", COLOR_WHITE, COLOR_BLACK, 1);
    snprintf(buf, sizeof(buf), "%3d", spd_kmh);
    renderer_draw_text(x1, y_val, buf, COLOR_WHITE, COLOR_BLACK, 2);
    snprintf(buf, sizeof(buf), "TOP %d", top_kmh);
    renderer_draw_text(x1, y_sub, buf, COLOR_CYAN, COLOR_BLACK, 1);

    if (stage_bonus_flash && (stage_bonus_ticks/8) % 2 == 0) {
        renderer_draw_text(x2, y_lbl, "BONUS", COLOR_YELLOW, COLOR_BLACK, 1);
        renderer_draw_text(x2, y_val, "+60S", COLOR_YELLOW, COLOR_BLACK, 2);
    } else {
        renderer_draw_text(x2, y_lbl, "STAGE", COLOR_WHITE, COLOR_BLACK, 1);
        snprintf(buf, sizeof(buf), "%d", stage);
        renderer_draw_text(x2, y_val, buf, COLOR_WHITE, COLOR_BLACK, 2);
        snprintf(buf, sizeof(buf), "%lu%%", (unsigned long)km_pct);
        renderer_draw_text(x2, y_sub, buf, COLOR_CYAN, COLOR_BLACK, 1);
    }

    int32_t secs_left = countdown_ticks / TICKS_S;
    if (secs_left < 0) secs_left = 0;
    bool time_warn = (secs_left <= 10);
    if (!time_warn || (countdown_ticks/(TICKS_S/2)) % 2 == 0) {
        renderer_draw_text(x3, y_lbl, "TIME", COLOR_WHITE, COLOR_BLACK, 1);
        snprintf(buf, sizeof(buf), "%ds", (int)secs_left);
        renderer_draw_text(x3, y_val, buf, time_warn ? COLOR_RED : COLOR_WHITE, COLOR_BLACK, 2);
    }
}

// ---------------------------------------------------------------------------
// Ciclo de partida
// ---------------------------------------------------------------------------
static void game_init(void) {
    memset(parts, 0, sizeof(parts));
    road_speed        = 0;
    top_speed         = 0;
    play_ticks        = 0;
    player_score      = 0;
    countdown_ticks   = COUNTDOWN_START;
    km_acc_fp         = 0;
    stage             = 1;
    km_stage_target   = KM_BASE;
    stage_bonus_flash = false;
    stage_bonus_ticks = 0;
    finish_line_active = false;
    crash_flash_ticks = 0;
    init_posts();
}

static void demo_ai(void) {
    road_speed  = SPEED_MAX / 2;
    steer_angle = steer_angle * 252 / 256;
}

static void nd_tick(void) {
    blink++;

    // Vacía el delta pendiente del volante en cualquier estado que no
    // sea "conduciendo" (título, aturdido tras un choque, game over,
    // marcador). controls_get_raw_delta() acumula movimiento desde la
    // última vez que se leyó -- si no se lee mientras no controlas el
    // coche, todo lo que gires el volante en el título o durante el
    // choque se queda pendiente y se aplicaba de golpe en el primer
    // tick de vuelta a ND_PLAYING (el volante "saltaba" en vez de
    // arrancar recto, aunque steer_angle ya se pusiera a 0 a mano).
    // En modo demo no hace falta: el chequeo de "any" de abajo ya lee
    // (y por tanto consume) el delta él solo.
    if (!demo && state != ND_PLAYING) {
        (void)controls_get_raw_delta(0);
    }

    if (demo) {
        bool any = controls_menu_select()
                || controls_get_raw_delta(0) != 0
                || controls_button_down(BTN_J1_A)
                || controls_button_down(BTN_J1_B);
        if (any || ++demo_ticks >= TICKS_S*30) { g_done = true; return; }
    }

    switch (state) {

    case ND_TITLE:
        sound_engine_stop();
        sound_skid_stop();
        if (controls_button_pressed(BTN_ENC2_SW)) { g_done = true; break; }
        if (controls_menu_select()) {
            game_init();
            sound_stop_menu_music();
            state = ND_PLAYING;
        }
        break;

    case ND_PLAYING: {
        if (!demo && controls_button_pressed(BTN_ENC2_SW)) { g_done = true; break; }

        if (demo) {
            demo_ai();
        } else {
            bool accel = controls_button_down(BTN_J1_A);
            bool brake = controls_button_down(BTN_J1_B);
            if (accel) {
                road_speed += SPEED_ACCEL;
                if (road_speed > SPEED_MAX) road_speed = SPEED_MAX;
            } else if (brake) {
                road_speed -= SPEED_BRAKE;
                if (road_speed < 0) road_speed = 0;
            } else {
                road_speed -= SPEED_DRAG;
                if (road_speed < 0) road_speed = 0;
            }

            int enc = controls_get_raw_delta(0);
            if (enc) steer_angle += (int32_t)enc * STEER_GAIN;
            steer_angle = clampi(steer_angle, -STEER_LIMIT, STEER_LIMIT);
        }

        if (road_speed > top_speed) top_speed = road_speed;
        play_ticks++;
        km_acc_fp += (uint32_t)road_speed;

        // Motor: el tono sube con la velocidad actual.
        sound_engine_set_speed(
            (uint8_t)clampi((int)(road_speed * 255 / SPEED_MAX), 0, 255)
        );

        // Derrape: velocidad alta + volante girado a fondo (ambos por
        // encima de ~60% de su máximo). Ninguno de los dos umbrales es
        // sagrado -- si se activa demasiado (o demasiado poco), son los
        // primeros números a tocar.
        {
            int32_t steer_abs = steer_angle < 0 ? -steer_angle : steer_angle;
            bool skidding = (road_speed > SPEED_MAX * 3/5)
                         && (steer_abs  > STEER_LIMIT * 3/5);
            if (skidding) sound_skid_start();
            else          sound_skid_stop();
        }

        if (km_acc_fp >= km_stage_target) {
            km_acc_fp        = 0;
            countdown_ticks  += COUNTDOWN_BONUS;
            stage++;
            km_stage_target  = KM_BASE;
            for (int s = 1; s < stage; s++) km_stage_target = km_stage_target*5/4;
            stage_bonus_flash = true;
            stage_bonus_ticks = TICKS_S*3;
            finish_line_active = true;
            finish_post.y_fp   = PX2FP(HORIZ_Y);
            finish_post.cx_fp  = horizon_cx_fp;
            sound_effect_success();
        }
        if (stage_bonus_ticks > 0 && --stage_bonus_ticks == 0) stage_bonus_flash = false;

        {
            static int score_cd = 0;
            if (++score_cd >= TICKS_S/4) {
                score_cd = 0;
                player_score += (uint32_t)(1 + road_speed*10/SPEED_MAX);
            }
        }

        update_curve_and_posts();
        update_particles();

        if (!demo) {
            countdown_ticks--;
            if (countdown_ticks <= 0) {
                countdown_ticks = 0;
                spawn_explosion(CX, DASH_Y-6, 16);
                road_speed = 0;
                sound_engine_stop();
                sound_skid_stop();
                sound_effect_explosion();
                pause_ticks = TICKS_S*2;
                state = ND_GAME_OVER;
                break;
            }
        }

        if (road_speed > 0 && check_offroad()) {
            spawn_explosion(CX, DASH_Y-6, 28);
            road_speed       = 0;
            steer_angle      = 0;
            road_curve_vel   = 0;
            curve_target     = 0;
            curve_ticks_left = 120;
            horizon_cx_fp    = PX2FP(CX);
            for (int i = 0; i < NUM_POSTS; i++) posts[i].cx_fp = PX2FP(CX);
            countdown_ticks -= 8*TICKS_S;    // antes 5s -- salirse pesa más
            if (countdown_ticks < 0) countdown_ticks = 0;
            sound_engine_set_speed(0);
            sound_skid_stop();
            sound_effect_explosion();
            pause_ticks       = TICKS_S*3/2;
            crash_flash_ticks = 10;
            state = ND_CRASH;
            break;
        }
        break;
    }

    case ND_CRASH:
        update_particles();
        if (crash_flash_ticks > 0) crash_flash_ticks--;
        if (--pause_ticks <= 0) {
            road_speed  = 0;
            steer_angle = 0;
            state = ND_PLAYING;
        }
        break;

    case ND_GAME_OVER:
        sound_engine_stop();
        sound_skid_stop();
        update_particles();
        if (--pause_ticks <= 0 && controls_menu_select()) {
            if (!demo && highscores_is_top(ND_GAME_ID, player_score)) {
                highscores_enter(ND_GAME_ID, player_score);
            }
            pause_ticks = TICKS_S*5;
            state = ND_SCORES;
        }
        break;

    case ND_SCORES:
        if (--pause_ticks <= 0 || controls_menu_select()) g_done = true;
        break;
    }
}

// ---------------------------------------------------------------------------
// Gráfico decorativo de la pantalla de título: una carretera en
// perspectiva en miniatura, con el mismo espíritu visual que
// draw_road() (bordes que convergen hacia un horizonte + marcas de
// carril que se agrandan hacia el jugador) pero completamente
// estática -- no toca posts[]/horizon_cx_fp ni ningún estado vivo de
// la partida, así que es segura de llamar desde ND_TITLE sin haber
// inicializado nada de eso todavía.
// ---------------------------------------------------------------------------
static void draw_title_road(int top_y, int bottom_y) {
    const int hw_top = 6, hw_bot = 74;   // semiancho carretera arriba/abajo
    int lx_top = CX - hw_top, rx_top = CX + hw_top;
    int lx_bot = CX - hw_bot, rx_bot = CX + hw_bot;

    // Horizonte
    renderer_fill_rect(CX - hw_top - 30, top_y, (hw_top + 30) * 2, 1, COLOR_WHITE);

    // Bordes de la carretera, convergiendo hacia el horizonte
    line(lx_top, top_y, lx_bot, bottom_y, COLOR_WHITE);
    line(rx_top, top_y, rx_bot, bottom_y, COLOR_WHITE);

    // Marcas centrales del carril, agrandandose segun se acercan
    // (mismo patron alterno amarillo/blanco que usa draw_road() con
    // los postes reales)
    const int n = 4;
    for (int i = 0; i < n; i++) {
        int t = (i + 1) * 100 / (n + 1);          // 20,40,60,80% hacia abajo
        int y = top_y + (bottom_y - top_y) * t / 100;
        int w = 2 + (hw_bot - 2) * t / 100;        // se ensancha hacia abajo
        int h = 1 + 3 * t / 100;
        renderer_fill_rect(CX - w / 2, y, w, h, (i & 1) ? COLOR_YELLOW : COLOR_WHITE);
    }
}

static void nd_draw(void) {
    bool bon = (blink/15) % 2 == 0;

    switch (state) {
    case ND_TITLE:
        renderer_clear(COLOR_BLACK);
        renderer_draw_text(centered_x("NIGHT DRIVER",3), PLAY_Y + 6, "NIGHT DRIVER", COLOR_CYAN, COLOR_BLACK, 3);
        draw_title_road(PLAY_Y + 42, PLAY_Y + 108);
        renderer_draw_text(centered_x("PULSA PARA JUGAR",2), PLAY_Y + 120, "PULSA PARA JUGAR", COLOR_WHITE, COLOR_BLACK, 2);
        if (bon)
            renderer_draw_text(centered_x("LLEGA LO MAS LEJOS QUE PUEDAS",1), PLAY_Y + 146,
                                "LLEGA LO MAS LEJOS QUE PUEDAS", COLOR_YELLOW, COLOR_BLACK, 1);
        renderer_draw_text(centered_x("A:ACELERA  B:FRENA  ENC1:VOLANTE",1), PLAY_Y + 162,
                            "A:ACELERA  B:FRENA  ENC1:VOLANTE", COLOR_WHITE, COLOR_BLACK, 1);
        renderer_draw_text(centered_x("ENC2: SALIR",1), PLAY_Y + 176, "ENC2: SALIR", COLOR_WHITE, COLOR_BLACK, 1);
        renderer_flush();
        break;

    case ND_PLAYING:
    case ND_CRASH:
        draw_road();
        draw_particles();
        draw_dashboard();
        draw_hud();
        if (state == ND_CRASH && crash_flash_ticks > 0 && (crash_flash_ticks & 1)) {
            renderer_fill_rect(PLAY_X, PLAY_Y, PLAY_W, 3, COLOR_RED);
            renderer_fill_rect(PLAY_X, PLAY_Y+PLAY_H-3, PLAY_W, 3, COLOR_RED);
            renderer_fill_rect(PLAY_X, PLAY_Y, 3, PLAY_H, COLOR_RED);
            renderer_fill_rect(PLAY_X+PLAY_W-3, PLAY_Y, 3, PLAY_H, COLOR_RED);
        }
        if (state == ND_CRASH && bon)
            renderer_draw_text(centered_x("CRASH",2), HORIZ_Y + ROAD_RANGE/2 - 7,
                                "CRASH", COLOR_RED, COLOR_BLACK, 2);
        if (stage_bonus_flash)
            renderer_draw_text(centered_x("META",3), HORIZ_Y + ROAD_RANGE/3,
                                "META", COLOR_YELLOW, COLOR_BLACK, 3);
        if (demo && bon)
            renderer_draw_text(centered_x("DEMO",1), DASH_Y-10, "DEMO", COLOR_YELLOW, COLOR_BLACK, 1);
        renderer_flush();
        break;

    case ND_GAME_OVER:
        draw_road();
        draw_particles();
        draw_dashboard();
        draw_hud();
        renderer_draw_text(centered_x("GAME OVER",2), CY-10, "GAME OVER", COLOR_YELLOW, COLOR_BLACK, 2);
        if (bon)
            renderer_draw_text(centered_x("PULSA PARA CONTINUAR",1), CY+12,
                                "PULSA PARA CONTINUAR", COLOR_WHITE, COLOR_BLACK, 1);
        renderer_flush();
        break;

    case ND_SCORES:
        renderer_clear(COLOR_BLACK);
        highscores_draw(ND_GAME_ID, "NIGHT DRIVER", 20);
        if (bon)
            renderer_draw_text(centered_x("PULSA PARA CONTINUAR",1), SCREEN_H-14,
                                "PULSA PARA CONTINUAR", COLOR_WHITE, COLOR_BLACK, 1);
        renderer_flush();
        break;
    }
}

// ---------------------------------------------------------------------------
// Entrada pública
// ---------------------------------------------------------------------------
void game_night_driver_run(game_mode_t mode) {
    demo        = (mode == GAME_MODE_DEMO);
    blink       = 0;
    demo_ticks  = 0;
    g_done      = false;

    if (demo) {
        game_init();
        road_speed = SPEED_MAX / 2;
        state = ND_PLAYING;
    } else {
        memset(parts, 0, sizeof(parts));
        road_speed  = 0;
        steer_angle = 0;
        state = ND_TITLE;
        sound_start_menu_music();
    }

    while (!g_done) {
        controls_update();
        nd_tick();
        nd_draw();
        sound_update();
        sleep_ms(1);
    }

    // Por si se sale del juego (ENC2) en mitad de una partida, con el
    // motor o el derrape todavía sonando.
    sound_engine_stop();
    sound_skid_stop();

    highscores_flush();
}