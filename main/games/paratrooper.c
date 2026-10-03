/**
 * paratrooper.c -- torreta antiaérea, helicópteros que sueltan
 * paracaidistas, torre marchante que escala la base, avión bombardero,
 * oleadas con dificultad creciente. Portado de la version Pico a
 * ArcadeColor / ESP32, mismo criterio de adaptación que asteroids.c:
 *
 *  - Resolución, física de punto fijo Q8, trigonometría por tabla,
 *    primitivas de dibujo vectorial: nada de esto es específico del
 *    SDK de Pico, así que se porta literal -- ver el resto de esta
 *    cabecera, que se mantiene tal cual porque sigue siendo una
 *    descripción exacta del comportamiento del juego.
 *  - FÍSICA CON TIEMPO DELTA REAL (g_dt_scale, ver asteroids.c):
 *    idéntico mecanismo, solo cambia la fuente de tiempo real -- ver
 *    nota de PORT A ESP32 más abajo.
 *
 * ACTUALIZADO con la última versión de la Pico:
 *  - Jingle de inicio: Toccata y fuga en re menor (BWV 565), una sola
 *    pasada de ~8,6 s (sound_start_paratrooper_music()); se corta al
 *    morir la torreta y al salir del juego.
 *  - Bombas del avión: apuntan a la altura de la TORRETA (CANNON_OY) en
 *    vez de al suelo, vy0 = 24, el homing se detiene al pasar esa altura
 *    (antes daba un bandazo) y el impacto se juzga por altura + alineación
 *    horizontal con TURRET_X.
 *  - Soldados en verde, caja del mensaje inferior según la escala del
 *    texto y mensaje DEMO a escala 2.
 *  - Pantalla de título nueva con torreta y dos paracaidistas dibujados
 *    con las mismas primitivas del juego.
 *  - Demo: se vacía el acumulador del stick antes de arrancar, para que
 *    un rebote antiguo no la corte en el primer tick.
 * AJUSTES POSTERIORES (ESP32):
 *  - Paracaidistas: la suelta de los helicópteros, la entrada de helis por
 *    carril, el modo ataque y las pausas de fin de partida van en ms REALES
 *    (antes en frames, y a más de 60 fps corrían más deprisa de lo que se
 *    veía moverse todo). El ritmo de suelta sube un 9 % por ola superada
 *    (drop_rate_pct(), tope 180 %) y el tope de paracaidistas cayendo a la
 *    vez pasa de 4 (ola 1) a 7 (ola 7+). Ver DROP_*_MS y DROP_WAVE_*.
 *  - Bombas: los aviones las sueltan poco después de entrar en pantalla
 *    (PLANE_DROP_MIN/RANGE) para que crucen media pantalla en una parábola
 *    amplia, y vuelve una zona protegida (BOMB_SAFE_R px alrededor del
 *    cañón) en la que no se pueden destruir.
 *  - Suelo y soldados: el soldado aterriza cuando sus PIES tocan el suelo (o
 *    el último apilado), no cuando el enganche del paracaídas llega a la línea
 *    (se hundía 15 px y saltaba); y la línea del suelo se repone cada vez que
 *    una caja de borrado la cruza (repaint_ground) y cada GROUND_REFRESH_MS.
 *    El mensaje inferior va bajo la línea del suelo (escala 1 si no cabe).
 *  - Game Over -> récords: se vacía la entrada tras highscores_enter() y al
 *    cambiar de pantalla, y durante END_SCREEN_LOCK_MS se ignoran botones y
 *    click de salida, para que no salte al título sin verse la pantalla.
 *  - Bombas del avión: quitada la zona de seguridad de 30 px junto a la
 *    torreta (impedía romperlas en el último tramo de su caída), radio de
 *    impacto BOMB_HIT_R y colisión bala->bomba por barrido (seg_hit()).
 *
 * RENDIMIENTO (ESP32):
 *  - Un único renderer_flush() por frame en vez de uno por entidad, y los
 *    soldados parados en el suelo ya no se borran/redibujan cada frame (solo
 *    cuando algo les muerde el sprite). Con 12 soldados en el suelo se
 *    pasaba de ~27 flushes y ~540 fill_rect por frame a unos pocos.
 *  - Giro del cañón, marcha y escalada de los soldados por tiempo real
 *    (g_dt_scale) en vez de por frame: ya no dependen de los fps. La marcha
 *    pasa de 0,5 a 1 px por tick nominal (MARCH_SPD).
 *
 * AVIONES (ESP32):
 *  - La bomba se suelta al llegar a una posición de pantalla al azar
 *    (drop_x), no a los 30 frames: con frames cortos el avión aún estaba
 *    fuera de pantalla y la bomba se descartaba al nacer.
 *  - Pasada de 1 a 5 aviones al azar, con intervalo variable (250-2500 ms
 *    reales) entre lanzamientos; pueden ir seguidos por el mismo lado
 *    (PLANE_MIN_SEP px entre sí) y solo cambian de lado cuando no queda
 *    ninguno en pantalla. Cada uno suelta su bomba (MAX_BOMBS = 5).
 *  - Ranuras de aterrizaje: 5 por lado (quitadas las 2 más cercanas a la
 *    torreta).
 *
 * SE MANTIENE lo específico de la ESP32: DROP_PROB=2 y DROP_PROB_ATTACK=1
 * (más agresivos que los 5 y 3 de la Pico), controles con eje X del
 * joystick 1, click de stick (índices 4/5) para salir, bucle a 60 fps con
 * vTaskDelayUntil y esp_timer.
 *
 * PORT A ESP32 -- igual que asteroids.c/scramble.c/space_invaders.c/
 * lunar_lander.c: nada de Pico SDK (pico/stdlib.h, absolute_time_t,
 * sleep_ms, time_us_32...), sustituido por esp_timer.h + FreeRTOS. Lo
 * que cambia respecto a la version Pico:
 *
 *  - Resolución: 320x240 en vez de 768x576. TODA la geometría del
 *    juego (torreta, helicópteros, soldados, paracaídas, avión,
 *    bombas, ranuras de aterrizaje...) se ha reescalado a mano
 *    (~0.5-0.6x, no proporcionalmente exacta) y algunos trazos muy
 *    finos del original (el hueco triangular de la cabina del heli,
 *    la hélice de cola animada en 4 fases geométricas) se han
 *    simplificado a versiones más robustas a este tamaño de pantalla
 *    -- siguen siendo reconocibles pero no son replicas pixel-a-pixel.
 *    Esto no es específico de ESP32 -- ya era así en la version Pico
 *    que se ha portado, se mantiene tal cual.
 *  - Tiempo real: last_tick_time era absolute_time_t (get_absolute_time()/
 *    absolute_time_diff_us()) -- ahora es un uint32_t en ms vía
 *    esp_timer_get_time() (ver now_ms() más abajo, mismo patrón que
 *    el resto de juegos ya portados). update_dt_scale() no cambia su
 *    lógica ni sus límites (1..50 ms de recorte), solo la fuente del
 *    tiempo.
 *  - Bucle a ritmo fijo con vTaskDelayUntil() (igual que el resto de
 *    juegos) en vez de sleep_ms(8). Como la física ya se escala por
 *    g_dt_scale (tiempo real), este período objetivo solo evita que
 *    el bucle corra sin descanso -- no afecta al ritmo de juego.
 *  - Controles: con un único jugador y el joystick 1 (2 ejes + botones)
 *    en vez de un encoder + 2 botones, el mapeo queda:
 *      * controls_get_raw_delta_x(0) -> gira el cañón -- OJO, esto es
 *        un cambio real de eje, no solo de nombre. El "Enc1" original
 *        no distinguía X/Y; en controls.h de ESP32 el índice 0 de
 *        controls_get_raw_delta() (sin _x) es el eje Y, así que
 *        dejarlo tal cual habría hecho que el cañón girase inclinando
 *        el stick arriba/abajo. Se usa el eje X -- mismo criterio
 *        "girar = inclinar como un volante" que ya usan asteroids.c y
 *        lunar_lander.c para sus respectivas rotaciones. El eje Y del
 *        stick queda sin usar en este juego.
 *      * BTN_IDX_J1_A/BTN_IDX_J1_B (0/1, cualquiera de los dos)
 *        dispara, igual que el original.
 *      * BTN_IDX_J1_SW/BTN_IDX_J2_SW (4/5, click de cualquiera de los
 *        dos joysticks) salen al menú en cualquier momento -- mismos
 *        índices que ya usaba la version Pico para los switches de
 *        encoder, solo cambia qué hay físicamente detrás del índice
 *        (click del stick en vez de switch de encoder).
 *  - Sonido: sound.c no tiene el enum de efectos con datos de nota
 *    embebidos del original (SFX_FIRE, SFX_AST_*, SFX_BALL_*...);
 *    cada uno se mapea al efecto más parecido de nuestro motor
 *    (sound_effect_shoot/explosion/move/success/game_over...), y el
 *    "avión" reutiliza sound_siren_start()/stop() como aviso de
 *    aproximación, igual que el OVNI de asteroids.c.
 *  - Sin hs_input/PT_ENTER_NAME: highscores_enter() bloqueante, como
 *    en asteroids.c/pong.c/space_invaders.c.
 *  - API pública: void game_paratrooper_run(game_mode_t mode), la
 *    misma firma game_run_fn que el resto de juegos (game_common.h)
 *    -- Paratrooper no tiene concepto de 2 jugadores, GAME_MODE_2P se
 *    trata igual que 1P. Bucle propio dentro de esa función, nada de
 *    callbacks de dibujo/tick registrados aparte.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "paratrooper.h"
#include "../renderer.h"
#include "../controls.h"
#include "../highscores.h"
#include "../sound.h"

// Ritmo objetivo del bucle principal (vTaskDelayUntil en
// game_paratrooper_run) -- ver nota de cabecera: la física ya está
// escalada por g_dt_scale (tiempo real), así que esto solo evita que
// el bucle corra sin descanso.
#define TARGET_FPS 60

// Tiempo real en ms desde el arranque -- sustituye a
// absolute_time_t/get_absolute_time() del SDK de Pico (mismo patrón
// que el resto de juegos ya portados).
static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// ---------------------------------------------------------------------------
// Pantalla / área de juego -- literales fijos, ver el mismo comentario
// (más largo) en asteroids.c sobre por qué no usar TFT_WIDTH/HEIGHT aquí.
// ---------------------------------------------------------------------------
#define SCREEN_W 320
#define SCREEN_H 240

#define PLAY_X   4
#define PLAY_Y   3
#define PLAY_W   (SCREEN_W - 2 * PLAY_X)   // 312
#define PLAY_H   (SCREEN_H - 2 * PLAY_Y)   // 234
#define CX       (PLAY_X + PLAY_W / 2)
#define CY       (PLAY_Y + PLAY_H / 2)

#define TICKS_S  60   // referencia nominal SOLO para convertir a ms (ver g_dt_scale)

// Índices de controls_button_down()/controls_button_pressed(), tal
// cual los documenta controls.h de ESP32: 0=J1_A, 1=J1_B, 2=J2_A,
// 3=J2_B, 4=J1_SW (click del joystick 1), 5=J2_SW (click del
// joystick 2) -- los switches de encoder del original (ENC1_SW/
// ENC2_SW) pasan a ser estos mismos índices 4/5, ahora el click del
// propio stick en vez de un switch de encoder aparte.
#define BTN_IDX_J1_A   0
#define BTN_IDX_J1_B   1
#define BTN_IDX_J1_SW  4
#define BTN_IDX_J2_SW  5

// ---------------------------------------------------------------------------
// Punto fijo Q8
// ---------------------------------------------------------------------------
#define FP         256
#define PX2FP(x)   ((int32_t)(x) * FP)
#define FP2PX(x)   ((int)((x) / FP))

// ---------------------------------------------------------------------------
// Tiempo delta real -- idéntico mecanismo que asteroids.c, con la
// fuente de tiempo real adaptada a ESP32 (ver now_ms() más abajo).
// ---------------------------------------------------------------------------
static uint32_t last_tick_time_ms;
static int32_t g_dt_scale = FP;
static int32_t g_elapsed_ms = 16;   // ms reales del último tick (para temporizadores que no deben depender de los fps)

static void update_dt_scale(void) {
    uint32_t now = now_ms();
    int32_t elapsed_ms = (int32_t)(now - last_tick_time_ms);
    last_tick_time_ms = now;

    if (elapsed_ms < 1)  elapsed_ms = 1;
    if (elapsed_ms > 50) elapsed_ms = 50; // limita saltos tras una pausa larga (highscores_enter())

    g_dt_scale = elapsed_ms * FP / 16;
    g_elapsed_ms = elapsed_ms;
}

// ---------------------------------------------------------------------------
// Trigonometría -- idéntica al original (tabla de grados, no de pasos).
// ---------------------------------------------------------------------------
static const int16_t SIN90[91] = {
  0,4,9,13,18,22,27,31,36,40,44,49,53,57,62,66,70,74,79,83,
  87,91,95,99,103,107,111,115,119,122,126,130,133,137,140,144,
  147,150,154,157,160,163,166,169,172,175,177,180,182,185,187,
  190,192,194,196,198,200,202,204,205,207,208,210,211,212,213,
  214,215,216,217,218,218,219,219,220,220,220,220,220,220,220,
  221,221,221,222,222,222,222,223,223,256
};
static int16_t sin_deg(int d) {
    d = d % 360; if (d < 0) d += 360;
    if (d <= 90)  return  SIN90[d];
    if (d <= 180) return  SIN90[180-d];
    if (d <= 270) return -SIN90[d-180];
    return             -SIN90[360-d];
}
static int16_t cos_deg(int d) { return sin_deg(d + 90); }
static inline int16_t can_dx(int a) { return  sin_deg(a); }
static inline int16_t can_dy(int a) { return -cos_deg(a); }

// ---------------------------------------------------------------------------
// Soldado -- SOLDIER_H = altura pies-cabeza. BASE_H = 2*SOLDIER_H exactos
// para que la escalera de la torre encaje.
// ---------------------------------------------------------------------------
#define SOLDIER_H    15

// ---------------------------------------------------------------------------
// Torreta
// ---------------------------------------------------------------------------
#define TURRET_X     CX
#define TURRET_Y     (PLAY_Y + PLAY_H - 12)   // pie de la base = suelo
#define BASE_H       (2 * SOLDIER_H)          // 30
#define BASE_W       24
#define BODY_RX      7
#define BODY_RY      13
#define CANNON_LEN   16
#define CANNON_W     2
#define TURRET_R     10

#define GROUND_Y     (TURRET_Y - 1)

#define CANNON_OX    TURRET_X
#define CANNON_OY    (TURRET_Y - BASE_H - BODY_RY/2)

// ---------------------------------------------------------------------------
// Ranuras de aterrizaje -- 5 por lado, X fija al aterrizar.
// ---------------------------------------------------------------------------
// Antes había 7 ranuras por lado; las 2 más cercanas a la torreta se han
// quitado (los paracaidistas caían demasiado pegados a la base). Se conserva
// el reparto original de 7 (SLOT_TOTAL) para que las 5 que quedan estén
// exactamente donde estaban las antiguas 2..6.
#define SLOT_TOTAL   7
#define SLOT_SKIP    2
#define NUM_SLOTS    (SLOT_TOTAL - SLOT_SKIP)
#define SLOT_MARGIN  (BASE_W/2 + 6)
#define SLOT_EDGE    10
#define SLOT_SPACE_L (TURRET_X - PLAY_X - SLOT_MARGIN - SLOT_EDGE)
#define SLOT_SPACE_R (PLAY_X + PLAY_W - TURRET_X - SLOT_MARGIN - SLOT_EDGE)
#define SLOT_X_L(n)  (TURRET_X - SLOT_MARGIN - (((n)+SLOT_SKIP) * SLOT_SPACE_L) / (SLOT_TOTAL-1))
#define SLOT_X_R(n)  (TURRET_X + SLOT_MARGIN + (((n)+SLOT_SKIP) * SLOT_SPACE_R) / (SLOT_TOTAL-1))
#define SLOT_X(s,n)  ((s)==0 ? SLOT_X_L(n) : SLOT_X_R(n))
#define SLOT_STACK   3

// ---------------------------------------------------------------------------
// Helicópteros -- 2 franjas fijas. HELI_SPD en px/tick "nominal" (no FP),
// escalado por g_dt_scale en update_helis() -- ver cabecera del archivo.
// ---------------------------------------------------------------------------
#define LANE0_Y       (PLAY_Y + 15)
#define LANE1_Y       (PLAY_Y + 36)
#define HELI_SPD      1
#define HELI_GAP_MS   2000   // hueco mínimo entre helicópteros de un mismo carril (+ hasta 1000 ms al azar)
#define HELI_MIN_SEP  80
#define DROP_PROB     2   // antes 5 -- 1 de cada 2 comprobaciones sueltan, en vez de 1 de cada 5

// RITMO DE SUELTA, en ms REALES (antes en frames: a más de 60 fps corría
// más deprisa de lo que se veía moverse el helicóptero).
// Un helicóptero tarda ~7 s en cruzar la pantalla. En la ola 1 hace una
// primera comprobación a los 1,0-2,2 s y luego una cada 1,6-2,9 s (~3 por
// cruce), y cada comprobación suelta con probabilidad 1/DROP_PROB.
// El ritmo sube un DROP_WAVE_STEP_PCT % por cada ola superada (los tiempos
// se dividen entre drop_rate_pct()/100) hasta DROP_WAVE_MAX_PCT %.
#define DROP_FIRST_MIN_MS      1000
#define DROP_FIRST_RANGE_MS    1200
#define DROP_INTERVAL_MIN_MS   1600
#define DROP_INTERVAL_RANGE_MS 1300
#define DROP_WAVE_STEP_PCT     9
#define DROP_WAVE_MAX_PCT      180

// Tope de paracaidistas cayendo a la vez: 4 en la ola 1, +1 cada 2 olas, máx. 7.
#define PARAS_AIR_BASE   4
#define PARAS_AIR_MAX    7

#define ATTACK_DUR_MS         6000
#define ATTACK_INTERVAL_MS    20000
#define DROP_PROB_ATTACK   1   // antes 3 -- en modo ataque, suelta siempre que toca comprobar

// ---------------------------------------------------------------------------
// Cañón
// ---------------------------------------------------------------------------
#define ANGLE_MIN_DEG  (-75)
#define ANGLE_MAX_DEG  ( 75)
#define ANGLE_UP_DEG   (  0)
#define CANNON_ROT_SPD  3   // grados por tick NOMINAL (16 ms); se escala con g_dt_scale

// ---------------------------------------------------------------------------
// Torre / escalera -- march/climb se dejan en ticks discretos (sin dt),
// igual que el tic-tac de asteroids.c.
// ---------------------------------------------------------------------------
#define TOWER_COUNT   4
// Velocidad de marcha/escalada en FP por tick NOMINAL (16 ms), escalada con
// g_dt_scale como el resto de la física. Antes eran "1 px cada 2 ticks" y
// "1 px por tick" contados en frames: a 60 fps ya era lenta (30 px/s) y si el
// juego bajaba de fps se arrastraban. 256 = 1 px por tick nominal (~60 px/s).
#define MARCH_SPD     256
#define CLIMB_SPD     256

// ---------------------------------------------------------------------------
// Constantes de entidades -- reducidas respecto al original (pantalla
// más pequeña, menos carga de render incremental), mismo criterio que
// asteroids.c.
// ---------------------------------------------------------------------------
#define MAX_HELIS      8
#define MAX_PARAS      24
#define MAX_BULLETS    6
#define MAX_BOMBS      5   // una por avión (hasta MAX_PLANES a la vez)
#define MAX_PLANES     5
#define MAX_PARTICLES  32

#define PTS_HELI            150
#define PTS_PARA_AIR         25
#define PTS_PARA_CHUTE_HIT   15
#define PTS_PARA_GROUND      10
#define PTS_PLANE           500
#define PTS_BOMB_DODGE       50
#define PTS_WAVE_BONUS      500

#define BULLET_SPD   4   // px/tick nominal, ver player_fire()
#define BOMB_HIT_R   7   // radio de impacto bala->bomba (bala 3 + sprite ~4)
#define BOMB_SAFE_R  45  // radio alrededor del cañón dentro del cual la bomba NO se puede destruir
#define PLANE_SPD    2   // px/tick nominal

// Pasada de aviones al terminar la ola: entre 1 y MAX_PLANES aviones (al
// azar), lanzados con un intervalo variable (JET_GAP_*), en ms REALES.
// Pueden ir seguidos por el mismo lado como los helicópteros, guardando
// PLANE_MIN_SEP px entre sí para no solaparse, y solo cambian de lado de
// aparición cuando ya no queda ningún avión en pantalla.
#define PLANE_MIN_SEP      70
#define PLANE_DROP_MIN     25   // px dentro de la pantalla (desde el borde de entrada) a los que suelta la bomba...
#define PLANE_DROP_RANGE   45   // ...más hasta este margen al azar
#define JET_GAP_MIN_MS     250
#define JET_GAP_RANGE_MS   2250

#define CHUTE_OPEN_Y_MIN  (PLAY_Y + PLAY_H/5)
#define CHUTE_OPEN_Y_MAX  (PLAY_Y + PLAY_H*2/3)

// ---------------------------------------------------------------------------
// Tipos
// ---------------------------------------------------------------------------
typedef struct {
    int32_t x;          // FP
    int     y;           // Y fija de la franja
    int     vx;           // px/tick nominal, con signo (dirección)
    bool    active;
    int     drop_timer, anim_timer;
    int     rotor_frame, tail_frame;
} Heli;

typedef struct {
    int32_t x, y, vy;    // FP
    bool    active, chute_open, chute_shot, landed;
    int     side;         // 0=izq 1=der
    int     slot;         // ranura asignada, -1=en vuelo
    int     stack;        // nivel de apilamiento (0=suelo)
    int     chute_open_y;
    bool    marching, climbing;
    int     march_order;  // 0..3 en la fila de ataque, -1=sin asignar
    int32_t target_x, target_y;
    int     anim_timer;
} Para;

typedef struct { int32_t x, y, vx, vy; int32_t px, py; bool active; } Bullet;   // px,py = posición del tick anterior (colisión por barrido)
typedef struct { int32_t x, y, vx, vy, ay; bool active; } Bomb;
typedef struct {
    int32_t x, y; int vx; bool active;
    int bomb_dropped, anim_timer;
    int drop_x;          // px de pantalla donde suelta su bomba (al azar por avión)
} Plane;
typedef struct { int32_t x, y, vx, vy; int life, life_max; bool active; } Particle;

// ---------------------------------------------------------------------------
// Estados
// ---------------------------------------------------------------------------
typedef enum {
    PT_TITLE, PT_PLAYING, PT_JET_PASS, PT_DEAD, PT_GAMEOVER, PT_SCORES
} PtState;

static PtState state;
static int     blink;
static bool    demo_mode;
static int     pause_cnt, demo_ticks;   // ms
static int32_t input_lock_ms;           // ms durante los que se ignoran botones y click de salida (ver flush_inputs)
static int     score, wave;
static bool    g_done;

// Cañón
static int  cannon_angle_deg;
static int  cannon_rot_dir;
static int  cannon_inv;
static bool cannon_alive;
static int  cannon_enc_acc;
static int32_t cannon_rot_acc;   // resto fraccionario de grados, en FP
static bool fire_held;

// Suelo
static int  slot_count[2][NUM_SLOTS];
static int  ground_total[2];

// Torre marchante
static bool tower_active;
static int  tower_side;
static bool helis_frozen;

// Oleada
static int  wave_heli_budget;
static int  wave_helis_spawned;
static bool wave_done;
static int32_t jet_delay;     // ms hasta lanzar el primer avión tras despejar la ola
static bool jet_launched;
static int  jets_left;        // aviones que faltan por lanzar en esta pasada
static int  jet_next_dir;     // lado por el que entrará el siguiente (+1 izq->der, -1 der->izq)
static int32_t jet_gap_ms;    // ms hasta poder lanzar el siguiente

// Modo ataque
static bool attack_mode;
static int  attack_timer;
static int  attack_cooldown;

static int lane_timer[2];

// Entidades
static Heli     helis[MAX_HELIS];
static Para     paras[MAX_PARAS];
static Bullet   bullets[MAX_BULLETS];
static Bomb     bombs[MAX_BOMBS];
static Plane    planes[MAX_PLANES];
static Particle particles[MAX_PARTICLES];

// ---------------------------------------------------------------------------
// Utilidades
// ---------------------------------------------------------------------------
static int  rnd(int n)  { return n > 0 ? rand() % n : 0; }
static int  absi(int x) { return x < 0 ? -x : x; }
static bool chit(int ax, int ay, int ra, int bx, int by, int rb) {
    int dx = ax-bx, dy = ay-by, r = ra+rb;
    return (dx*dx + dy*dy) <= r*r;
}

// Colisión por BARRIDO: ¿pasa el segmento (x0,y0)->(x1,y1) -- lo que ha
// recorrido la bala en este tick -- a menos de r píxeles del punto
// (cx,cy)? Con el dt real escalado, una bala puede avanzar varios
// píxeles entre dos ticks y "saltarse" un objetivo pequeño sin que
// chit() (que solo mira la posición final) llegue a verlo.
static bool seg_hit(int x0, int y0, int x1, int y1, int cx, int cy, int r) {
    long vx = x1-x0, vy = y1-y0;
    long wx = cx-x0, wy = cy-y0;
    long len2 = vx*vx + vy*vy;
    long t256 = 0;                                   // punto más cercano, en 1/256
    if (len2 > 0) {
        t256 = (wx*vx + wy*vy) * 256 / len2;
        if (t256 < 0)   t256 = 0;
        if (t256 > 256) t256 = 256;
    }
    long qx = x0*256L + vx*t256, qy = y0*256L + vy*t256;   // en 1/256 px
    long dx = cx*256L - qx,      dy = cy*256L - qy;
    long r256 = r*256L;
    return dx*dx + dy*dy <= r256*r256;
}
static int count_paras_air(void) {
    int n = 0;
    for (int i=0;i<MAX_PARAS;i++)
        if (paras[i].active && !paras[i].landed) n++;
    return n;
}

// ---------------------------------------------------------------------------
// Partículas
// ---------------------------------------------------------------------------
static void spawn_expl(int wx, int wy, int n, int spd, int life) {
    static const int16_t S32[32] = {
        0,50,98,142,181,213,237,251,256,251,237,213,181,142,98,50,
        0,-50,-98,-142,-181,-213,-237,-251,-256,-251,-237,-213,-181,-142,-98,-50};
    int done = 0;
    for (int i=0;i<MAX_PARTICLES && done<n;i++) {
        if (particles[i].active) continue;
        int a = rnd(32), s = spd/2 + rnd(spd/2+1);
        particles[i].active = true;
        particles[i].x = PX2FP(wx); particles[i].y = PX2FP(wy);
        particles[i].vx = (int32_t)s*S32[(a+8)&31]/256;
        particles[i].vy = (int32_t)s*S32[a]/256;
        particles[i].life = life + rnd(life/3+1);
        particles[i].life_max = particles[i].life;
        done++;
    }
}
static void upd_particles(void) {
    for (int i=0;i<MAX_PARTICLES;i++) {
        Particle *p = &particles[i]; if (!p->active) continue;
        p->vy += FP/8;   // gravedad, aplicada tal cual una vez por tick
        p->x  += p->vx * g_dt_scale / FP;
        p->y  += p->vy * g_dt_scale / FP;
        if (--p->life <= 0) p->active = false;
    }
}

// ---------------------------------------------------------------------------
// Primitivas de dibujo vectorial -- líneas por Bresenham, 1px (a esta
// escala ya reducida no hace falta engordarlas como en asteroids.c).
// ---------------------------------------------------------------------------
static void line(int x0, int y0, int x1, int y1, uint16_t color) {
    int dx = x1-x0; if (dx<0) dx=-dx;
    int dy = y1-y0; if (dy<0) dy=-dy;
    int sx = x0<x1 ? 1:-1, sy = y0<y1 ? 1:-1;
    int err = dx - dy;
    for (;;) {
        if (x0>=0 && y0>=0) renderer_fill_rect(x0, y0, 1, 1, color);
        if (x0==x1 && y0==y1) break;
        int e2 = 2*err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

// Isqrt entero de num/den por Newton -- usado por las dos primitivas
// rellenas de abajo (cúpula, dosel).
static int isqrt_ratio(int32_t num, int32_t den) {
    if (num <= 0) return 0;
    int32_t q = num/den;
    int w = 0;
    if (q > 0) {
        int32_t r = q;
        for (int k=0;k<8;k++) { int32_t r2=(r+q/r)>>1; if (r2>=r) break; r=r2; }
        w = (int)r;
    }
    while ((int32_t)(w+1)*(w+1)*den <= num) w++;
    return w;
}

// Semielipse superior rellena (cúpula de la torreta).
static void fill_top_ellipse(int cx, int cy, int rx, int ry, uint16_t color) {
    for (int row=0; row<=ry; row++) {
        int w = isqrt_ratio((int32_t)rx*rx*((int32_t)ry*ry-(int32_t)row*row), (int32_t)ry*ry);
        if (w <= 0 && row < ry) continue;
        renderer_fill_rect(cx-w, cy-row, 2*w+1, 1, color);
    }
}

// Elipse completa rellena (fuselaje del helicóptero).
static void fill_ellipse(int cx, int cy, int rx, int ry, uint16_t color) {
    for (int row=-ry; row<=ry; row++) {
        int w = isqrt_ratio((int32_t)rx*rx*((int32_t)ry*ry-(int32_t)row*row), (int32_t)ry*ry);
        if (w <= 0) continue;
        renderer_fill_rect(cx-w, cy+row, 2*w+1, 1, color);
    }
}

// Dosel de paracaídas: ancho arriba, estrecho abajo (cy = borde inferior,
// de donde arrancan las cuerdas).
static void draw_chute_filled(int cx, int cy, int rw, int rh, uint16_t color) {
    for (int row=0; row<=rh; row++) {
        int w = isqrt_ratio((int32_t)rw*rw*(rh-row), rh);
        if (row < rh && w < 1) w = 1;
        if (w > 0) renderer_fill_rect(cx-w, cy-row, 2*w+1, 1, color);
    }
}

// ---------------------------------------------------------------------------
// Colores -- paleta CGA de 4 colores (negro/cian/magenta/blanco), como
// en el original: torreta base blanca + cañón cian, helicópteros cuerpo
// blanco + detalles magenta, avión cian.
// ---------------------------------------------------------------------------
#define COLOR_TURRET_BASE  COLOR_WHITE
#define COLOR_TURRET_GUN   COLOR_CYAN
#define COLOR_HELI_BODY    COLOR_WHITE
#define COLOR_HELI_ACCENT  COLOR_MAGENTA
#define COLOR_SOLDIER      COLOR_GREEN
#define COLOR_CHUTE        COLOR_CYAN
#define COLOR_JET          COLOR_CYAN
#define COLOR_BOMB         COLOR_MAGENTA
#define COLOR_BULLET       COLOR_WHITE
#define COLOR_PART         COLOR_MAGENTA

// ---------------------------------------------------------------------------
// Gestión de ranuras y torre
// ---------------------------------------------------------------------------
static void kill_slot(int side, int slot) {
    int n = 0;
    for (int i=0;i<MAX_PARAS;i++) {
        Para *p = &paras[i];
        if (!p->active || !p->landed || p->side!=side || p->slot!=slot) continue;
        spawn_expl(FP2PX(p->x), GROUND_Y - p->stack*SOLDIER_H, 4, 3, 14);
        p->active = false; n++;
    }
    if (n > 0) {
        score += PTS_PARA_GROUND*n;
        slot_count[side][slot] = 0;
        ground_total[side] -= n;
        if (ground_total[side] < 0) ground_total[side] = 0;
        if (tower_active && tower_side==side && ground_total[side]<TOWER_COUNT) {
            tower_active = false; helis_frozen = false;
        }
        sound_effect_explosion();
    }
}

static int find_slot(int side, int land_x) {
    int best=-1, bd=0x7fffffff;
    for (int n=0;n<NUM_SLOTS;n++) {
        if (slot_count[side][n]>=SLOT_STACK) continue;
        int d = absi(SLOT_X(side,n)-land_x);
        if (d<bd) { bd=d; best=n; }
    }
    if (best<0) {
        for (int n=0;n<NUM_SLOTS;n++) {
            int d = absi(SLOT_X(side,n)-land_x);
            if (d<bd) { bd=d; best=n; }
        }
    }
    return best;
}

static int pick_slot_x(int side) {
    int tries[NUM_SLOTS];
    for (int i=0;i<NUM_SLOTS;i++) tries[i]=i;
    for (int i=NUM_SLOTS-1;i>0;i--) {
        int j=rnd(i+1); int t=tries[i]; tries[i]=tries[j]; tries[j]=t;
    }
    for (int i=0;i<NUM_SLOTS;i++) {
        int n = tries[i];
        if (slot_count[side][n]<SLOT_STACK) return SLOT_X(side,n);
    }
    return SLOT_X(side, rnd(NUM_SLOTS));
}

// ---------------------------------------------------------------------------
// Reset de ola / inicio de partida
// ---------------------------------------------------------------------------
static void reset_wave(void) {
    for (int i=0;i<MAX_HELIS;i++) helis[i].active=false;
    for (int i=0;i<MAX_PARAS;i++) if (!paras[i].landed) paras[i].active=false;
    for (int i=0;i<MAX_BULLETS;i++) bullets[i].active=false;
    for (int i=0;i<MAX_BOMBS;i++)   bombs[i].active=false;
    for (int i=0;i<MAX_PLANES;i++) planes[i].active=false;
    for (int i=0;i<MAX_PARTICLES;i++) particles[i].active=false;
    for (int s=0;s<2;s++) {
        ground_total[s]=0;
        for (int n=0;n<NUM_SLOTS;n++) slot_count[s][n]=0;
    }
    tower_active=false; helis_frozen=false;
    wave_done=false; jet_delay=0; jet_launched=false; jets_left=0; jet_gap_ms=0;
    attack_mode=false; attack_timer=0;
    attack_cooldown=ATTACK_INTERVAL_MS;
    wave_heli_budget = 12 + wave*2; if (wave_heli_budget>48) wave_heli_budget=48;
    wave_helis_spawned=0;
    lane_timer[0]=1000;
    lane_timer[1]=2000+rnd(1000);
    sound_siren_stop(); // por si el avión seguía sonando
}

static void game_start(void) {
    score=0; wave=1;
    cannon_angle_deg=ANGLE_UP_DEG; cannon_rot_dir=0;
    cannon_alive=true; cannon_inv=TICKS_S*2; cannon_enc_acc=0; cannon_rot_acc=0;
    fire_held=false;
    reset_wave();
    // Jingle de inicio (una sola pasada, ~8.6s) -- se apaga solo y a
    // partir de ahí solo se oyen los efectos (sound_effect_shoot/
    // explosion/..., y la sirena del avión, que cede/retoma el canal 2
    // sin cortar la música mientras suena -- ver sound_update() y
    // sound_siren_stop() en sound.c).
    sound_start_paratrooper_music();
    state=PT_PLAYING;
}

// ---------------------------------------------------------------------------
// Spawn
// ---------------------------------------------------------------------------
static int drop_rate_pct(void) {
    int p = 100 + (wave-1) * DROP_WAVE_STEP_PCT;
    return p > DROP_WAVE_MAX_PCT ? DROP_WAVE_MAX_PCT : p;
}

// Tiempo (ms) hasta la siguiente comprobación de suelta, ya escalado por ola.
static int drop_ms(int base_min, int range) {
    return (base_min + rnd(range)) * 100 / drop_rate_pct();
}

static int max_paras_air(void) {
    int m = PARAS_AIR_BASE + (wave-1)/2;
    return m > PARAS_AIR_MAX ? PARAS_AIR_MAX : m;
}

static void spawn_heli(int lane) {
    int dir = (lane==0) ? 1 : -1;
    int lane_y = (lane==0) ? LANE0_Y : LANE1_Y;
    int entry_x = (dir==1) ? (PLAY_X-20) : (PLAY_X+PLAY_W+20);

    for (int i=0;i<MAX_HELIS;i++) {
        if (!helis[i].active) continue;
        if (helis[i].y != lane_y) continue;
        int hx = FP2PX(helis[i].x);
        if (absi(hx-entry_x) < HELI_MIN_SEP) return;
    }

    for (int i=0;i<MAX_HELIS;i++) {
        if (helis[i].active) continue;
        helis[i].active = true;
        helis[i].x = PX2FP(entry_x);
        helis[i].y = lane_y;
        helis[i].vx = dir*HELI_SPD;
        helis[i].drop_timer = drop_ms(DROP_FIRST_MIN_MS, DROP_FIRST_RANGE_MS);
        helis[i].anim_timer = 0;
        helis[i].rotor_frame = 0; helis[i].tail_frame = 0;
        break;
    }
}

static void spawn_para_at_slot(int side, int hy) {
    if (count_paras_air() >= max_paras_air()) return;
    int sx = pick_slot_x(side);
    for (int i=0;i<MAX_PARAS;i++) {
        if (paras[i].active) continue;
        Para *p = &paras[i];
        p->active = true;
        p->x = PX2FP(sx); p->y = PX2FP(hy+8);
        p->vy = PX2FP(1);
        p->chute_open = p->chute_shot = p->landed = false;
        p->marching = p->climbing = false;
        p->march_order = -1;
        p->target_x = p->target_y = 0;
        p->slot = -1; p->stack = 0;
        p->side = side;
        p->anim_timer = 0;
        p->chute_open_y = CHUTE_OPEN_Y_MIN + rnd(CHUTE_OPEN_Y_MAX-CHUTE_OPEN_Y_MIN);
        break;
    }
}

static void spawn_para(int hx, int hy) {
    int side = (hx < TURRET_X) ? 0 : 1;
    spawn_para_at_slot(side, hy);
}

static void player_fire(void) {
    for (int i=0;i<MAX_BULLETS;i++) {
        if (bullets[i].active) continue;
        int dx = can_dx(cannon_angle_deg), dy = can_dy(cannon_angle_deg);
        int tip_x = CANNON_OX + dx*CANNON_LEN/256;
        int tip_y = CANNON_OY + dy*CANNON_LEN/256;
        bullets[i].active = true;
        bullets[i].x = PX2FP(tip_x); bullets[i].y = PX2FP(tip_y);
        bullets[i].px = bullets[i].x; bullets[i].py = bullets[i].y;
        bullets[i].vx = (int32_t)BULLET_SPD*dx/256*FP;
        bullets[i].vy = (int32_t)BULLET_SPD*dy/256*FP;
        if (score > 0) score--;
        sound_effect_shoot();
        return;
    }
}

static int planes_active(void) {
    int n = 0;
    for (int i=0;i<MAX_PLANES;i++) if (planes[i].active) n++;
    return n;
}

static bool bombs_active(void) {
    for (int i=0;i<MAX_BOMBS;i++) if (bombs[i].active) return true;
    return false;
}

static void launch_jet(int dir) {
    for (int i=0;i<MAX_PLANES;i++) {
        if (planes[i].active) continue;
        Plane *pl = &planes[i];
        if (planes_active() == 0) sound_siren_start();   // la sirena suena mientras haya alguno
        pl->active = true;
        pl->vx = dir*PLANE_SPD;
        pl->x = PX2FP((dir==1) ? (PLAY_X-30) : (PLAY_X+PLAY_W+30));
        pl->y = PX2FP(PLAY_Y+12+rnd(25));
        pl->bomb_dropped = 0;
        pl->anim_timer = 0;
        // Punto de suelta: por POSICIÓN en pantalla, no por número de frames.
        // Antes la bomba se soltaba a los 30 frames de vida del avión; si el
        // juego iba más rápido de 60 fps el avión aún no había entrado, la
        // bomba nacía fuera de pantalla y se descartaba al instante.
        // Se suelta POCO DESPUÉS DE ENTRAR (PLANE_DROP_MIN..+RANGE px dentro de
        // la pantalla por el lado de entrada) y no en mitad del recorrido:
        // así la bomba tiene que cruzar casi media pantalla hasta la torreta
        // y describe una parábola amplia, en vez de caer casi en vertical.
        int off = PLANE_DROP_MIN + rnd(PLANE_DROP_RANGE);
        pl->drop_x = (dir==1) ? (PLAY_X + off) : (PLAY_X + PLAY_W - off);
        return;
    }
}

// Planificador de la pasada de aviones. Se llama cada tick mientras quedan
// aviones por lanzar. Reglas:
//  - espera jet_gap_ms (aleatorio, en ms reales) desde el último lanzamiento;
//  - no lanza si algún avión en pantalla va en sentido contrario (para cambiar
//    de lado hay que esperar a que la pantalla esté libre de aviones);
//  - no lanza si el último avión del mismo lado aún está a menos de
//    PLANE_MIN_SEP px de la entrada (no se solapan).
static void update_jet_schedule(void) {
    if (jets_left <= 0) return;
    jet_gap_ms -= g_elapsed_ms;
    if (jet_gap_ms > 0) return;

    int dir = jet_next_dir;
    int entry_x = (dir==1) ? (PLAY_X-30) : (PLAY_X+PLAY_W+30);
    for (int i=0;i<MAX_PLANES;i++) {
        if (!planes[i].active) continue;
        if ((planes[i].vx > 0 ? 1 : -1) != dir) return;
        if (absi(FP2PX(planes[i].x) - entry_x) < PLANE_MIN_SEP) return;
    }
    launch_jet(dir);
    jets_left--;
    jet_next_dir = rnd(2) ? 1 : -1;
    jet_gap_ms = JET_GAP_MIN_MS + rnd(JET_GAP_RANGE_MS);
}

// ---------------------------------------------------------------------------
// Update helicópteros
// ---------------------------------------------------------------------------
static void update_helis(void) {
    for (int i=0;i<MAX_HELIS;i++) {
        Heli *h = &helis[i]; if (!h->active) continue;
        h->anim_timer++;
        if (h->anim_timer >= 3) {
            h->anim_timer = 0;
            h->rotor_frame = (h->rotor_frame+1) & 3;
            h->tail_frame  = (h->tail_frame+1) & 1;
        }
        if (!helis_frozen) h->x += (int32_t)h->vx * g_dt_scale;
        int hx = FP2PX(h->x);
        if (hx < PLAY_X-60 || hx > PLAY_X+PLAY_W+60) { h->active=false; continue; }
        if (!tower_active && !wave_done && cannon_alive) {
            h->drop_timer -= g_elapsed_ms;
            if (h->drop_timer <= 0) {
                /*
                 * Antes, en modo ataque DROP_PROB_ATTACK=1 hacía que CADA
                 * helicóptero soltara un paracaidista con un 100% de
                 * probabilidad cada 0.5-1s -- con varios helis a la vez
                 * eso los tiraba casi todos de golpe. Ahora el intervalo
                 * de comprobación es más corto (más a menudo se decide si
                 * toca soltar), pero la probabilidad real de soltar en
                 * cada comprobación es baja, así que en la práctica caen
                 * uno a uno, repartidos en el tiempo, en vez de en racha.
                 */
                int base_interval = attack_mode
                    ? drop_ms(450, 400)   // modo ataque: 450-850 ms en la ola 1 (antes 330-660), y también se acorta con las olas
                    : drop_ms(DROP_INTERVAL_MIN_MS, DROP_INTERVAL_RANGE_MS);
                h->drop_timer = base_interval;
                int prob = attack_mode ? DROP_PROB_ATTACK : DROP_PROB;
                if (rnd(prob) == 0) {
                    spawn_para(FP2PX(h->x), h->y);
                    sound_effect_move();
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Update paracaidistas
// ---------------------------------------------------------------------------
static void update_paras(void) {
    for (int i=0;i<MAX_PARAS;i++) {
        Para *p = &paras[i]; if (!p->active || p->landed) continue;
        p->anim_timer++; if (p->anim_timer >= 6) p->anim_timer = 0;

        if (p->chute_shot) {
            p->vy += PX2FP(1)/3; if (p->vy > PX2FP(6)) p->vy = PX2FP(6);
        } else if (p->chute_open) {
            p->vy = FP*3/4;   // antes 1px/tick (FP) -- ahora 0.75px/tick, más fácil de acertar
        } else {
            p->vy += PX2FP(1)/6; if (p->vy > PX2FP(3)) p->vy = PX2FP(3);
            if (FP2PX(p->y) >= p->chute_open_y) { p->chute_open = true; p->vy = FP*3/4; }
        }
        p->y += p->vy * g_dt_scale / FP;

        int side = (FP2PX(p->x) < TURRET_X) ? 0 : 1;

        // El soldado en el aire se dibuja SOLDIER_H px POR DEBAJO de p->y (p->y
        // es el enganche del paracaídas). Antes se aterrizaba cuando p->y llegaba
        // a GROUND_Y, o sea con los pies 15 px por DEBAJO de la línea del suelo:
        // se veía hundirse en el suelo y luego "saltar" a su sitio. Ahora se
        // aterriza cuando los PIES tocan el suelo o la cabeza del último soldado
        // apilado en esa ranura.
        int fall_x   = FP2PX(p->x);
        int fall_slot = find_slot(side, fall_x);
        int land_top = GROUND_Y - ((fall_slot >= 0) ? slot_count[side][fall_slot] : 0) * SOLDIER_H;
        bool feet_down = (FP2PX(p->y) + SOLDIER_H >= land_top);

        if (p->chute_shot && feet_down) {
            int lx = FP2PX(p->x);
            int kill_s = -1;
            if (p->slot>=0 && slot_count[side][p->slot]>0) {
                kill_s = p->slot;
            } else {
                for (int n=0;n<NUM_SLOTS;n++)
                    if (slot_count[side][n]>0 && SLOT_X(side,n)==lx) { kill_s=n; break; }
            }
            if (kill_s>=0) kill_slot(side, kill_s);
            spawn_expl(lx, land_top, 5, 3, 14);
            p->active = false; continue;
        }

        if (!p->chute_shot && feet_down) {
            p->y = PX2FP(GROUND_Y);
            p->landed = true; p->side = side;
            p->marching = false; p->climbing = false;
            p->march_order = -1;

            int land_x = FP2PX(p->x);
            int sn = find_slot(side, land_x);
            p->slot = sn;
            p->stack = slot_count[side][sn];
            slot_count[side][sn]++;
            p->x = PX2FP(SLOT_X(side,sn));
            p->y = PX2FP(GROUND_Y - p->stack*SOLDIER_H);

            ground_total[side]++;
            sound_effect_move();

            if (ground_total[side] >= TOWER_COUNT && !tower_active) {
                tower_active = true;
                tower_side = side;
                helis_frozen = true;
                sound_effect_lose_point();
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Update torre -- misma secuencia de conquista que el original; la marcha y
// la escalada van ahora por tiempo real (MARCH_SPD/CLIMB_SPD x g_dt_scale).
// ---------------------------------------------------------------------------
static void update_tower(void) {
    if (!tower_active) return;

    int dir = (tower_side==0) ? 1 : -1;
    int base_edge = (tower_side==0) ? (TURRET_X - BASE_W/2) : (TURRET_X + BASE_W/2);

    {
        bool needs_assign = false;
        for (int i=0;i<MAX_PARAS;i++) {
            Para *p = &paras[i];
            if (p->active && p->landed && p->side==tower_side && p->march_order<0)
                needs_assign = true;
        }
        if (needs_assign) {
            int idx[TOWER_COUNT]; int cnt=0;
            for (int i=0;i<MAX_PARAS && cnt<TOWER_COUNT;i++) {
                Para *p = &paras[i];
                if (p->active && p->landed && p->side==tower_side && p->march_order<0)
                    idx[cnt++] = i;
            }
            for (int a=0;a<cnt-1;a++)
                for (int b=a+1;b<cnt;b++) {
                    Para *pa=&paras[idx[a]], *pb=&paras[idx[b]];
                    bool swap=false;
                    if (pb->stack > pa->stack) swap=true;
                    else if (pb->stack==pa->stack) {
                        int da=absi(FP2PX(pa->x)-base_edge), db=absi(FP2PX(pb->x)-base_edge);
                        if (db<da) swap=true;
                    }
                    if (swap) { int t=idx[a]; idx[a]=idx[b]; idx[b]=t; }
                }
            for (int a=0;a<cnt;a++) paras[idx[a]].march_order = a;
        }
    }

    int32_t march_step = MARCH_SPD * g_dt_scale / FP;
    int32_t climb_step = CLIMB_SPD * g_dt_scale / FP;
    if (march_step < 1) march_step = 1;
    if (climb_step < 1) climb_step = 1;

    Para *sol[TOWER_COUNT]; for (int k=0;k<TOWER_COUNT;k++) sol[k]=NULL;
    for (int i=0;i<MAX_PARAS;i++) {
        Para *p = &paras[i];
        if (p->active && p->landed && p->side==tower_side && p->march_order>=0 && p->march_order<TOWER_COUNT)
            sol[p->march_order] = p;
    }

    int target_x[TOWER_COUNT], target_y[TOWER_COUNT];
    int beside = base_edge - dir*(SOLDIER_H/2+2);
    target_x[0]=base_edge;  target_y[0]=GROUND_Y;
    target_x[1]=base_edge;  target_y[1]=GROUND_Y-SOLDIER_H;
    target_x[2]=beside;     target_y[2]=GROUND_Y;
    target_x[3]=base_edge;  target_y[3]=CANNON_OY;

    for (int k=0;k<TOWER_COUNT;k++) {
        Para *p = sol[k];
        if (!p) continue;

        bool prev_ready = true;
        if (k>0) {
            Para *prev = sol[k-1];
            if (!prev) prev_ready = false;
            else {
                int px=FP2PX(prev->x), py=FP2PX(prev->y);
                prev_ready = (absi(px-target_x[k-1])<=1 && absi(py-target_y[k-1])<=1);
            }
        }
        if (!prev_ready) continue;

        int cx=FP2PX(p->x), cy=FP2PX(p->y);
        int tx=target_x[k], ty=target_y[k];

        bool stacked_descending = (p->stack>0) && (cy<GROUND_Y);
        if (stacked_descending) {
            p->y += climb_step;
            if (FP2PX(p->y) >= GROUND_Y) { p->y = PX2FP(GROUND_Y); p->stack = 0; }
            continue;
        }

        bool at_x = absi(cx-tx)<=1;
        bool at_y = absi(cy-ty)<=1;
        p->marching = !at_x;
        p->climbing = at_x && !at_y;

        if (!at_x) {
            int step = (tx>cx) ? 1 : -1;
            p->x += step * march_step;
            if ((step>0 && FP2PX(p->x)>tx) || (step<0 && FP2PX(p->x)<tx)) p->x = PX2FP(tx);
        } else if (at_x && !at_y) {
            int step = (ty<cy) ? -1 : 1;
            p->y += step * climb_step;
            if ((step<0 && FP2PX(p->y)<ty) || (step>0 && FP2PX(p->y)>ty)) p->y = PX2FP(ty);
        }

        if (k==3 && absi(FP2PX(p->x)-tx)<=1 && absi(FP2PX(p->y)-ty)<=2) {
            cannon_alive = false; tower_active = false;
            spawn_expl(TURRET_X, CANNON_OY, 20, 5, 35);
            sound_effect_explosion();
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Update avión / bombas / balas
// ---------------------------------------------------------------------------

/*
 * Ticks (nominales) que le quedan a la bomba hasta llegar a la
 * ALTURA DE LA TORRETA (CANNON_OY) -- que es donde check_collisions()
 * de verdad comprueba si la bomba acierta, NO cuando toca el suelo
 * (GROUND_Y, bastante más abajo). Antes se apuntaba a GROUND_Y: la
 * corrección de rumbo (vx) se repartía en el tiempo hasta tocar
 * suelo, así que al llegar a la altura de la torreta -- donde de
 * verdad se juzga el impacto -- la bomba aún no había terminado de
 * centrarse, y a veces fallaba por poco aunque "en teoría" iba a
 * caer justo encima. Apuntando a CANNON_OY, la x ya está corregida
 * del todo para cuando importa.
 *
 * Se usa tanto al lanzarla como, cada tick, para RECALCULAR la vx
 * necesaria -- así el impacto siempre cae justo en TURRET_X sin
 * importar que el dt real fluctúe de un tick a otro (antes se
 * calculaba una vez al lanzar asumiendo un tick nominal fijo, y con
 * el dt real variable el número de ticks reales hasta llegar no
 * coincidía con el planeado, así que la bomba caía a un lado u otro
 * de la torreta en vez de encima).
 */
static int bomb_ticks_to_ground(int32_t y, int32_t vy, int32_t ay) {
    int32_t sim_vy=vy, sim_y=y;
    for (int t=1; t<=500; t++) {
        sim_vy += ay; sim_y += sim_vy;
        if (FP2PX(sim_y) >= CANNON_OY) return t;
    }
    return 500;
}

static void update_planes(void) {
    for (int pi=0; pi<MAX_PLANES; pi++) {
        Plane *pl = &planes[pi];
        if (!pl->active) continue;
        pl->anim_timer++; if (pl->anim_timer>=3) pl->anim_timer=0;
        pl->x += (int32_t)pl->vx * g_dt_scale;

        int px = FP2PX(pl->x);
        if (pl->bomb_dropped == 0) {
            bool reached = (pl->vx > 0) ? (px >= pl->drop_x) : (px <= pl->drop_x);
            if (reached) {
                for (int bi=0;bi<MAX_BOMBS;bi++) {
                    if (bombs[bi].active) continue;
                    pl->bomb_dropped = 1;
                    int32_t bx = pl->x;
                    int32_t by = pl->y + PX2FP(6);
                    /*
                     * Caída lenta a propósito (antes tardaba ~1.8s en tocar
                     * el suelo, sin apenas margen para dispararle; ahora
                     * tarda ~3s) para que dé tiempo real a acertarla.
                     */
                    int32_t vy0 = 24, ay = 3;
                    int tof = bomb_ticks_to_ground(by, vy0, ay);
                    int32_t vx = (PX2FP(TURRET_X) - bx) / tof;
                    bombs[bi].active=true;
                    bombs[bi].x=bx; bombs[bi].y=by;
                    bombs[bi].vx=vx; bombs[bi].vy=vy0; bombs[bi].ay=ay;
                    sound_play_tone(220, 60);
                    break;
                }
                // Si no había hueco de bomba libre, reintenta en el siguiente tick.
            }
        }
        if (px<PLAY_X-50 || px>PLAY_X+PLAY_W+50) {
            pl->active=false;
            if (planes_active() == 0) sound_siren_stop();
        }
    }
}

static void update_bombs(void) {
    for (int i=0;i<MAX_BOMBS;i++) {
        Bomb *b = &bombs[i]; if (!b->active) continue;

        /*
         * Solo recalculamos vx (homing hacia TURRET_X) mientras la
         * bomba TODAVÍA no ha llegado a la altura de la torreta.
         * Una vez pasada esa altura (falló o ya no hay nada que
         * comprobar), "rem" se quedaría clavado en 1 tick para
         * siempre y cada frame intentaría cerrar TODA la distancia
         * restante de golpe -- un bandazo visible en la caída en
         * vez de seguir recta hasta salir de pantalla.
         */
        if (FP2PX(b->y) < CANNON_OY) {
            int rem = bomb_ticks_to_ground(b->y, b->vy, b->ay);
            if (rem < 1) rem = 1;
            b->vx = (PX2FP(TURRET_X) - b->x) / rem;
        }


        b->vy += b->ay;
        b->x  += b->vx * g_dt_scale / FP;
        b->y  += b->vy * g_dt_scale / FP;
        if (FP2PX(b->y) > PLAY_Y+PLAY_H ||
            FP2PX(b->x) < PLAY_X || FP2PX(b->x) > PLAY_X+PLAY_W) {
            b->active = false; score += PTS_BOMB_DODGE;
        }
    }
}

static void update_bullets(void) {
    for (int i=0;i<MAX_BULLETS;i++) {
        Bullet *b = &bullets[i]; if (!b->active) continue;
        b->px = b->x; b->py = b->y;
        b->x += b->vx * g_dt_scale / FP;
        b->y += b->vy * g_dt_scale / FP;
        int bx=FP2PX(b->x), by=FP2PX(b->y);
        if (bx<PLAY_X || bx>PLAY_X+PLAY_W || by<PLAY_Y || by>PLAY_Y+PLAY_H)
            b->active = false;
    }
}

// ---------------------------------------------------------------------------
// Colisiones -- idéntica lógica al original, radios reescalados.
// ---------------------------------------------------------------------------
static void check_collisions(void) {
    for (int bi=0;bi<MAX_BULLETS;bi++) {
        Bullet *bu = &bullets[bi]; if (!bu->active) continue;
        int bx=FP2PX(bu->x), by=FP2PX(bu->y);

        for (int i=0;i<MAX_HELIS;i++) {
            if (!helis[i].active) continue;
            if (chit(bx,by,3, FP2PX(helis[i].x),helis[i].y,10)) {
                bu->active = helis[i].active = false;
                spawn_expl(FP2PX(helis[i].x),helis[i].y,10,4,20);
                score += PTS_HELI; sound_effect_explosion(); break;
            }
        }
        if (!bu->active) continue;

        for (int i=0;i<MAX_PARAS;i++) {
            Para *p = &paras[i];
            if (!p->active || p->landed || p->chute_shot) continue;
            int px=FP2PX(p->x), py=FP2PX(p->y);
            /*
             * Dos hitboxes bien separadas (antes se solapaban casi del
             * todo y en la práctica siempre "ganaba" el dosel):
             *  - Dosel: solo existe con chute_open, centrado en el propio
             *    dosel (py-10, arriba del cuerpo) -- acertarlo solo corta
             *    las cuerdas (chute_shot=true), no mata.
             *  - Soldado: centrado en el cuerpo que cuelga (py+SOLDIER_H/2,
             *    la posición real donde se dibuja con draw_soldier_at),
             *    tanto con el dosel abierto como en caída libre -- acertarlo
             *    mata directamente.
             */
            if (p->chute_open && chit(bx,by,3, px,py-10,8)) {
                bu->active=false; p->chute_open=false; p->chute_shot=true;
                p->vy=PX2FP(2); score+=PTS_PARA_CHUTE_HIT;
                sound_effect_explosion(); break;
            }
            if (chit(bx,by,3, px,py+SOLDIER_H/2, SOLDIER_H/2+3)) {
                bu->active=p->active=false;
                spawn_expl(px,py+SOLDIER_H/2,5,3,14);
                score+=PTS_PARA_AIR; sound_effect_explosion(); break;
            }
        }
        if (!bu->active) continue;

        for (int i=0;i<MAX_PARAS;i++) {
            Para *p = &paras[i]; if (!p->active || !p->landed) continue;
            int px = FP2PX(p->x);
            int top_y = GROUND_Y - slot_count[p->side][p->slot]*SOLDIER_H;
            if (bx>=px-4 && bx<=px+4 && by>=top_y && by<=GROUND_Y) {
                bu->active=false; kill_slot(p->side,p->slot); break;
            }
        }
        if (!bu->active) continue;

        for (int pi=0; pi<MAX_PLANES; pi++) {
            Plane *pl = &planes[pi];
            if (!pl->active) continue;
            if (chit(bx,by,3,FP2PX(pl->x),FP2PX(pl->y),9)) {
                bu->active = pl->active = false;
                spawn_expl(FP2PX(pl->x),FP2PX(pl->y),14,5,26);
                score += PTS_PLANE; sound_effect_explosion();
                if (planes_active() == 0) sound_siren_stop();
                break;
            }
        }
        if (!bu->active) continue;

        for (int i=0;i<MAX_BOMBS;i++) {
            if (!bombs[i].active) continue;
            int ox=FP2PX(bombs[i].x), oy=FP2PX(bombs[i].y);
            // Zona protegida: una bomba a menos de BOMB_SAFE_R px del cañón ya
            // no se puede destruir (si no, acertarla en el último momento
            // es demasiado fácil). La zona es un CÍRCULO alrededor del cañón,
            // no una columna de 30 px como la versión antigua: aquella cubría
            // toda la caída si el avión soltaba cerca de la torreta y la bomba
            // parecía indestructible. Como ahora los aviones sueltan la bomba
            // cerca del borde de entrada (>= 85 px de la torreta), la bomba
            // siempre sale fuera de la zona y es alcanzable durante casi todo
            // su recorrido; solo se protege el tramo final de la aproximación.
            int sdx = ox - CANNON_OX, sdy = oy - CANNON_OY;
            if (sdx*sdx + sdy*sdy < BOMB_SAFE_R*BOMB_SAFE_R) continue;
            if (seg_hit(FP2PX(bu->px), FP2PX(bu->py), bx, by, ox, oy, BOMB_HIT_R)) {
                bu->active = bombs[i].active = false;
                spawn_expl(ox,oy,6,3,16);
                score+=PTS_PARA_AIR; sound_effect_explosion(); break;
            }
        }
    }

    if (!cannon_alive || cannon_inv>0) return;

    for (int i=0;i<MAX_BOMBS;i++) {
        Bomb *b = &bombs[i]; if (!b->active) continue;
        int bx = FP2PX(b->x);
        int by = FP2PX(b->y);

        // Si la bomba baja a la altura de la torreta (o más) y está
        // alineada horizontalmente con ella, impacta.
        if (by >= CANNON_OY && absi(bx - TURRET_X) < (BASE_W / 2 + 4)) {
            b->active = cannon_alive = false;
            spawn_expl(CANNON_OX, CANNON_OY, 18, 5, 32);
            sound_effect_explosion();
            return;
        }
    }
    for (int i=0;i<MAX_HELIS;i++) {
        if (!helis[i].active) continue;
        if (chit(FP2PX(helis[i].x),helis[i].y,10,CANNON_OX,CANNON_OY,TURRET_R)) {
            helis[i].active = cannon_alive = false;
            spawn_expl(CANNON_OX,CANNON_OY,14,5,28);
            sound_effect_explosion(); return;
        }
    }
}

static bool wave_clear(void) {
    if (wave_helis_spawned < wave_heli_budget) return false;
    for (int i=0;i<MAX_HELIS;i++) if (helis[i].active) return false;
    for (int i=0;i<MAX_PARAS;i++) if (paras[i].active && !paras[i].landed) return false;
    return true;
}

// ---------------------------------------------------------------------------
// DRAW: primitivas de entidades
// ---------------------------------------------------------------------------
static void draw_turret(void) {
    if (!cannon_alive) return;
    if (cannon_inv>0 && (cannon_inv/4)%2==0) return;

    int tx=TURRET_X, ty=TURRET_Y;
    int base_top = ty - BASE_H;

    renderer_fill_rect(tx-BASE_W/2, base_top, BASE_W, BASE_H, COLOR_TURRET_BASE);
    fill_top_ellipse(tx, base_top, BODY_RX, BODY_RY, COLOR_TURRET_BASE);
    renderer_fill_rect(tx-1, base_top-BODY_RY+2, 2, 2, COLOR_BLACK);

    int dx2=can_dx(cannon_angle_deg), dy2=can_dy(cannon_angle_deg);
    int tip_x = CANNON_OX + dx2*CANNON_LEN/256;
    int tip_y = CANNON_OY + dy2*CANNON_LEN/256;
    for (int t=-CANNON_W; t<=CANNON_W; t++) {
        int ox = (int)(t*(-dy2)/256);
        int oy = (int)(t*( dx2)/256);
        line(CANNON_OX+ox, CANNON_OY+oy, tip_x+ox, tip_y+oy, COLOR_TURRET_GUN);
    }
}

#define HELI_RX 11
#define HELI_RY 5

static void draw_heli(const Heli *h) {
    if (!h->active) return;
    int cx=FP2PX(h->x), cy=h->y;
    int d = (h->vx>=0) ? 1 : -1;

    fill_ellipse(cx, cy, HELI_RX, HELI_RY, COLOR_HELI_BODY);
    // hueco de cabina (marca la dirección de la nariz)
    renderer_fill_rect(cx + d*(HELI_RX-4), cy-1, 3, 3, COLOR_BLACK);

    int tail_len = 8;
    int tail_x = (d>0) ? (cx-HELI_RX-tail_len) : (cx+HELI_RX);
    renderer_fill_rect(tail_x, cy-1, tail_len, 2, COLOR_HELI_ACCENT);
    int mast_x = (d>0) ? (tail_x-1) : (tail_x+tail_len);

    if ((h->tail_frame & 1)==0)
        renderer_fill_rect(mast_x-3, cy-1, 7, 1, COLOR_HELI_ACCENT);
    else
        renderer_fill_rect(mast_x, cy-4, 1, 7, COLOR_HELI_ACCENT);

    int mx = cx + d;
    renderer_fill_rect(mx-1, cy-HELI_RY-3, 2, 3, COLOR_HELI_ACCENT);
    static const int8_t RL[4] = {10,7,4,1};
    int rl = RL[h->rotor_frame & 3];
    renderer_fill_rect(mx-rl, cy-HELI_RY-4, 2*rl+1, 1, COLOR_HELI_ACCENT);

    renderer_fill_rect(cx-HELI_RX+2, cy+HELI_RY,   2, 4, COLOR_HELI_ACCENT);
    renderer_fill_rect(cx+HELI_RX-4, cy+HELI_RY,   2, 4, COLOR_HELI_ACCENT);
    renderer_fill_rect(cx-HELI_RX,   cy+HELI_RY+4, 2*HELI_RX, 2, COLOR_HELI_ACCENT);
}

// Soldado, pies en py. walking anima las piernas (marcha/escalada).
static void draw_soldier_at(int cx, int py, bool walking, uint16_t color) {
    if (walking) {
        int phase = (blink/8) & 1;
        int off = (phase==0) ? 2 : -2;
        line(cx-2, py-6, cx-2+off, py, color);
        line(cx+2, py-6, cx+2-off, py, color);
    } else {
        renderer_fill_rect(cx-3, py-6, 2, 6, color);
        renderer_fill_rect(cx+1, py-6, 2, 6, color);
    }
    renderer_fill_rect(cx-2, py-11, 4, 6, color);              // tronco
    line(cx-4, py-10, cx+4, py-10, color);                     // brazos
    renderer_fill_rect(cx-2, py-15, 4, 4, color);               // cabeza
}

static void draw_para(const Para *p, uint16_t color) {
    if (!p->active) return;

    if (p->landed) {
        int px=FP2PX(p->x), py=FP2PX(p->y);
        bool walk = (tower_active && p->side==tower_side && (p->marching||p->climbing));
        draw_soldier_at(px, py, walk, color);
        return;
    }

    int px=FP2PX(p->x), py=FP2PX(p->y);

    if (p->chute_shot) {
        draw_soldier_at(px, py+SOLDIER_H, false, color);
        return;
    }
    if (p->chute_open) {
        int rw=9, rh=7;
        int db = py - rh;
        draw_chute_filled(px, db, rw, rh, COLOR_CHUTE);
        line(px-rw, db, px, py, COLOR_CHUTE);
        line(px+rw, db, px, py, COLOR_CHUTE);
        line(px-2, py, px+2, py, COLOR_CHUTE);
        draw_soldier_at(px, py+SOLDIER_H, false, color);
        return;
    }
    draw_soldier_at(px, py+SOLDIER_H, false, color);
}

static void draw_plane_sprite(const Plane *pl, uint16_t color) {
    if (!pl->active) return;
    int x=FP2PX(pl->x), y=FP2PX(pl->y), d=(pl->vx>0)?1:-1;
    renderer_fill_rect(x-11, y-1, 22, 2, color);
    line(x+d*11, y, x+d*16, y-1, color);
    line(x+d*11, y, x+d*16, y+1, color);
    line(x-d*4, y-1, x-d*10, y-5, color);
    line(x-d*4, y+1, x-d*10, y+5, color);
    line(x-d*10, y-5, x-d*10, y+5, color);
}

static void draw_bomb_sprite(const Bomb *b, uint16_t color) {
    if (!b->active) return;
    int bx=FP2PX(b->x), by=FP2PX(b->y);
    renderer_fill_rect(bx-2, by-3, 4, 5, color);
    renderer_fill_rect(bx-1, by-4, 2, 2, color);
}

// ---------------------------------------------------------------------------
// Render incremental -- borrado por caja delimitadora, un flush por
// entidad, mismo patrón que asteroids.c.
// ---------------------------------------------------------------------------
typedef struct { int x,y; bool active; } PrevPos;

static PrevPos prev_heli[MAX_HELIS];
static PrevPos prev_para[MAX_PARAS];
static bool    prev_para_landed[MAX_PARAS]; // tamaño de caja usado en el último borrado de cada para
static PrevPos prev_bomb[MAX_BOMBS];
static PrevPos prev_plane_pos[MAX_PLANES];
static PrevPos prev_turret = { .active=false };
static bool    prev_tower_region_active = false;
static bool    field_needs_redraw = true;
static int     prev_score = -1, prev_wave = -1;
static char    prev_center_msg[24] = "";
static char    prev_bottom_msg[40] = "";

/*
 * HELI_BOX_HW tiene que cubrir no solo el fuselaje (HELI_RX) sino la
 * cola completa: tail_len(8) + 1px de separación del mástil + el brazo
 * de la hélice de cola (3px a cada lado) = HELI_RX+8+1+3 = HELI_RX+12
 * en el peor caso. Antes era solo HELI_RX+3 -- por eso la hélice de
 * cola se quedaba sin borrar y dejaba un rastro al moverse.
 */
#define HELI_BOX_HW  (HELI_RX+14)
#define HELI_BOX_UP  (HELI_RY+8)
#define HELI_BOX_DN  (HELI_RY+8)
#define PARA_BOX_HW  12
#define PARA_BOX_UP  28
#define PARA_BOX_DN  18
/*
 * Caja reducida para el soldado YA ATERRIZADO (draw_soldier_at(px,py,...)
 * dibujado directamente en py, sin el offset +SOLDIER_H del paracaidista
 * en el aire): el sprite ocupa solo [py-15, py]. La caja grande de arriba
 * (pensada para el paracaídas abierto, que sobresale ~14px por encima de
 * py y deja el cuerpo ~15px por debajo) se queda muy holgada para este
 * caso -- y como los soldados apilados en una ranura están separados
 * exactamente SOLDIER_H=15px entre sí, un borrado de 28px hacia arriba
 * se comía 13px del soldado de la ranura de encima en cada tick (se
 * borraba y no se volvía a dibujar hasta el turno de ESE soldado en el
 * bucle, de ahí el parpadeo al estar juntos).
 */
#define PARA_LAND_BOX_UP  16
#define PARA_LAND_BOX_DN   2
#define BOMB_BOX_R   6
#define PLANE_BOX_HW 20
#define PLANE_BOX_HH 10
#define TURRET_BOX_HW  (BASE_W/2 + CANNON_LEN + 3)
#define TURRET_BOX_TOP (CANNON_OY - BODY_RY - CANNON_LEN - 2)
#define TURRET_BOX_H   (TURRET_Y - TURRET_BOX_TOP)

/*
 * Soldados aterrizados "estáticos" (parados en su ranura): antes se
 * borraban y redibujaban ENTEROS en cada frame, cada uno con su propio
 * renderer_flush() -- con 12 soldados en el suelo eran ~12 flushes y
 * cientos de fill_rect por frame solo para pintar lo mismo, y de ahí la
 * ralentización. Ahora solo se repintan cuando algo ha borrado parte de
 * su sprite (para_dirty, lo marca erase_box()) o se han movido.
 */
static bool para_dirty[MAX_PARAS];
static bool turret_dirty;
static int  prev_turret_angle = -1000;
static bool prev_turret_vis;

static void mark_paras_dirty(int x, int y, int w, int h) {
    for (int i=0;i<MAX_PARAS;i++) {
        const Para *p = &paras[i];
        if (!p->active || !p->landed) continue;
        int sx = FP2PX(p->x) - PARA_BOX_HW, sy = FP2PX(p->y) - PARA_LAND_BOX_UP;
        int sw = 2*PARA_BOX_HW,             sh = PARA_LAND_BOX_UP + PARA_LAND_BOX_DN;
        if (x < sx+sw && x+w > sx && y < sy+sh && y+h > sy) para_dirty[i] = true;
    }
    // Lo mismo con la torreta: una bala que sale por la boca del cañón o un
    // borrado vecino le muerde el sprite.
    int tx = TURRET_X-TURRET_BOX_HW, ty = TURRET_BOX_TOP;
    if (x < tx+2*TURRET_BOX_HW && x+w > tx && y < ty+TURRET_BOX_H && y+h > ty) turret_dirty = true;
}

// La línea del suelo son 2 px (GROUND_Y..GROUND_Y+1) que solo se pintan al
// empezar el campo (draw_field_static). Casi todas las cajas de borrado
// (soldado aterrizado, torreta, torre en marcha, mensajes...) la cruzan y, al
// no volver a pintarla nadie, iban dejando huecos que no se rellenaban nunca:
// con la torre activa, por ejemplo, desaparecía media línea cada frame.
static void repaint_ground(int x, int w) {
    if (x < PLAY_X) { w -= PLAY_X - x; x = PLAY_X; }
    if (x + w > PLAY_X + PLAY_W) w = PLAY_X + PLAY_W - x;
    if (w <= 0) return;
    renderer_fill_rect(x, GROUND_Y, w, 2, COLOR_WHITE);
}

static void erase_box(int x, int y, int w, int h) {
    if (x<0) { w+=x; x=0; }
    if (y<0) { h+=y; y=0; }
    if (w<=0 || h<=0) return;
    renderer_fill_rect(x, y, w, h, COLOR_BLACK);
    // Si la caja cruza la línea del suelo, se repone el tramo borrado. Es
    // dentro de la misma zona sucia, así que no añade coste de SPI.
    if (y < GROUND_Y + 2 && y + h > GROUND_Y) repaint_ground(x, w);
    mark_paras_dirty(x, y, w, h);
}

// Repintado periódico de la línea completa (red de seguridad por si algún
// otro dibujo la pisa). Una vez cada GROUND_REFRESH_MS: es una franja de
// 320x2 px, coste despreciable.
#define GROUND_REFRESH_MS 1500
static int32_t ground_refresh_ms;

// Cuándo se llama a renderer_flush() (cada flush = una transacción SPI con
// su espera, y el driver solo guarda 8 rectángulos sucios: si se acumulan
// más, los fusiona TODOS en uno gigante):
//   0 = uno por entidad (como la Pico): ~27 flushes por frame con 12
//       soldados en el suelo, de ahí la ralentización.
//   1 = uno solo al final del frame: pocos flushes, pero se desbordan los 8
//       rectángulos y acaba transmitiendo media pantalla cada frame (peor).
//   2 = por zonas (suelo / cielo / proyectiles): probado, peor que el 0: el
//       driver fusiona rectángulos lejanos si "no desperdician" más de 3x
//       su área (dos helicópteros en carriles distintos, la torreta y los
//       soldados de ambos lados...) y transmite rectángulos enormes.
// Se usa el 0, ahora barato porque los soldados parados y la torreta ya no
// se redibujan cada frame (ver para_dirty / turret_dirty).
#ifndef PT_FLUSH_MODE
#define PT_FLUSH_MODE 0
#endif
#if PT_FLUSH_MODE == 0
#define pt_flush() renderer_flush()
#else
#define pt_flush() ((void)0)
#endif
#if PT_FLUSH_MODE == 2
#define pt_flush_group() renderer_flush()
#else
#define pt_flush_group() ((void)0)
#endif

static void reset_render_trace(void) {
    for (int i=0;i<MAX_HELIS;i++) prev_heli[i].active=false;
    for (int i=0;i<MAX_PARAS;i++) { prev_para[i].active=false; prev_para_landed[i]=false; para_dirty[i]=false; }
    for (int i=0;i<MAX_BOMBS;i++) prev_bomb[i].active=false;
    for (int i=0;i<MAX_PLANES;i++) prev_plane_pos[i].active=false;
    prev_turret.active=false;
    turret_dirty=false; prev_turret_angle=-1000; prev_turret_vis=false;
    prev_tower_region_active=false;
    prev_score=prev_wave=-1;
    prev_center_msg[0]='\0';
    prev_bottom_msg[0]='\0';
}

static void draw_field_static(void) {
    renderer_clear(COLOR_BLACK);
    renderer_fill_rect(PLAY_X, GROUND_Y, PLAY_W, 2, COLOR_WHITE);
    reset_render_trace();
    field_needs_redraw = false;
    renderer_flush();
}

static bool turret_visible(void) {
    return cannon_alive && !(cannon_inv>0 && (cannon_inv/4)%2==0);
}

static void draw_turret_if_changed(void) {
    bool vis = turret_visible();
    bool show = cannon_alive;
    if (!show && !prev_turret.active) return;
    // Solo se borra y redibuja entera cuando cambia algo propio (ángulo,
    // parpadeo de invulnerabilidad, muerte) o mientras hay torre activa, que
    // ya la repinta a medias cada frame. El cañón son ~150 fill_rect de línea:
    // repintarlo en cada frame aunque no se mueva era lo más caro del juego.
    if (vis != prev_turret_vis || cannon_angle_deg != prev_turret_angle ||
        show != prev_turret.active || tower_active) {
        erase_box(TURRET_X-TURRET_BOX_HW, TURRET_BOX_TOP, 2*TURRET_BOX_HW, TURRET_BOX_H);
        if (show) draw_turret();
        turret_dirty = false;
        prev_turret_vis = vis; prev_turret_angle = cannon_angle_deg;
        prev_turret.active = show;
        pt_flush();
    }
}

// Repone la torreta si otro borrado le ha comido parte (sin borrar antes).
static void draw_turret_if_dirty(void) {
    if (!turret_dirty) return;
    turret_dirty = false;
    if (tower_active || !turret_visible() || !prev_turret.active) return;
    draw_turret();
}

static void draw_helis_if_moved(void) {
    for (int i=0;i<MAX_HELIS;i++) {
        const Heli *h = &helis[i];
        bool show = h->active;
        if (!show && !prev_heli[i].active) continue;
        int cx=FP2PX(h->x), cy=h->y;
        if (prev_heli[i].active)
            erase_box(prev_heli[i].x-HELI_BOX_HW, prev_heli[i].y-HELI_BOX_UP,
                      2*HELI_BOX_HW, HELI_BOX_UP+HELI_BOX_DN);
        if (show) draw_heli(h);
        prev_heli[i].x=cx; prev_heli[i].y=cy; prev_heli[i].active=show;
        pt_flush();
    }
}

/* ¿Este paracaidista está aterrizado y en el lado que está marchando
 * ahora mismo hacia la torreta? Esos los gestiona draw_tower_region()
 * en un único borrado+redibujado atómico (ver más abajo); el resto
 * sigue el patrón normal de un flush por entidad. */
static bool para_in_active_tower(const Para *p) {
    return tower_active && p->active && p->landed && p->side==tower_side;
}

static void draw_paras_if_moved(void) {
    for (int i=0;i<MAX_PARAS;i++) {
        const Para *p = &paras[i];
        bool show = p->active;
        if (!show && !prev_para[i].active) continue;
        int cx=FP2PX(p->x), cy=FP2PX(p->y);

        if (para_in_active_tower(p)) {
            /* No se toca la pantalla aquí -- solo se mantiene al día su
             * posición "previa" para que, cuando deje de estar en la
             * torre (aterriza otro/se destruye la torre/etc.), el borrado
             * individual de más abajo parta de la posición real y no de
             * una desfasada de antes de empezar a marchar. */
            prev_para[i].x=cx; prev_para[i].y=cy; prev_para[i].active=show;
            prev_para_landed[i] = true;
            continue;
        }

        // Soldado parado en su ranura, sin cambios: no se borra ni se redibuja
        // (ver para_dirty arriba); si algo le mordió el sprite lo repone
        // draw_static_paras_if_dirty() al final del frame.
        if (show && p->landed && prev_para[i].active && prev_para_landed[i] &&
            prev_para[i].x==cx && prev_para[i].y==cy) continue;

        if (prev_para[i].active) {
            /* La caja de borrado depende de cómo se dibujó la ÚLTIMA vez
             * (aterrizado o cayendo), no de cómo esté ahora: si acaba de
             * aterrizar en este mismo tick hay que borrar todavía la
             * silueta grande del paracaídas, no la pequeña del soldado. */
            int up = prev_para_landed[i] ? PARA_LAND_BOX_UP : PARA_BOX_UP;
            int dn = prev_para_landed[i] ? PARA_LAND_BOX_DN : PARA_BOX_DN;
            erase_box(prev_para[i].x-PARA_BOX_HW, prev_para[i].y-up,
                      2*PARA_BOX_HW, up+dn);
        }
        if (show) draw_para(p, COLOR_SOLDIER);
        prev_para[i].x=cx; prev_para[i].y=cy; prev_para[i].active=show;
        prev_para_landed[i] = p->landed;
        para_dirty[i] = false;
        pt_flush();
    }
}

// Repone los soldados parados a los que otro borrado (bala, partícula, caja
// de la torreta, un compañero que cae...) les ha comido parte del sprite. Va
// al FINAL del frame, cuando ya no queda ningún borrado por hacer. No borra
// antes de pintar: el sprite es siempre el mismo y se pinta encima.
static void draw_static_paras_if_dirty(void) {
    for (int i=0;i<MAX_PARAS;i++) {
        const Para *p = &paras[i];
        if (!para_dirty[i]) continue;
        para_dirty[i] = false;
        if (!p->active || !p->landed || para_in_active_tower(p)) continue;
        if (!prev_para[i].active || !prev_para_landed[i]) continue;
        draw_para(p, COLOR_SOLDIER);
    }
}

/*
 * Torre/escalera: mientras tower_active, TODOS los soldados aterrizados
 * del lado que ataca (los que marchan/escalan Y los que aún esperan su
 * turno en su ranura original) se borran y redibujan de una sola vez
 * sobre la franja completa de ese lado, en un único flush -- en vez de
 * objeto por objeto. Están a menudo a menos de 15-20px unos de otros
 * (justo su propia altura), así que con cajas individuales el borrado
 * de uno se comía los píxeles recién dibujados de otro en el mismo
 * frame. Al ser un único borrado+redibujado atómico, eso ya no puede
 * pasar.
 *
 * OJO -- esta franja llega en horizontal justo hasta TURRET_X (x0/x1),
 * pero el sprite de la torreta (ver TURRET_BOX_HW) sobresale ~31px más
 * allá de TURRET_X hacia ESTE mismo lado -- es decir, la mitad del
 * dibujo de la torreta cae dentro de esta franja. El soldado que
 * escala (target_x llega a base_edge = TURRET_X∓12, y hasta CANNON_OY
 * en el peldaño más alto) queda literalmente pegado a la base. El
 * erase_box de aquí arriba borra esa mitad de la torreta, así que hay
 * que volver a dibujarla aquí mismo, en el mismo flush -- si se deja
 * para draw_turret_if_changed() por separado, lo que se borre/dibuje
 * ahí "gana" sobre lo que se acaba de pintar aquí (o al revés, según
 * el orden de llamada) y uno de los dos parpadea cada tick.
 */
static void draw_tower_region(void) {
    if (!tower_active) { prev_tower_region_active = false; return; }

    int side = tower_side;
    int x0 = (side==0) ? PLAY_X : TURRET_X;
    int x1 = (side==0) ? TURRET_X : (PLAY_X+PLAY_W);
    int top = CANNON_OY - SOLDIER_H - 4;   // cubre al soldado que sube hasta el cañón
    int bottom = GROUND_Y + 1;

    erase_box(x0, top, x1-x0, bottom-top);
    if (cannon_alive) draw_turret();   // repone la mitad de la torreta que cae en esta franja
    for (int i=0;i<MAX_PARAS;i++) {
        const Para *p = &paras[i];
        if (!para_in_active_tower(p)) continue;
        draw_para(p, COLOR_SOLDIER);
    }
    prev_tower_region_active = true;
    pt_flush();
}

// La bomba nace pegada al vientre del avión y su caja de borrado le comía
// parte del ala/cola durante los primeros frames. Tras pintar las bombas se
// repinta (sin borrar) el sprite de los aviones que tengan una bomba encima.
static void redraw_planes_over_bombs(void) {
    for (int i=0;i<MAX_PLANES;i++) {
        const Plane *pl = &planes[i];
        if (!pl->active) continue;
        int x=FP2PX(pl->x), y=FP2PX(pl->y);
        for (int j=0;j<MAX_BOMBS;j++) {
            if (!bombs[j].active) continue;
            if (absi(FP2PX(bombs[j].x)-x) < PLANE_BOX_HW+BOMB_BOX_R &&
                absi(FP2PX(bombs[j].y)-y) < PLANE_BOX_HH+BOMB_BOX_R) {
                draw_plane_sprite(pl, COLOR_JET);
                break;
            }
        }
    }
}

static void draw_bombs_if_moved(void) {
    for (int i=0;i<MAX_BOMBS;i++) {
        const Bomb *b = &bombs[i];
        bool show = b->active;
        if (!show && !prev_bomb[i].active) continue;
        int bx=FP2PX(b->x), by=FP2PX(b->y);
        if (prev_bomb[i].active)
            erase_box(prev_bomb[i].x-BOMB_BOX_R, prev_bomb[i].y-BOMB_BOX_R, 2*BOMB_BOX_R, 2*BOMB_BOX_R);
        if (show) draw_bomb_sprite(b, COLOR_BOMB);
        prev_bomb[i].x=bx; prev_bomb[i].y=by; prev_bomb[i].active=show;
        pt_flush();
    }
}

static void draw_plane_if_moved(void) {
    for (int i=0;i<MAX_PLANES;i++) {
        const Plane *pl = &planes[i];
        bool show = pl->active;
        if (!show && !prev_plane_pos[i].active) continue;
        int x=FP2PX(pl->x), y=FP2PX(pl->y);
        if (prev_plane_pos[i].active)
            erase_box(prev_plane_pos[i].x-PLANE_BOX_HW, prev_plane_pos[i].y-PLANE_BOX_HH,
                      2*PLANE_BOX_HW, 2*PLANE_BOX_HH);
        if (show) draw_plane_sprite(pl, COLOR_JET);
        prev_plane_pos[i].x=x; prev_plane_pos[i].y=y; prev_plane_pos[i].active=show;
        pt_flush();
    }
}

static int prev_bullet_x[MAX_BULLETS], prev_bullet_y[MAX_BULLETS];
static bool prev_bullet_active[MAX_BULLETS];
static int prev_part_x[MAX_PARTICLES], prev_part_y[MAX_PARTICLES];
static bool prev_part_active[MAX_PARTICLES];

static void draw_bullets_if_moved(void) {
    bool any=false;
    for (int i=0;i<MAX_BULLETS;i++) {
        int x=FP2PX(bullets[i].x)-1, y=FP2PX(bullets[i].y)-1;
        bool show = bullets[i].active;
        if (!show && !prev_bullet_active[i]) continue;
        if (prev_bullet_active[i]) erase_box(prev_bullet_x[i],prev_bullet_y[i],3,3);
        if (show) renderer_fill_rect(x,y,3,3,COLOR_BULLET);
        prev_bullet_x[i]=x; prev_bullet_y[i]=y; prev_bullet_active[i]=show;
        any=true;
    }
    if (any) pt_flush();
}

static void draw_particles_if_moved(void) {
    bool any=false;
    for (int i=0;i<MAX_PARTICLES;i++) {
        int x=FP2PX(particles[i].x), y=FP2PX(particles[i].y);
        bool show = particles[i].active &&
                    x>=PLAY_X && x<PLAY_X+PLAY_W && y>=PLAY_Y && y<PLAY_Y+PLAY_H;
        if (!show && !prev_part_active[i]) continue;
        if (prev_part_active[i]) erase_box(prev_part_x[i],prev_part_y[i],2,2);
        if (show) renderer_fill_rect(x,y,2,2,COLOR_PART);
        prev_part_x[i]=x; prev_part_y[i]=y; prev_part_active[i]=show;
        any=true;
    }
    if (any) pt_flush();
}

// ---------------------------------------------------------------------------
// HUD
// ---------------------------------------------------------------------------
static void draw_hud_if_changed(void) {
    char buf[16];
    bool changed=false;

    if (score != prev_score) {
        renderer_fill_rect(PLAY_X+2, PLAY_Y+2, 90, 12, COLOR_BLACK);
        snprintf(buf, sizeof(buf), "%06d", score);
        st7789_draw_text(PLAY_X+2, PLAY_Y+2, buf, COLOR_WHITE, COLOR_BLACK, 2);
        prev_score = score; changed = true;
    }
    if (wave != prev_wave) {
        renderer_fill_rect(PLAY_X+PLAY_W-60, PLAY_Y+2, 58, 12, COLOR_BLACK);
        snprintf(buf, sizeof(buf), "OLA %d", wave);
        int x = PLAY_X+PLAY_W-2-(int)st7789_text_width(buf,1);
        st7789_draw_text(x, PLAY_Y+2, buf, COLOR_WHITE, COLOR_BLACK, 1);
        prev_wave = wave; changed = true;
    }
    if (changed) pt_flush();
}

static int centered_x(const char *text, int scale) {
    int w = (int)st7789_text_width(text, (uint8_t)scale);
    int x = (TFT_WIDTH - w) / 2;
    return (x<0) ? 0 : x;
}

static void update_center_message(const char *target, uint16_t color, int scale) {
    if (strcmp(target, prev_center_msg)==0) return;
    renderer_fill_rect(0, CY-16, TFT_WIDTH, 32, COLOR_BLACK);
    if (target[0]) st7789_draw_text(centered_x(target,scale), CY-10, target, color, COLOR_BLACK, scale);
    strncpy(prev_center_msg, target, sizeof(prev_center_msg)-1);
    prev_center_msg[sizeof(prev_center_msg)-1]='\0';
    renderer_flush();
}

static void update_bottom_message(const char *target, int scale) {
    if (strcmp(target, prev_bottom_msg)==0) return;
    /*
     * Caja de borrado/dibujo dimensionada según "scale" -- antes era
     * un hueco fijo de 16px pensado solo para escala 1; con textos a
     * escala 2 (el doble de alto) se salían por abajo o quedaban mal
     * encajados. Con esto cabe cualquier escala razonable y siempre
     * queda a la misma distancia (2px) del borde inferior del área
     * de juego.
     */
    // El mensaje va en la franja que queda BAJO la línea del suelo
    // (GROUND_Y+2 hasta el borde de pantalla, ~14 px). Antes la caja se
    // calculaba desde PLAY_Y+PLAY_H-2 hacia arriba: con escala 1 empezaba en
    // GROUND_Y-1 y con escala 2 mucho antes, así que borraba la línea del
    // suelo (y la base de la torreta) cada vez que cambiaba el mensaje, y el
    // texto quedaba escrito encima del suelo. Si no cabe, baja a escala 1.
    int box_y = GROUND_Y + 2;
    int avail = TFT_HEIGHT - box_y;
    while (scale > 1 && 8*scale + 4 > avail) scale--;
    int text_h = 8*scale;
    int box_h = text_h + 4;
    if (box_h > avail) box_h = avail;

    renderer_fill_rect(0, box_y, TFT_WIDTH, box_h, COLOR_BLACK);
    if (target[0]) st7789_draw_text(centered_x(target,scale), box_y+(box_h-text_h)/2, target, COLOR_WHITE, COLOR_BLACK, scale);
    strncpy(prev_bottom_msg, target, sizeof(prev_bottom_msg)-1);
    prev_bottom_msg[sizeof(prev_bottom_msg)-1]='\0';
    renderer_flush();
}

static void draw_playing_frame(void) {
    if (field_needs_redraw) draw_field_static();

    /*
     * draw_turret_if_changed() va ANTES que draw_tower_region(): la caja
     * de borrado de la torreta invade la franja de marcha/escalada (ver
     * comentario en draw_tower_region), así que quien se dibuje último
     * sobre ese solape es el que se ve bien. Poniendo la torreta primero,
     * draw_tower_region() (que ya redibuja la torreta por su cuenta en
     * esa zona) tiene siempre la última palabra sobre los soldados.
     */
    draw_turret_if_changed();
    draw_paras_if_moved();
    draw_tower_region();
    pt_flush_group();            // suelo: torreta + soldados + torre

    draw_helis_if_moved();
    draw_plane_if_moved();
    pt_flush_group();            // cielo: helicópteros + avión

    draw_bombs_if_moved();
    redraw_planes_over_bombs();
    draw_bullets_if_moved();
    draw_particles_if_moved();
    draw_hud_if_changed();
    draw_static_paras_if_dirty();
    draw_turret_if_dirty();
    ground_refresh_ms -= g_elapsed_ms;
    if (ground_refresh_ms <= 0) {
        ground_refresh_ms = GROUND_REFRESH_MS;
        repaint_ground(PLAY_X, PLAY_W);
    }
    pt_flush_group();            // proyectiles + HUD + soldados reparados

    bool bon = (blink/20)%2==0;

    const char *center = "";
    int center_scale = 2;
    if (state==PT_DEAD && bon) { center="TORRETA DESTRUIDA"; }
    update_center_message(center, COLOR_YELLOW, center_scale);

    const char *bottom = "";
    if (demo_mode && bon) bottom = "DEMO - PULSA PARA JUGAR";
    update_bottom_message(bottom, 2);

    renderer_flush();   // lo que quede (mensajes); ver PT_FLUSH_MODE
}

// ---------------------------------------------------------------------------
// Pantallas estáticas
// ---------------------------------------------------------------------------
/*
 * Torreta y paracaidista decorativos para la pantalla de título --
 * mismas primitivas que draw_turret()/draw_para() (fill_top_ellipse,
 * draw_chute_filled, draw_soldier_at, line con can_dx/can_dy), pero
 * en tamaño propio y sin depender del estado de partida
 * (cannon_alive/cannon_angle_deg/Para), para poder colocarlos donde
 * convenga en una pantalla estática. El dibujo anterior (un par de
 * rectángulos sueltos) no llegaba a parecerse a una torreta ni a un
 * paracaidista -- esto reutiliza la forma real del juego, solo que
 * un poco más grande para que se lea bien como decoración.
 */
#define TITLE_TURRET_BASE_W   26
#define TITLE_TURRET_BASE_H   20
#define TITLE_TURRET_RX       9
#define TITLE_TURRET_RY       14
#define TITLE_TURRET_GUN_LEN  18
#define TITLE_TURRET_GUN_W    2
#define TITLE_TURRET_GUN_ANGLE (-18)   // grados, misma convención que cannon_angle_deg

static void draw_title_turret(int tx, int ground_y) {
    int base_top = ground_y - TITLE_TURRET_BASE_H;

    renderer_fill_rect(
        tx - TITLE_TURRET_BASE_W/2, base_top,
        TITLE_TURRET_BASE_W, TITLE_TURRET_BASE_H,
        COLOR_TURRET_BASE
    );
    fill_top_ellipse(tx, base_top, TITLE_TURRET_RX, TITLE_TURRET_RY, COLOR_TURRET_BASE);
    renderer_fill_rect(tx-1, base_top-TITLE_TURRET_RY+2, 2, 2, COLOR_BLACK);

    int ox = tx;
    int oy = base_top - TITLE_TURRET_RY/2;
    int dx = can_dx(TITLE_TURRET_GUN_ANGLE);
    int dy = can_dy(TITLE_TURRET_GUN_ANGLE);
    int tip_x = ox + dx*TITLE_TURRET_GUN_LEN/256;
    int tip_y = oy + dy*TITLE_TURRET_GUN_LEN/256;

    for (int t=-TITLE_TURRET_GUN_W; t<=TITLE_TURRET_GUN_W; t++) {
        int ex = (int)(t*(-dy)/256);
        int ey = (int)(t*( dx)/256);
        line(ox+ex, oy+ey, tip_x+ex, tip_y+ey, COLOR_TURRET_GUN);
    }
}

#define TITLE_PARA_RW 10
#define TITLE_PARA_RH 8

static void draw_title_para(int px, int hang_y) {
    int db = hang_y - TITLE_PARA_RH;
    draw_chute_filled(px, db, TITLE_PARA_RW, TITLE_PARA_RH, COLOR_CHUTE);
    line(px-TITLE_PARA_RW, db, px, hang_y, COLOR_CHUTE);
    line(px+TITLE_PARA_RW, db, px, hang_y, COLOR_CHUTE);
    line(px-2, hang_y, px+2, hang_y, COLOR_CHUTE);
    draw_soldier_at(px, hang_y+SOLDIER_H, false, COLOR_SOLDIER);
}

static void draw_title_screen(void) {
    renderer_clear(COLOR_BLACK);

    // --- Título, centrado de verdad (centered_x mide el ancho real
    // de la fuente en vez de asumirlo a mano) ---
    static const char *title = "PARATROOPER";
    st7789_draw_text(centered_x(title, 3), 10, title, COLOR_WHITE, COLOR_BLACK, 3);

    renderer_fill_rect(30, 38, TFT_WIDTH-60, 2, COLOR_TURRET_GUN);

    // --- Escena decorativa: dos paracaidistas bajando hacia la
    // torreta central, con las mismas formas que se ven en partida ---
    draw_title_para(66, 58);
    draw_title_turret(CX, 108);
    draw_title_para(254, 58);

    // --- Instrucciones -- centradas, con hueco de sobra antes del
    // "PULSA A/B PARA JUGAR" parpadeante que pinta pt_tick() más abajo ---
    static const char *line1 = "GIRA Y DISPARA";
    static const char *line2 = "DEFIENDE LA TORRETA";

    st7789_draw_text(centered_x(line1, 2), 155, line1, COLOR_YELLOW, COLOR_BLACK, 2);
    st7789_draw_text(centered_x(line2, 2), 183, line2, COLOR_CYAN, COLOR_BLACK, 2);

    // Específico de la ESP32: cómo se sale (click del stick).
    static const char *line3 = "CLICK STICK: SALIR";
    st7789_draw_text(centered_x(line3, 1), 207, line3, COLOR_WHITE, COLOR_BLACK, 1);

    prev_center_msg[0]='\0';
    prev_bottom_msg[0]='\0';
    renderer_flush();
}

static void draw_gameover_screen(void) {
    renderer_clear(COLOR_BLACK);
    st7789_draw_text(centered_x("GAME OVER",3), CY-40, "GAME OVER", COLOR_RED, COLOR_BLACK, 3);
    char b[24]; snprintf(b,sizeof(b),"PUNTOS: %d", score);
    st7789_draw_text(centered_x(b,2), CY+4, b, COLOR_WHITE, COLOR_BLACK, 2);
    prev_center_msg[0]='\0';
    prev_bottom_msg[0]='\0';
    renderer_flush();
}

static void draw_scores_screen(void) {
    renderer_clear(COLOR_BLACK);
    highscores_draw(PT_GAME_ID, "PARATROOPER", 20);
    prev_center_msg[0]='\0';
    prev_bottom_msg[0]='\0';
    renderer_flush();
}

// ---------------------------------------------------------------------------
// IA demo -- idéntica al original.
// ---------------------------------------------------------------------------
static int demo_tgt = ANGLE_UP_DEG, demo_fcd = 0;

static int aim_at(int tx, int ty) {
    int ddx=tx-CANNON_OX, ddy=CANNON_OY-ty;
    if (ddy<=0) return ANGLE_UP_DEG;
    int a = ddx*75/(absi(ddx)+ddy/2+1);
    if (a<ANGLE_MIN_DEG) a=ANGLE_MIN_DEG;
    if (a>ANGLE_MAX_DEG) a=ANGLE_MAX_DEG;
    return a;
}

static void demo_ai(void) {
    int best_ang=ANGLE_UP_DEG, best_d=0x7fffffff;
    for (int i=0;i<MAX_HELIS;i++) {
        if (!helis[i].active) continue;
        int hx=FP2PX(helis[i].x), hy=helis[i].y;
        int dx=hx-CANNON_OX, dy=hy-CANNON_OY;
        int d2=dx*dx+dy*dy;
        if (d2<best_d) { best_d=d2; best_ang=aim_at(hx,hy); }
    }
    for (int i=0;i<MAX_PARAS;i++) {
        Para *p=&paras[i]; if (!p->active||p->landed) continue;
        int px=FP2PX(p->x), py=FP2PX(p->y);
        int dx=px-CANNON_OX, dy=py-CANNON_OY;
        int d2=dx*dx+dy*dy;
        if (d2<best_d) { best_d=d2; best_ang=aim_at(px,py); }
    }
    for (int i=0;i<MAX_BOMBS;i++) {
        if (!bombs[i].active) continue;
        int bx=FP2PX(bombs[i].x), by=FP2PX(bombs[i].y);
        int dx=bx-CANNON_OX, dy=by-CANNON_OY;
        int d2=dx*dx+dy*dy;
        if (d2<best_d) { best_d=d2; best_ang=aim_at(bx,by); }
    }
    demo_tgt = best_ang;
    if (cannon_angle_deg<demo_tgt) cannon_angle_deg++;
    else if (cannon_angle_deg>demo_tgt) cannon_angle_deg--;
    if (--demo_fcd<=0 && absi(cannon_angle_deg-demo_tgt)<=2 && best_d<140*140) {
        cannon_rot_dir=0; player_fire(); demo_fcd=8;
    }
}

// ---------------------------------------------------------------------------
// Tick principal
// ---------------------------------------------------------------------------
/*
 * Vacía la entrada pendiente. highscores_enter() es bloqueante y se maneja
 * con los mismos botones/click que el juego: lo que quedaba SIN CONSUMIR
 * (un "pulsado" en cola, el click del stick con el que se confirman las
 * iniciales...) lo leía pt_tick() justo después y saltaba GAME OVER ->
 * RÉCORDS -> salir al título sin dejar ver nada. Solo se usan los índices
 * de entrada que ya usa este juego.
 */
static void flush_inputs(void) {
    for (int k=0; k<2; k++) {
        controls_update();
        (void)controls_button_pressed(BTN_IDX_J1_A);
        (void)controls_button_pressed(BTN_IDX_J1_B);
        (void)controls_button_pressed(BTN_IDX_J1_SW);
        (void)controls_button_pressed(BTN_IDX_J2_SW);
        (void)controls_get_raw_delta(0);
        (void)controls_get_raw_delta_x(0);
    }
    cannon_enc_acc = 0; cannon_rot_dir = 0; cannon_rot_acc = 0;
}

// Pantalla de fin: vaciar entrada y no aceptar botones durante este tiempo.
#define END_SCREEN_LOCK_MS 1200

static void pt_tick(void) {
    blink++;
    update_dt_scale();

    if (demo_mode) {
        bool any = controls_menu_select()
                || controls_get_raw_delta_x(0) != 0
                || controls_button_down(BTN_IDX_J1_B);
        demo_ticks += g_elapsed_ms;
        if (any || demo_ticks >= 30000) {
            g_done = true;
            return;
        }
    } else {
        // Click de cualquiera de los dos joysticks (índice 4/5, ver
        // BTN_IDX_J1_SW/J2_SW más arriba) sale al menú en cualquier
        // momento -- equivalente a "SW1: salir" del original.
        // (se lee siempre, aunque la entrada esté bloqueada, para consumirlo)
        bool sw_click = controls_button_pressed(BTN_IDX_J1_SW) || controls_button_pressed(BTN_IDX_J2_SW);
        if (sw_click && input_lock_ms <= 0) {
            g_done = true;
            return;
        }
    }

    bool btn = controls_button_pressed(BTN_IDX_J1_A) || controls_button_pressed(BTN_IDX_J1_B);
    if (input_lock_ms > 0) { input_lock_ms -= g_elapsed_ms; btn = false; }

    switch (state) {

    case PT_TITLE:
        if (btn) game_start();
        {
            bool bon = (blink/20)%2==0;
            update_bottom_message(bon ? "PULSA A/B PARA JUGAR" : "", 1);
        }
        break;

    case PT_PLAYING:
    case PT_JET_PASS: {
        if (!demo_mode) {
            int enc = controls_get_raw_delta_x(0);
            cannon_enc_acc += enc;
            if (cannon_enc_acc>=4)       { cannon_enc_acc=0; cannon_rot_dir= 1; }
            else if (cannon_enc_acc<=-4) { cannon_enc_acc=0; cannon_rot_dir=-1; }
            if (cannon_rot_dir!=0) {
                // Grados por tiempo real: si un frame tarda el doble, gira el doble
                // en ese frame (antes giraba siempre CANNON_ROT_SPD por frame y con
                // pocos fps el cañón se arrastraba y "no respondía").
                cannon_rot_acc += cannon_rot_dir * CANNON_ROT_SPD * g_dt_scale;
                int whole = cannon_rot_acc / FP;
                cannon_rot_acc -= whole * FP;
                cannon_angle_deg += whole;
                if (cannon_angle_deg<=ANGLE_MIN_DEG) { cannon_angle_deg=ANGLE_MIN_DEG; cannon_rot_dir=0; cannon_rot_acc=0; }
                if (cannon_angle_deg>=ANGLE_MAX_DEG) { cannon_angle_deg=ANGLE_MAX_DEG; cannon_rot_dir=0; cannon_rot_acc=0; }
            }
            if (btn) { cannon_rot_dir=0; cannon_rot_acc=0; cannon_enc_acc=0; if (!fire_held) player_fire(); fire_held=true; }
            else fire_held=false;
        } else {
            demo_ai();
        }

        if (cannon_inv>0) cannon_inv--;

        if (state==PT_PLAYING && !tower_active) {
            for (int lane=0;lane<2;lane++) {
                lane_timer[lane] -= g_elapsed_ms;
                if (lane_timer[lane] <= 0) {
                    lane_timer[lane] = HELI_GAP_MS + rnd(1000);
                    if (wave_helis_spawned < wave_heli_budget) {
                        spawn_heli(lane);
                        wave_helis_spawned++;
                    }
                }
            }
        }

        update_helis();
        update_paras();
        update_tower();
        update_jet_schedule();
        update_planes();
        update_bombs();
        update_bullets();
        upd_particles();
        check_collisions();

        if (state==PT_PLAYING && cannon_alive) {
            if (attack_mode) {
                attack_timer -= g_elapsed_ms;
                if (attack_timer<=0) {
                    attack_mode=false;
                    attack_cooldown=ATTACK_INTERVAL_MS+rnd(10000);
                }
            } else {
                attack_cooldown -= g_elapsed_ms;
                if (attack_cooldown<=0) {
                    attack_mode=true;
                    attack_timer=ATTACK_DUR_MS;
                }
            }
        }

        if (!cannon_alive) {
            for (int i=0;i<MAX_HELIS;i++) helis[i].active=false;
            state=PT_DEAD; pause_cnt=3000;
            sound_stop_paratrooper_music(); // por si el jingle de inicio seguía sonando
            draw_playing_frame();
            break;
        }

        if (state==PT_PLAYING) {
            if (!wave_done && !tower_active && wave_clear()) {
                wave_done = true;
                jet_delay = 2000;   // ms
            }
            if (wave_done && !jet_launched) {
                jet_delay -= g_elapsed_ms;
                if (jet_delay <= 0) {
                    jets_left    = 1 + rnd(MAX_PLANES);      // 1..MAX_PLANES aviones
                    jet_next_dir = rnd(2) ? 1 : -1;
                    jet_gap_ms   = 0;
                    jet_launched = true;
                    state = PT_JET_PASS;
                }
            }
        }
        if (state==PT_JET_PASS) {
            if (jets_left <= 0 && planes_active() == 0 && !bombs_active()) {
                wave++; score += PTS_WAVE_BONUS;
                reset_wave();
                for (int i=0;i<MAX_PARAS;i++) {
                    Para *p = &paras[i];
                    if (!p->active || !p->landed) continue;
                    slot_count[p->side][p->slot]++;
                    ground_total[p->side]++;
                    p->marching=false; p->climbing=false;
                    p->march_order=-1;
                    p->target_x=p->target_y=0;
                }
                state = PT_PLAYING;
                sound_effect_success();
            }
        }

        draw_playing_frame();
        break;
    }

    case PT_DEAD:
        upd_particles();
        pause_cnt -= g_elapsed_ms;
        if (pause_cnt <= 0) {
            sound_siren_stop();
            sound_effect_game_over();
            draw_playing_frame();
            if (!demo_mode && highscores_is_top(PT_GAME_ID, (uint32_t)score)) {
                highscores_enter(PT_GAME_ID, (uint32_t)score); // bloqueante
            }
            flush_inputs();
            input_lock_ms = END_SCREEN_LOCK_MS;
            last_tick_time_ms = now_ms();   // que el tiempo en highscores_enter() no cuente como un tick
            pause_cnt = 0;
            state = PT_GAMEOVER;
            draw_gameover_screen();
        } else {
            draw_playing_frame();
        }
        break;

    case PT_GAMEOVER: {
        bool bon = (blink/20)%2==0;
        update_bottom_message(bon ? "PULSA PARA CONTINUAR" : "", 1);
        pause_cnt += g_elapsed_ms;
        if (demo_mode) {
            if (pause_cnt > 2000) g_done = true;
        } else if (btn || pause_cnt > 8000) {
            flush_inputs();
            input_lock_ms = END_SCREEN_LOCK_MS;
            pause_cnt = 0;
            state = PT_SCORES;
            draw_scores_screen();
        }
        break;
    }

    case PT_SCORES:
        pause_cnt += g_elapsed_ms;
        if (btn || pause_cnt > 8000) g_done = true;
        break;
    }
}

// ---------------------------------------------------------------------------
// API pública -- firma game_run_fn (game_common.h): el menú solo conoce
// void paratrooper_run(game_mode_t mode), sin valor de retorno y sin
// función de demo separada (demo = mode==GAME_MODE_DEMO), exactamente
// igual que game_asteroids_run() en asteroids.c. Paratrooper no tiene
// concepto de 2 jugadores -- GAME_MODE_2P se trata igual que 1P.
// ---------------------------------------------------------------------------
void game_paratrooper_run(game_mode_t mode) {
    srand((unsigned)esp_timer_get_time());

    bool is_demo = (mode == GAME_MODE_DEMO);

    demo_mode = is_demo;
    demo_ticks = 0;
    blink = 0;
    pause_cnt = 0;
    input_lock_ms = 0;
    fire_held = false;
    g_done = false;
    cannon_enc_acc = 0; cannon_rot_dir = 0;
    demo_fcd = 0; demo_tgt = ANGLE_UP_DEG;
    wave = 1;
    field_needs_redraw = true;

    memset(helis, 0, sizeof(helis));
    memset(paras, 0, sizeof(paras));
    memset(bullets, 0, sizeof(bullets));
    memset(bombs, 0, sizeof(bombs));
    memset(planes, 0, sizeof(planes));
    memset(particles, 0, sizeof(particles));

    last_tick_time_ms = now_ms();
    reset_render_trace();

    if (is_demo) {
        /*
         * Vaciamos el acumulador de controls_get_raw_delta_x(0) antes
         * de arrancar -- es la señal que usa pt_tick() para saber si
         * un jugador de verdad ha tocado el encoder y así cortar la
         * demo. Sin este vaciado, cualquier ruido/rebote mecánico
         * acumulado desde la ÚLTIMA vez que alguien leyó ese
         * acumulador -- que puede ser rato antes, mientras el menú
         * estaba en el marcador o en otra pantalla que no lo consume
         * -- se lee como "el jugador ha tocado el mando" en el
         * primerísimo tick, y la demo se corta antes de llegar a
         * dibujar un solo fotograma.
         */
        controls_get_raw_delta_x(0);
        game_start();
    } else {
        state = PT_TITLE;
        draw_title_screen();
    }

    // Bucle a ritmo fijo con vTaskDelayUntil() -- ver nota de cabecera
    // del archivo y el mismo patrón en el resto de juegos ya portados.
    const TickType_t period_ticks = pdMS_TO_TICKS(1000 / TARGET_FPS);
    TickType_t last_wake = xTaskGetTickCount();

    while (!g_done) {
        controls_update();
        pt_tick();
        sound_update();
        vTaskDelayUntil(&last_wake, period_ticks ? period_ticks : 1);
    }

    sound_siren_stop();
    sound_stop_paratrooper_music(); // por si se sale a mitad del jingle de inicio
    flush_inputs();                 // que el menú no reciba lo que quedó sin leer aquí
    highscores_flush();
}