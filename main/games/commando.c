/*
 * MISSING IN ARCADE - fase 1 (ESP32 / ESPArcade): esqueleto, terreno con
 * scroll, jugador, disparo y medidor de rendimiento.
 *
 * Portado de la fase 1 de Pico 1 (commando_pico_fase1). Mismo criterio que
 * paratrooper.c/asteroids.c: nada de Pico SDK, tiempo real con esp_timer,
 * bucle con vTaskDelayUntil, y controles con la API de controls.h de ESP32.
 * La lógica y el dibujo del terreno son los de la Pico, sin cambios.
 *
 * Diferencias respecto a la Pico:
 *  - Partida en vertical 240x320: st7789_set_rotation(CMD_ROTATION) al
 *    entrar y se vuelve a la rotación 1 (horizontal, la del menú) al salir.
 *  - Joystick 1 (ejes del ADC, normalizados -1000..1000) con zona muerta;
 *    A = disparo, B = granada (aún sin usar en esta fase). Click de cualquiera
 *    de los dos sticks (índices 4/5) = salir al menú.
 *  - El scroll vertical cambia casi todo el fondo en cada frame, así que a
 *    20 MHz (ILI9341) el límite es de ~16 fps al moverse. La lógica va a paso
 *    fijo (GAME_SPEED) y no depende de los fps. El medidor de arriba a la
 *    derecha ("NNFPS Dx.x Fx.x") dice cuántos fps reales salen, ms de dibujo
 *    y ms de flush por SPI.
 *  - Flush: por defecto renderer_flush() completo (imagen coherente, sin
 *    bandas). Con CMD_FLUSH_BUDGET > 0 usa renderer_flush_limited(); en un
 *    scroll eso reparte cada pantalla en varios frames y se ve a tirones.
 *
 * Texto original de la Pico:
 *
 * Todo en enteros (el RP2040 no tiene FPU). El mundo usa directamente
 * la resolución de pantalla (240x320): las medidas del juego HTML
 * (384x512) están escaladas por 0.625.
 *
 * Posiciones en punto fijo Q8 (1 px = 256). La lógica corre a paso fijo
 * (GAME_SPEED pasos/s, como en el HTML) y el dibujo va aparte, así la
 * velocidad del juego no depende de los fps.
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
#include "commando.h"
#include "commando_title.h"
#include "../renderer.h"
#include "../controls.h"
#include "../sound.h"
#include "../highscores.h"

/* ---------------------------------------------------------
 * Específico de ESP32: tiempo, rotación, controles
 * --------------------------------------------------------- */
#define TARGET_FPS 60                /* solo evita que el bucle corra sin descanso */

/* Orientación de la partida. 1 = apaisado 320x240 (la del menú y del resto de
 * juegos, scroll vertical con menos campo hacia delante). 0 = vertical 240x320
 * (Pac-Man usa rotación 2 en este hardware; si sale al revés, prueba 0). */
#ifndef CMD_LANDSCAPE
#define CMD_LANDSCAPE 1
#endif
#if CMD_LANDSCAPE
#define CMD_ROTATION 1
#define SCR_W 320
#define SCR_H 240
#else
#define CMD_ROTATION 2
#define SCR_W 240
#define SCR_H 320
#endif
#define MENU_ROTATION 1              /* la del menú y las pantallas horizontales */

/* ZONA DE JUEGO. En apaisado se recorta por los lados (como en vertical): solo
 * se redibuja y transmite FIELD_W px de ancho, centrados, y a los lados quedan
 * dos paneles fijos con el marcador (no cuestan nada de SPI). Menos ancho =
 * más fps. Debe ser múltiplo de 16 para que los tiles de 8 px del driver
 * queden alineados. 240 = el ancho del juego vertical original. */
#ifndef CMD_FIELD_W
#if CMD_LANDSCAPE
#define CMD_FIELD_W 192
#else
#define CMD_FIELD_W 240
#endif
#endif
#define FIELD_W CMD_FIELD_W
#define FIELD_X ((SCR_W - FIELD_W) / 2)
_Static_assert(FIELD_W % 16 == 0 && FIELD_W >= 128 && FIELD_W <= SCR_W, "CMD_FIELD_W: multiplo de 16 entre 128 y SCR_W");
#define SIDE_PANELS (FIELD_X >= 48)  /* hay sitio para marcador lateral */
#define FSC(v) ((v) * FIELD_W / 240) /* escala medidas pensadas para 240 px de ancho */

/* 0 = renderer_flush() completo cada frame. >0 = renderer_flush_limited(bytes). */
#ifndef CMD_FLUSH_BUDGET
#define CMD_FLUSH_BUDGET 0
#endif

/* Si al empujar el stick el personaje va al revés en un eje, pon 1. */
#define CMD_INVERT_X 0
#define CMD_INVERT_Y 0
#define CMD_DEADZONE 150             /* sobre 1000 */

#define JOY_MAX      256             /* rango interno de los ejes: -256..+256 */

#define BTN_FIRE     0               /* controls_button_down(): 0=J1_A */
#define BTN_NADE     1               /* 1=J1_B */
#define BTN_IDX_J1_SW 4
#define BTN_IDX_J2_SW 5

static inline uint64_t now_us(void) { return (uint64_t)esp_timer_get_time(); }

static int joy_axis(int axis_id, bool invert)
{
    int v = controls_debug_axis_normalized(axis_id);   /* -1000..1000 */
    if (invert) v = -v;
    if (v > -CMD_DEADZONE && v < CMD_DEADZONE) return 0;
    v = (v > 0) ? (v - CMD_DEADZONE) : (v + CMD_DEADZONE);
    v = v * JOY_MAX / (1000 - CMD_DEADZONE);
    if (v >  JOY_MAX) v =  JOY_MAX;
    if (v < -JOY_MAX) v = -JOY_MAX;
    return v;
}
static int controls_joy_x(void) { return joy_axis(0, CMD_INVERT_X); }   /* J1 X */
static int controls_joy_y(void) { return joy_axis(1, CMD_INVERT_Y); }   /* J1 Y, negativo = arriba */

/* Entrada leída UNA vez por frame del bucle principal (los flancos no se
 * pierden aunque en un frame corran varios pasos de lógica). */
static bool in_fire_down, in_nade_down, in_start_edge, in_quit_edge;
static int  fire_buf;                 /* pulsación de disparo pendiente (el disparo no es automático) */
static bool g_done;

/* ---------------------------------------------------------
 * Constantes
 * --------------------------------------------------------- */
#define MAP_H        2375            /* 3800 * 0.625 */
#define HUD_H        14

#define GAME_SPEED   65              /* pasos de lógica por segundo */
#define STEP_US      (1000000 / GAME_SPEED)
#define MAX_STEPS    8               /* pasos máx. recuperados por frame (a ~16 fps hacen falta 4) */

#define FX(v)        ((int32_t)(v) << 8)
#define UNFX(v)      ((int)((v) >> 8))

#define PLAYER_W     12
#define PLAYER_H     15
#define PLAYER_SPEED 256             /* 1.0 px/paso (1.6 * 0.625) */
#define FIRE_CD      9
#define BULLET_LIFE  46              /* 230 px orig * .625 / 3.1 px/paso */
#define MAX_PBULLETS 16

#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

/* ---------------------------------------------------------
 * Temas de terreno (uno por fase, en ciclo)
 * --------------------------------------------------------- */
typedef struct {
    uint16_t grass, pA, pB, pC, edge1, edge2, dirt, dirtL, rut, tuft;
} Theme;

static const Theme THEMES[3] = {
    /* Jungla  */ { RGB(0x2d,0x4a,0x12), RGB(0x3a,0x5a,0x18), RGB(0x26,0x40,0x10), RGB(0x5a,0x4a,0x24),
                    RGB(0x4c,0x5a,0x22), RGB(0x6a,0x54,0x28), RGB(0x6b,0x4c,0x27), RGB(0x7a,0x5a,0x30),
                    RGB(0x58,0x40,0x1f), RGB(0x35,0x56,0x1a) },
    /* Desierto*/ { RGB(0xa0,0x8c,0x52), RGB(0xb0,0x9a,0x5c), RGB(0x8e,0x7c,0x46), RGB(0x7a,0x6a,0x3c),
                    RGB(0x9c,0x88,0x50), RGB(0x8a,0x74,0x48), RGB(0x8a,0x6a,0x3c), RGB(0xa0,0x7c,0x48),
                    RGB(0x6e,0x55,0x30), RGB(0x6d,0x7a,0x2c) },
    /* Bosque  */ { RGB(0x3b,0x5a,0x24), RGB(0x4f,0x6e,0x2c), RGB(0x2b,0x44,0x19), RGB(0x6e,0x54,0x28),
                    RGB(0x56,0x6a,0x2a), RGB(0x6a,0x5a,0x30), RGB(0x5a,0x41,0x26), RGB(0x6b,0x4e,0x2e),
                    RGB(0x46,0x32,0x1c), RGB(0x48,0x6b,0x22) },
};

#define C_PEBBLE  RGB(0x8c, 0x7a, 0x58)
#define C_SKIN    RGB(0xff, 0xcc, 0x99)
#define C_BODY    RGB(0x2d, 0x5a, 0x1e)
#define C_LEGS    RGB(0x1e, 0x3d, 0x14)
#define C_BAND    RGB(0xe6, 0x00, 0x00)
#define C_GUN     RGB(0x11, 0x11, 0x11)
#define C_BULLET  RGB(0xff, 0xff, 0x33)

/* ---------------------------------------------------------
 * Utilidades enteras: seno por tabla, hash, aleatorio
 * --------------------------------------------------------- */
static int16_t sin_tab[256];         /* amplitud 1024, 256 pasos por vuelta */

static void math_init(void)
{
    for (int i = 0; i < 256; i++)
        sin_tab[i] = (int16_t)lroundf(sinf((float)i * 6.2831853f / 256.0f) * 1024.0f);
}

static inline int isin(uint32_t a) { return sin_tab[a & 255]; }

static uint32_t rng_state = 0x1234abcd;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
static int rnd_range(int a, int b) { return a + (int)(rng() % (uint32_t)(b - a + 1)); }

static inline uint32_t hash2i(int a, int b)
{
    uint32_t h = (uint32_t)a * 0x9E3779B1u ^ (uint32_t)b * 0x85EBCA6Bu;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}
static inline int hash8(int a, int b) { return (int)(hash2i(a, b) & 255); }

/* ---------------------------------------------------------
 * Sendero: dos senos suaves + convergencia hacia la puerta de la
 * fortaleza (igual que pathCenter/pathHalf del HTML, en enteros).
 * --------------------------------------------------------- */
typedef struct {
    int a1, a2;               /* amplitudes en px */
    int32_t f1, f2;           /* fase por px en Q16, unidades de 1/256 de vuelta */
    int p1, p2;               /* fases iniciales (0..255) */
    int h0, ha, hp;           /* semiancho base, variación y fase */
} PathParams;

static PathParams path;
static int fort_door_cx;      /* centro X de la puerta de la fortaleza */

static int32_t rad_to_q16(float f_rad_per_px)
{
    /* rad/px -> (1/256 de vuelta)/px en Q16 */
    return (int32_t)lroundf(f_rad_per_px * 256.0f / 6.2831853f * 65536.0f);
}

static void path_generate(int level)
{
    (void)level;
    /* parámetros originales * 0.625 (amplitudes) y / 0.625 (frecuencias) */
    path.a1 = FSC(rnd_range(22, 49));
    path.a2 = FSC(rnd_range(9, 22));
    path.f1 = rad_to_q16(0.0028f / 0.625f + (float)rnd_range(0, 27) / 10000.0f / 0.625f);
    path.f2 = rad_to_q16(0.008f / 0.625f + (float)rnd_range(0, 70) / 10000.0f / 0.625f);
    path.p1 = rnd_range(0, 255);
    path.p2 = rnd_range(0, 255);
    path.h0 = FSC(rnd_range(36, 45));
    if (path.h0 < 30) path.h0 = 30;
    path.ha = FSC(rnd_range(8, 15));
    path.hp = rnd_range(0, 255);
    fort_door_cx = FSC((rnd_range(50, 270) + 32) * 5 / 8);
}

static int path_center(int y)
{
    int s1 = isin(((uint32_t)y * (uint32_t)path.f1 >> 16) + (uint32_t)path.p1);
    int s2 = isin(((uint32_t)y * (uint32_t)path.f2 >> 16) + (uint32_t)path.p2);
    int c = FIELD_W / 2 + (s1 * path.a1 + s2 * path.a2) / 1024;
    /* cerca de la fortaleza (y < 594) converge a la puerta */
    int t = (594 - y) * 256 / 406;
    if (t < 0) t = 0;
    if (t > 256) t = 256;
    return (c * (256 - t) + fort_door_cx * t) >> 8;
}

static int path_half(int y)
{
    /* 0.009/.625 = 0.0144 rad/px ; 0.027/.625 = 0.0432 rad/px */
    static int32_t q_a, q_b;
    if (!q_a) { q_a = rad_to_q16(0.0144f); q_b = rad_to_q16(0.0432f); }
    int s1 = isin(((uint32_t)y * (uint32_t)q_a >> 16) + (uint32_t)path.hp);
    int s2 = isin((uint32_t)y * (uint32_t)q_b >> 16);
    return path.h0 + (s1 * path.ha) / 1024 + (s2 * 5) / 1024;
}

/* ---------------------------------------------------------
 * Estado del juego
 * --------------------------------------------------------- */
typedef enum { ST_TITLE, ST_PLAYING, ST_GAMEOVER, ST_WIN, ST_SCORES } GameState;

typedef struct {
    int32_t x, y;            /* esquina superior izquierda, Q8 */
    int     dir_x, dir_y;    /* dirección de apuntado: -1,0,1 */
    int     fire_cd;
    int     lives;
    int     grenades;
    int     anim;            /* fase de las piernas, Q4 */
    int     invuln;
    bool    dying;
    int     death_t, death_dir;
    int     nade_cd, spread, noise;
    float   vx, vy;          /* velocidad en px/paso (para apuntar con adelanto) */
} Player;

typedef struct {
    bool    active;
    int32_t x, y;            /* Q8, mundo */
    int32_t vx, vy;          /* Q8 por paso */
    int     life;
    uint32_t rolled;         /* soldados que ya intentaron esquivarla */
} Bullet;

static GameState state;
static Player    pl;
static Bullet    pbullets[MAX_PBULLETS];
static int32_t   cam_q8;                 /* cámara (arriba de la pantalla), Q8 */
static int       level = 1;
static const Theme *theme = &THEMES[0];
static uint32_t  score;
static uint32_t  title_t;
static bool      panels_ready;           /* paneles laterales: se pintan una vez por partida */

/* velocidad de bala 3.1 px/paso (5 * 0.625), en Q8 */
#define BSPEED    794              /* 3.1 px/paso en Q8 */
#define BSPEED_D  561              /* 3.1 / sqrt(2) */

static int cam_px(void) { return UNFX(cam_q8); }

/* =========================================================
 * FASE 2: obstáculos, soldados enemigos con IA, balas y granadas,
 * muerte/respawn, objetos. Medidas del HTML * 0.625 (px/paso ya escalados).
 * La IA usa float (el ESP32 tiene FPU).
 * ========================================================= */
#define MAX_ENEMIES   32
#define MAX_EBULLETS  12
#define MAX_GRENADES  8
#define MAX_EXPL      10
#define MAX_OBS       72
#define MAX_ITEMS     16
#define EW            12             /* tamaño del soldado */
#define EH            15
#define EB_SPEED      0.84f          /* 1.35 * 0.625 */
#define EB_RANGE      125.0f
#define DEATH_STEPS   110
#define INVULN_STEPS  150

typedef enum { OB_SANDBAG, OB_ROCK, OB_TREE, OB_TENT, OB_HOUSE, OB_BARRACKS, OB_BUILDING, OB_BUNKER, OB_FORT, OB_GATE, OB_BRIDGE, OB_RUBBLE, OB_WATER } ObType;
typedef struct { int16_t x, y, w, h; uint8_t type, v; int8_t hp, maxhp; } Obstacle;   /* hp en cuartos de punto */
#define IS_STRUCT(t) ((t) >= OB_TENT && (t) <= OB_BUNKER)
/* estado extra por obstáculo: torreta del búnker / reloj de aparición del barracón */
typedef struct { float ang; int cd, charge, flash, spawned; } ObAux;
static ObAux aux[MAX_OBS];
static Obstacle obs[MAX_OBS];
static int      obs_n;

typedef enum { E_PATROL, E_SEARCH, E_CHASE } EState;
typedef struct {
    bool active, anchored, dormant, has_los, has_lk, alerted, stat, fort, rusher;
    uint8_t type, state;           /* type: 0 soldado, 1 oficial */
    int8_t  cover;                 /* índice de obstáculo (sacos de arena) o -1 */
    float   x, y, fx, fy;          /* esquina sup. izq. y dirección de apuntado */
    float   pat_ang, det_ang, strafe, dodge_x, dodge_y, lkx, lky;
    int     shoot_cd, nade_cd, strafe_t, patrol_t, search_t, dodge_t, detour_t;
    int     los_t, age, life, leaving, peek, anim, aim, rocket_cd, mflash, slot_t, hold_flip;
    float   slot_x, slot_y, hold_dir;
    uint8_t escort;                /* 1..3: escolta del rehén n-1 */
} Enemy;

typedef struct { bool active, rocket, shell; float x, y, vx, vy, dist, sp; int8_t ignore; } EBullet;
typedef struct { bool active; bool mine; float sx, sy, tx, ty, prog, speed, arc, r; uint32_t rolled; } Grenade;
typedef struct { bool active; float x, y, r, maxr; } Explosion;
typedef enum { IT_SPREAD, IT_GRENADES, IT_LIFE } ItemType;
typedef struct { bool active; int16_t x, y; uint8_t type; } Item;
#define MAX_HOSTAGES 3
typedef struct { bool active, freed; float x, y; int t, killed; } Hostage;
static Hostage hostages[MAX_HOSTAGES];

static Enemy     enemies[MAX_ENEMIES];
static EBullet   ebullets[MAX_EBULLETS];
static Grenade   grenades[MAX_GRENADES];
static Explosion booms[MAX_EXPL];
static Item      items[MAX_ITEMS];
static int       spawn_timer;
static int       gameover_t;
static int       scores_t;
static bool      scores_drawn;
static bool      g_demo;

static float frnd(float a, float b) { return a + (float)(rng() & 0xFFFF) / 65536.0f * (b - a); }
static inline float pl_cx(void) { return (float)pl.x / 256.0f + PLAYER_W / 2.0f; }
static inline float pl_cy(void) { return (float)pl.y / 256.0f + PLAYER_H / 2.0f; }

static int max_ebullets(void) { int n = 3 + 2 * (level - 1); return n > 20 ? 20 : n; }
static int max_egrenades(void) { int n = 1 + (level - 1); return n > 8 ? 8 : n; }
static bool can_enemy_shoot(void)
{
    int n = 0;
    for (int i = 0; i < MAX_EBULLETS; i++) n += ebullets[i].active;
    return n < max_ebullets() && n < MAX_EBULLETS;
}
static bool can_enemy_throw(void)
{
    int n = 0;
    for (int i = 0; i < MAX_GRENADES; i++) n += (grenades[i].active && !grenades[i].mine);
    return n < max_egrenades();
}

/* ---- Obstáculos ---- */
static bool rects_hit(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh)
{
    return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}
static bool obs_collide(int x, int y, int w, int h)
{
    for (int i = 0; i < obs_n; i++)
        if (obs[i].type != OB_RUBBLE && rects_hit(x, y, w, h, obs[i].x, obs[i].y, obs[i].w, obs[i].h)) return true;
    return false;
}
static int point_solid_idx(int x, int y, int ignore)
{
    for (int i = 0; i < obs_n; i++) {
        if (i == ignore) continue;
        const Obstacle *o = &obs[i];
        if (o->type != OB_RUBBLE && o->type != OB_WATER && x >= o->x && x < o->x + o->w && y >= o->y && y < o->y + o->h) return i;
    }
    return -1;
}
static bool point_solid(int x, int y, int ignore) { return point_solid_idx(x, y, ignore) >= 0; }
static bool los_blocked(float x1, float y1, float x2, float y2)
{
    float dx = x2 - x1, dy = y2 - y1;
    int n = (int)(hypotf(dx, dy) / 5.0f) + 1;
    for (int i = 1; i < n; i++) {
        float t = (float)i / (float)n;
        if (point_solid((int)(x1 + dx * t), (int)(y1 + dy * t), -1)) return true;
    }
    return false;
}

/* zonas reservadas (ríos, puente, fortaleza, salida): ningún elemento corriente cae encima */
typedef struct { int16_t x, y, w, h; } Zone;
static Zone zones[8];
static int  zone_n;
static void zone_add(int x, int y, int w, int h) { if (zone_n < 8) zones[zone_n++] = (Zone){ (int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h }; }
static bool zone_blocked(int x, int y, int w, int h)
{
    for (int i = 0; i < zone_n; i++)
        if (rects_hit(x, y, w, h, zones[i].x, zones[i].y, zones[i].w, zones[i].h)) return true;
    return false;
}

static bool obs_add(int type, int x, int y, int w, int h)
{
    if (obs_n >= MAX_OBS) return false;
    if (type < OB_FORT && zone_blocked(x - 9, y - 9, w + 18, h + 18)) return false;
    if ((type < OB_FORT || type > OB_BRIDGE) && type != OB_WATER) {
        if (x < 4) x = 4;
        if (x + w > FIELD_W - 4) x = FIELD_W - 4 - w;
    }
    static const int8_t hp_q[13] = { 0, 0, 0, 8, 12, 16, 20, 12, 0, 0, 0, 0, 0 };   /* tienda 2, casa 3, barracón 4, edificio 5, búnker 3 */
    aux[obs_n] = (ObAux){ 1.5708f, rnd_range(90, 200), 0, 0, 0 };
    obs[obs_n++] = (Obstacle){ (int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, (uint8_t)type,
                               (uint8_t)rnd_range(0, 255), hp_q[type], hp_q[type] };
    return true;
}

/* ---- Fortaleza final y puente con túnel ---- */
#define FORT_DOOR_W    36
#define FORT_ENEMIES   20
#define FORT_TRIGGER_Y 262
#define BR_H           40
#define TUNNEL_W       42
static int  fort_x;                 /* esquina izquierda de la puerta */
static int  gate_idx;
static bool fort_active;
static int  fort_spawned, fort_killed, fort_timer;
static int  bridge_y, tunnel_x;
typedef struct { int16_t y, h, bx, bw; } River;
static River rivers[2];
static int   river_n;
static int  win_t;

static bool near_bridge(int y, int h) { return y + h > bridge_y - 36 && y < bridge_y + BR_H + 36; }
static bool player_in_tunnel(void);

static void fort_bridge_generate(void)
{
    fort_x = fort_door_cx - FORT_DOOR_W / 2;
    if (fort_x < 6) fort_x = 6;
    if (fort_x > FIELD_W - FORT_DOOR_W - 6) fort_x = FIELD_W - FORT_DOOR_W - 6;
    obs_add(OB_FORT, 0, 12, FIELD_W, 32);
    obs_add(OB_FORT, 0, 44, fort_x, 68);
    obs_add(OB_FORT, fort_x + FORT_DOOR_W, 44, FIELD_W - fort_x - FORT_DOOR_W, 68);
    gate_idx = obs_n;
    obs_add(OB_GATE, fort_x, 74, FORT_DOOR_W, 8);
    zone_add(0, 0, FIELD_W, 206);                         /* zona de la fortaleza */
    zone_add(0, MAP_H - 137, FIELD_W, 137);               /* zona de salida del jugador */

    /* tres elementos que cruzan el campo (puente + 2 ríos) en posiciones y orden aleatorios */
    int slots[3], ns = 0;
    for (int t = 0; t < 600 && ns < 3; t++) {
        int y = rnd_range(594, MAP_H - 719);
        bool ok = true;
        for (int k = 0; k < ns; k++) if (abs(slots[k] - y) < 269) ok = false;
        if (ok) slots[ns++] = y;
    }
    if (ns < 3) { slots[0] = MAP_H - 1375; slots[1] = MAP_H - 938; slots[2] = MAP_H - 563; }
    for (int i = 0; i < 2; i++)
        for (int j = i + 1; j < 3; j++) if (slots[j] < slots[i]) { int t = slots[i]; slots[i] = slots[j]; slots[j] = t; }
    int bridge_idx = rnd_range(0, 2);
    river_n = 0;
    for (int i = 0; i < 3; i++) {
        if (i == bridge_idx) {                           /* el puente solo se pasa por el túnel */
            bridge_y = slots[i];
            tunnel_x = rnd_range(25, FIELD_W - TUNNEL_W - 25);
            obs_add(OB_BRIDGE, 0, bridge_y, tunnel_x, BR_H);
            obs_add(OB_BRIDGE, tunnel_x + TUNNEL_W, bridge_y, FIELD_W - tunnel_x - TUNNEL_W, BR_H);
            zone_add(0, bridge_y - 31, FIELD_W, BR_H + 62);
        } else {                                         /* río con un puente de madera */
            River *r = &rivers[river_n++];
            r->y = (int16_t)slots[i]; r->h = (int16_t)rnd_range(37, 59); r->bw = (int16_t)rnd_range(40, 56);
            r->bx = (int16_t)rnd_range(25, FIELD_W - 25 - r->bw);
            obs_add(OB_WATER, 0, r->y, r->bx, r->h);
            obs_add(OB_WATER, r->bx + r->bw, r->y, FIELD_W - r->bx - r->bw, r->h);
            zone_add(0, r->y - 25, FIELD_W, r->h + 50);
        }
    }
}

static void bunkers_generate(void)
{
    int step = 281 - 22 * (level - 1);
    if (step < 200) step = 200;
    for (int y = MAP_H - 380; y > 280; y -= step) {
        for (int tries = 0; tries < 8; tries++) {
            int side = (rng() & 1) ? 1 : -1;
            int py = y + rnd_range(-25, 25);
            int c = path_center(py), h = path_half(py);
            int x = c + side * (h + 8 + rnd_range(0, 25)) - (side < 0 ? 44 : 0);
            if (x < 6 || x + 44 > FIELD_W - 6) continue;
            if (near_bridge(py, 34) || py < 250) continue;
            if (obs_collide(x - 6, py - 6, 56, 46)) continue;
            if (obs_add(OB_BUNKER, x, py, 44, 34)) break;
        }
    }
}

static void obs_generate(void)
{
    obs_n = 0;
    zone_n = 0;
    fort_bridge_generate();
    bunkers_generate();
    for (int y = MAP_H - 200; y > 230 && obs_n < MAX_OBS; y -= rnd_range(46, 80)) {
        int c = path_center(y), h = path_half(y);
        if (near_bridge(y - 20, 60)) continue;
        int r = rnd_range(0, 99);
        int side = (rng() & 1) ? 1 : -1;
        if (r < 30) {                                    /* barricada de sacos a un lado del sendero */
            obs_add(OB_SANDBAG, c + side * h - (side > 0 ? 44 : 0), y, 44, 12);
        } else if (r < 44) {                             /* tienda, casa, barracón o edificio */
            static const int8_t sz[4][2] = { { 27, 21 }, { 35, 28 }, { 70, 29 }, { 40, 39 } };
            int k = rnd_range(0, 3), w = sz[k][0], hh = sz[k][1];
            obs_add(OB_TENT + k, c + side * (h + rnd_range(8, 30)) - (side < 0 ? w : 0), y, w, hh);
        } else if (r < 66) {                             /* roca junto al camino */
            int w = rnd_range(12, 18), hh = rnd_range(9, 13);
            obs_add(OB_ROCK, c + side * (h + rnd_range(4, 40)) - w / 2, y, w, hh);
        } else {                                         /* árbol (tronco macizo) */
            obs_add(OB_TREE, c + side * (h + rnd_range(12, 60)) - 4, y, 8, 8);
        }
    }
}

/* ---- Estructuras destruibles ---- */
static void expl_add(float x, float y, float maxr);
static void item_spawn(int x, int y, int type);
static void struct_damage(int i, int q)
{
    Obstacle *o = &obs[i];
    if (!IS_STRUCT(o->type) || o->hp <= 0) return;
    o->hp = (int8_t)(o->hp - q);
    if (o->hp > 0) return;
    static const uint16_t pts[5] = { 100, 300, 600, 500, 500 };
    score += pts[o->type - OB_TENT];
    expl_add(o->x + o->w / 2.0f, o->y + o->h / 2.0f, 16 + o->w / 6);
    sound_effect_explosion();
    if (o->type == OB_BUNKER || rnd_range(0, 99) < 40)
        item_spawn(o->x + o->w / 2 - 5, o->y + o->h / 2 - 5, rnd_range(0, 1) ? IT_GRENADES : IT_SPREAD);
    o->type = OB_RUBBLE; o->hp = 0;
}

/* ---- Jugador: muerte, objetos ---- */
static void expl_add(float x, float y, float maxr)
{
    for (int i = 0; i < MAX_EXPL; i++) {
        if (booms[i].active) continue;
        booms[i] = (Explosion){ true, x, y, 3.0f, maxr };
        return;
    }
}

static void player_hit(void)
{
    if (pl.invuln > 0 || pl.dying) return;
    pl.lives--;
    pl.dying = true;
    pl.death_t = DEATH_STEPS;
    pl.death_dir = (rng() & 1) ? 1 : -1;
    expl_add(pl_cx(), pl_cy(), 14);
    sound_effect_explosion();
}

static void item_spawn(int x, int y, int type)
{
    for (int i = 0; i < MAX_ITEMS; i++) {
        if (items[i].active) continue;
        items[i] = (Item){ true, (int16_t)x, (int16_t)y, (uint8_t)type };
        return;
    }
}

static void item_drop(float x, float y, int pct)
{
    if (rnd_range(0, 99) >= pct) return;
    static const uint8_t kinds[4] = { IT_SPREAD, IT_GRENADES, IT_GRENADES, IT_LIFE };
    for (int i = 0; i < MAX_ITEMS; i++) {
        if (items[i].active) continue;
        items[i] = (Item){ true, (int16_t)x, (int16_t)y, kinds[rnd_range(0, 3)] };
        return;
    }
}

static void enemy_kill(Enemy *e)
{
    if (!e->active) return;
    e->active = false;
    if (e->fort) fort_killed++;
    if (e->escort) hostages[e->escort - 1].killed++;
    static const uint16_t pts[4] = { 100, 300, 250, 250 };
    score += pts[e->type & 3];
    expl_add(e->x + EW / 2.0f, e->y + EH / 2.0f, 12);
    sound_effect_hit();
    item_drop(e->x, e->y, e->type >= 2 ? 35 : 20);
}

/* ---- Soldados ---- */
static void enemy_init(Enemy *e, float x, float y, int type)
{
    memset(e, 0, sizeof(*e));
    e->active = true; e->x = x; e->y = y; e->type = (uint8_t)type;
    e->fx = 0; e->fy = 1; e->cover = -1; e->state = E_PATROL;
    e->shoot_cd = rnd_range(60, 140);
    e->nade_cd = rnd_range(240, 540);
    e->pat_ang = frnd(0, 6.2831f);
    e->strafe = (rng() & 1) ? 1.0f : -1.0f;
    e->strafe_t = rnd_range(60, 150);
    e->los_t = rnd_range(0, 5);
    e->anim = rnd_range(0, 255);
    e->life = rnd_range(1500, 2400);
    e->rocket_cd = rnd_range(110, 230);
}

static Enemy *enemy_alloc(void)
{
    for (int i = 0; i < MAX_ENEMIES; i++)
        if (!enemies[i].active) return &enemies[i];
    return NULL;
}

static int enemy_pick_type(void)
{
    int bz = 7 + 4 * (level - 1); if (bz > 30) bz = 30;
    int r = rnd_range(0, 99);
    return r < bz ? 2 : (r < bz + 18 ? 1 : 0);
}

static void ebullet_fire_sp(float ox, float oy, float ang, int ignore, bool rocket, bool shell, float sp)
{
    for (int i = 0; i < MAX_EBULLETS; i++) {
        EBullet *b = &ebullets[i];
        if (b->active) continue;
        b->active = true; b->rocket = rocket; b->shell = shell; b->sp = sp; b->x = ox; b->y = oy; b->dist = 0; b->ignore = (int8_t)ignore;
        b->vx = cosf(ang) * sp; b->vy = sinf(ang) * sp;
        return;
    }
}
static void ebullet_fire_ex(float ox, float oy, float ang, int ignore, bool rocket, bool shell)
{
    ebullet_fire_sp(ox, oy, ang, ignore, rocket, shell, rocket ? 0.94f : (shell ? 1.375f : EB_SPEED));
}
static void ebullet_fire(float ox, float oy, float ang, int ignore) { ebullet_fire_ex(ox, oy, ang, ignore, false, false); }

static void rocket_fire(Enemy *e)
{
    float cx = e->x + EW / 2.0f, cy = e->y + EH / 2.0f;
    float tx = pl_cx(), ty = pl_cy();
    float t = hypotf(tx - cx, ty - cy) / 0.94f * 0.35f;
    float a = atan2f(ty + pl.vy * t - cy, tx + pl.vx * t - cx) + frnd(-0.11f, 0.11f);
    ebullet_fire_ex(cx + cosf(a) * 9, cy + sinf(a) * 9, a, -1, true, false);
    e->fx = cosf(a); e->fy = sinf(a);
}

static void rocket_detonate(const EBullet *b)
{
    expl_add(b->x, b->y, 16);
    sound_effect_explosion();
    if (!pl.dying && !player_in_tunnel() && hypotf(pl_cx() - b->x, pl_cy() - b->y) < 15) player_hit();
}

static void enemy_shoot(Enemy *e, float dist)
{
    float cx = e->x + EW / 2.0f, cy = e->y + EH / 2.0f;
    float tx = pl_cx(), ty = pl_cy();
    if (e->type == 1) {                                   /* el oficial anticipa el movimiento */
        float t = dist / EB_SPEED * 0.3f;
        tx += pl.vx * t; ty += pl.vy * t;
    }
    float err = (e->type == 1 ? 0.16f : 0.3f) * frnd(-1, 1);
    ebullet_fire(cx, cy, atan2f(ty - cy, tx - cx) + err, e->cover);
}

static void grenade_throw_ex(float sx, float sy, float tx, float ty, bool mine, float speed, float arc, float r)
{
    for (int i = 0; i < MAX_GRENADES; i++) {
        Grenade *g = &grenades[i];
        if (g->active) continue;
        *g = (Grenade){ true, mine, sx, sy, tx, ty, 0, speed, arc, r, 0 };
        return;
    }
}
static void grenade_throw(float sx, float sy, float tx, float ty, bool mine)
{
    grenade_throw_ex(sx, sy, tx, ty, mine, mine ? 0.025f : 0.012f, mine ? 25.0f : 22.0f, mine ? 28.0f : 24.0f);
}

static void enemy_try_move(Enemy *e, float mx, float my, float spd)
{
    float nx = e->x + mx * spd;
    if (nx < 0) nx = 0;
    if (nx > FIELD_W - EW) nx = FIELD_W - EW;
    float ny = e->y + my * spd;
    bool moved = false;
    if (!obs_collide((int)nx, (int)e->y, EW, EH)) { if (nx != e->x) moved = true; e->x = nx; }
    if (!obs_collide((int)e->x, (int)ny, EW, EH)) { if (ny != e->y) moved = true; e->y = ny; }
    if (!moved && e->detour_t <= 0 && e->dodge_t <= 0) {
        e->detour_t = rnd_range(35, 60);
        e->det_ang = atan2f(my, mx) + ((rng() & 1) ? 1.5708f : -1.5708f);
    }
}

static void enemy_release(Enemy *e)
{
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy *o = &enemies[i];
        if (o->active && o->anchored && o->cover == e->cover) {
            o->anchored = false; o->cover = -1; o->peek = 0;
            o->state = E_CHASE; o->alerted = true;
            o->shoot_cd = rnd_range(40, 80);
        }
    }
}

static void enemy_cover_update(Enemy *e, float dist, float ang, float pcy)
{
    int camy = cam_px();
    if (e->y > camy + SCR_H + 60) { e->active = false; return; }
    if (dist > 212) return;
    e->fx = cosf(ang); e->fy = sinf(ang);
    const Obstacle *o = &obs[e->cover];
    if (dist < 62 || pcy < o->y - 3) { enemy_release(e); return; }
    if (e->peek > 0) {
        e->peek--;
        if (e->peek == 26 && !pl.dying && can_enemy_shoot()) enemy_shoot(e, dist);
    } else if (e->shoot_cd > 0) {
        e->shoot_cd--;
    } else if (dist < 137 && !pl.dying) {
        e->peek = 34;
        e->shoot_cd = rnd_range(80, 150);
    }
}

/* Los oficiales esquivan balas del jugador; todos huyen de sus granadas. */
static void enemy_check_dodge(Enemy *e, int idx, float cx, float cy)
{
    if (e->type == 1) {
        for (int i = 0; i < MAX_PBULLETS; i++) {
            Bullet *b = &pbullets[i];
            if (!b->active) continue;
            float bx = (float)b->x / 256.0f, by = (float)b->y / 256.0f;
            float vx = (float)b->vx / 256.0f, vy = (float)b->vy / 256.0f;
            float dx = cx - bx, dy = cy - by, d = hypotf(dx, dy);
            if (d > 53 || d < 5) continue;
            float sp = hypotf(vx, vy);
            if (sp < 0.01f) continue;
            if ((vx * dx + vy * dy) / (sp * d) < 0.9f) continue;
            if (b->rolled & (1u << idx)) continue;
            b->rolled |= (1u << idx);
            if (frnd(0, 1) > 0.75f) continue;
            float nx = -vy / sp, ny = vx / sp;
            float side = dx * nx + dy * ny;
            float s = side > 0 ? 1.0f : -1.0f;
            e->dodge_x = nx * s; e->dodge_y = ny * s; e->dodge_t = 18;
            return;
        }
    }
    for (int i = 0; i < MAX_GRENADES; i++) {
        Grenade *g = &grenades[i];
        if (!g->active || !g->mine || g->prog < 0.2f) continue;
        float d = hypotf(cx - g->tx, cy - g->ty);
        if (d < g->r + 9) {
            if (g->rolled & (1u << idx)) continue;
            g->rolled |= (1u << idx);
            if (frnd(0, 1) > 0.75f) continue;
            if (d < 0.01f) d = 1;
            e->dodge_x = (cx - g->tx) / d; e->dodge_y = (cy - g->ty) / d; e->dodge_t = 28;
            return;
        }
    }
}

static void fort_pick_slot(Enemy *e, float pcx, float pcy)
{
    float lo = e->rusher ? 28.0f : 53.0f, hi = e->rusher ? 59.0f : 125.0f;
    int camy = cam_px();
    float maxy = pcy + 75; if (maxy > camy + SCR_H - 37) maxy = (float)(camy + SCR_H - 37); if (maxy < 156) maxy = 156;
    bool ok = false;
    for (int t = 0; t < 16 && !ok; t++) {
        float x = frnd(14, FIELD_W - 26), y = frnd(128, maxy);
        float d = hypotf(x + 6 - pcx, y + 7 - pcy);
        if (d < lo || d > hi || obs_collide((int)x, (int)y, EW, EH)) continue;
        bool free_ = true;
        for (int k = 0; k < MAX_ENEMIES && free_; k++) {
            const Enemy *o = &enemies[k];
            if (o != e && o->active && o->fort && hypotf(o->slot_x - x, o->slot_y - y) < 27) free_ = false;
        }
        if (!free_) continue;
        e->slot_x = x; e->slot_y = y; ok = true;
    }
    if (!ok) {
        float a = frnd(0, 6.2831f), r = frnd(lo + 10, hi - 12);
        e->slot_x = pcx + cosf(a) * r - 6; e->slot_y = pcy + sinf(a) * r - 7;
        if (e->slot_x < 12) e->slot_x = 12;
        if (e->slot_x > FIELD_W - 24) e->slot_x = (float)(FIELD_W - 24);
        if (e->slot_y < 128) e->slot_y = 128;
        if (e->slot_y > camy + SCR_H - 37) e->slot_y = (float)(camy + SCR_H - 37);
    }
    e->slot_t = rnd_range(150, 320);
}

static void fort_move(Enemy *e, float cx, float cy, float pcx, float pcy, float ang, float *mx, float *my)
{
    if (cy < 120) {                                      /* salir por la puerta sin pararse */
        float tx = fort_x + FORT_DOOR_W / 2.0f;
        float d = (tx - cx) / 12.0f;
        *mx = d > 1 ? 1 : (d < -1 ? -1 : d); *my = 1;
        return;
    }
    if (e->slot_t <= 0 || e->slot_x == 0) fort_pick_slot(e, pcx, pcy);
    e->slot_t--;
    float sx = e->slot_x + 6 - cx, sy = e->slot_y + 7 - cy, d = hypotf(sx, sy);
    if (d > 9) { *mx = sx / d; *my = sy / d; return; }
    if (--e->hold_flip <= 0) { e->hold_dir = -e->hold_dir; e->hold_flip = rnd_range(50, 110); }
    *mx = -sinf(ang) * e->hold_dir * 0.45f; *my = cosf(ang) * e->hold_dir * 0.45f;
}

static void enemy_update(Enemy *e, int idx)
{
    const float spd0 = e->type == 1 ? 0.47f : (e->type == 2 ? 0.31f : 0.34f);
    float cx = e->x + EW / 2.0f, cy = e->y + EH / 2.0f;
    float pcx = pl_cx(), pcy = pl_cy();
    float dist = hypotf(pcx - cx, pcy - cy);
    float ang = atan2f(pcy - cy, pcx - cx);
    int camy = cam_px();

    if (e->anchored) { enemy_cover_update(e, dist, ang, pcy); return; }
    if (e->dormant && !e->fort) {
        if (e->y > camy - 12 && e->y < camy + SCR_H && dist < 270) e->dormant = false;
        else return;
    }

    if (e->stat) {                                        /* mortero emplazado */
        if (e->y > camy + SCR_H + 60) { e->active = false; return; }
        if (e->mflash > 0) e->mflash--;
        if (dist < 47) { e->stat = false; e->state = E_CHASE; return; }   /* pegado a él: lucha como soldado */
        e->fx = cosf(ang); e->fy = sinf(ang);
        if (e->rocket_cd > 0) { e->rocket_cd--; return; }
        if (pl.dying || dist > 225 || dist < 75 || e->y < camy || e->y > camy + SCR_H - 12) return;
        if (!can_enemy_throw()) { e->rocket_cd = rnd_range(20, 50); return; }
        grenade_throw_ex(cx, e->y + 2, pcx + pl.vx * 24 + frnd(-19, 19), pcy + pl.vy * 24 + frnd(-19, 19),
                         false, 0.0075f, 59.0f, 29.0f);
        e->mflash = 8;
        e->rocket_cd = 250 + rnd_range(0, 130) - level * 12; if (e->rocket_cd < 150) e->rocket_cd = 150;
        return;
    }

    /* Desaparición por tiempo o por el borde de la pantalla (la fortaleza no) */
    if (e->escort) {                                       /* escolta de rehén: no se va, huye del jugador y se queda junto al rehén */
        e->age++;
        if (e->age > 60 && (e->x < -12 || e->x > FIELD_W || e->y < camy - 19 || e->y > camy + SCR_H)) {
            for (int k = 0; k < MAX_ENEMIES; k++) if (enemies[k].active && enemies[k].escort == e->escort) enemies[k].active = false;
            return;
        }
        if (dist < 130 && e->state != E_CHASE) e->state = E_CHASE;
    }
    if (!e->fort && !e->escort) {
    if (e->leaving > 0) { if (--e->leaving <= 0) e->active = false; return; }
    e->age++;
    {
        bool off = e->x < -12 || e->x > FIELD_W || e->y < camy - 19 || e->y > camy + SCR_H;
        if (off && e->age > 120) { e->active = false; return; }
        bool edge = e->x < 3 || e->x > FIELD_W - 15 || e->y < camy + 3 || e->y > camy + SCR_H - 16;
        if (e->age > e->life || (edge && e->age > 240)) { e->leaving = 30; return; }
    }
    }

    /* Percepción */
    if (--e->los_t <= 0) { e->los_t = 6; e->has_los = !los_blocked(cx, cy, pcx, pcy); }
    bool sees = e->fort ? dist < 281 : (dist < (e->type ? 119 : 100) && e->has_los);
    bool heard = !sees && pl.noise > 0 && dist < 187;
    if (sees) {
        if (e->state != E_CHASE) e->state = E_CHASE;
        e->lkx = pcx; e->lky = pcy; e->has_lk = true;
    } else if (heard) {
        if (e->state != E_SEARCH) { e->state = E_SEARCH; e->search_t = 420; }
        e->lkx = pcx; e->lky = pcy; e->has_lk = true;
    } else if (e->state == E_CHASE) {
        e->state = E_SEARCH; e->search_t = 420;
    }

    if (e->dodge_t <= 0) enemy_check_dodge(e, idx, cx, cy);

    /* Decisión de movimiento */
    float mx = 0, my = 0, spd = spd0 * (e->rusher ? 1.2f : 1.0f);
    if (e->aim > 0) {
        /* quieto apuntando el bazooka */
    } else if (e->dodge_t > 0) {
        e->dodge_t--; mx = e->dodge_x; my = e->dodge_y; spd = e->type ? 1.19f : 1.0f;
    } else if (e->detour_t > 0) {
        e->detour_t--; mx = cosf(e->det_ang); my = sinf(e->det_ang);
    } else if (e->fort) {
        fort_move(e, cx, cy, pcx, pcy, ang, &mx, &my);
    } else if (e->state == E_CHASE) {
        float nearD = e->type == 2 ? 75.0f : 53.0f, farD = e->type == 2 ? 119.0f : 84.0f;
        if (dist > farD) { mx = cosf(ang); my = sinf(ang); }
        else if (dist < nearD) { mx = -cosf(ang); my = -sinf(ang); }
        if (--e->strafe_t <= 0) { e->strafe = -e->strafe; e->strafe_t = rnd_range(60, 150); }
        if (dist < 119) { mx += -sinf(ang) * e->strafe * 0.8f; my += cosf(ang) * e->strafe * 0.8f; }
    } else if (e->state == E_SEARCH) {
        if (e->has_lk) {
            float d = hypotf(e->lkx - cx, e->lky - cy);
            if (d > 10) {
                float a = atan2f(e->lky - cy, e->lkx - cx);
                mx = cosf(a); my = sinf(a);
            } else {
                if (--e->patrol_t <= 0) { e->pat_ang = frnd(0, 6.2831f); e->patrol_t = rnd_range(40, 90); }
                mx = cosf(e->pat_ang) * 0.6f; my = sinf(e->pat_ang) * 0.6f;
            }
        }
        if (--e->search_t <= 0) { e->state = E_PATROL; e->has_lk = false; }
    } else {
        if (--e->patrol_t <= 0) {
            e->patrol_t = rnd_range(90, 210);
            e->pat_ang = (rng() & 0xFF) < 166 ? ang + frnd(-1, 1) : frnd(0, 6.2831f);
        }
        mx = cosf(e->pat_ang) * 0.8f; my = sinf(e->pat_ang) * 0.8f;
    }

    /* Ataque */
    if (e->shoot_cd > 0) e->shoot_cd--;
    if (e->nade_cd > 0) e->nade_cd--;
    if (e->type == 2) {
        if (e->aim > 0 && --e->aim <= 0) rocket_fire(e);
        if (e->rocket_cd > 0) e->rocket_cd--;
        if (e->state == E_CHASE && e->aim <= 0 && !pl.dying && e->rocket_cd <= 0 && dist > 50 && dist < 169 && e->has_los) {
            if (can_enemy_shoot()) { e->aim = 30; e->rocket_cd = (210 + rnd_range(0, 90) - level * 5) > 120 ? 210 + rnd_range(0, 90) - level * 5 : 120; }
            else e->rocket_cd = rnd_range(15, 40);
        }
    } else if (e->state == E_CHASE && e->dodge_t <= 0 && !pl.dying) {
        if (e->shoot_cd <= 0 && dist < 125 && e->has_los) {
            if (can_enemy_shoot()) { enemy_shoot(e, dist); e->shoot_cd = (e->fort ? 75 : 100) + rnd_range(0, 59); }
            else e->shoot_cd = rnd_range(8, 29);
        }
        if (e->nade_cd <= 0 && dist > 44 && dist < 134) {
            if (can_enemy_throw()) {
                grenade_throw(cx, cy,
                              pcx + frnd(-14, 14) + pl.vx * 12, pcy + frnd(-14, 14) + pl.vy * 12, false);
                e->nade_cd = (e->fort ? 200 : 280) + rnd_range(0, 419);
            } else e->nade_cd = rnd_range(20, 59);
        }
    }

    if (e->escort) {
        const Hostage *h = &hostages[e->escort - 1];
        float hx = h->x + 6, hy = h->y + 7, dh = hypotf(hx - cx, hy - cy);
        mx = my = 0;
        if (dist < 162) { mx = -cosf(ang) * 0.8f; my = -sinf(ang) * 0.8f; }
        if (dh > 25) { mx += (hx - cx) / dh * 1.2f; my += (hy - cy) / dh * 1.2f; }
    }
    /* Movimiento */
    float len = hypotf(mx, my);
    if (len > 1) { mx /= len; my /= len; len = 1; }
    if (e->state == E_CHASE) { e->fx = cosf(ang); e->fy = sinf(ang); }
    else if (len > 0.05f) { e->fx = mx / len; e->fy = my / len; }
    if (len > 0.01f) { enemy_try_move(e, mx, my, spd); e->anim += 5; }

    for (int i = 0; i < MAX_ENEMIES; i++) {                /* separación entre soldados */
        const Enemy *o = &enemies[i];
        if (i == idx || !o->active || o->anchored) continue;
        float sx = e->x - o->x, sy = e->y - o->y, sd = hypotf(sx, sy);
        if (sd > 0 && sd < 10) enemy_try_move(e, sx / sd, sy / sd, 0.22f);
    }
}

static void enemies_spawn(void)
{
    if (fort_active) return;
    if (++spawn_timer <= 120 + (level < 4 ? (4 - level) * 30 : 0)) return;
    spawn_timer = 0;
    int act = 0;
    for (int i = 0; i < MAX_ENEMIES; i++)
        if (enemies[i].active && !enemies[i].anchored && !enemies[i].dormant && !enemies[i].stat && !enemies[i].escort) act++;
    int limit = 3 + 2 * level; if (limit > 10) limit = 10;
    if (act >= limit) return;
    int camy = cam_px();
    for (int tries = 0; tries < 6; tries++) {
        int side = rnd_range(0, 2);
        int sx, sy;
        if (side == 0) { sx = 6; sy = camy + rnd_range(0, SCR_H - 125); }
        else if (side == 1) { sx = FIELD_W - 18; sy = camy + rnd_range(0, SCR_H - 125); }
        else { sx = rnd_range(0, FIELD_W - 18); sy = camy + 25; }
        if (sy < 206) continue;
        if (obs_collide(sx, sy, EW, EH)) continue;
        Enemy *e = enemy_alloc();
        if (!e) return;
        enemy_init(e, (float)sx, (float)sy, enemy_pick_type());
        return;
    }
}

/* Guardias dormidos a lo largo del sendero y soldados tras cada barricada */
static void enemies_populate(void)
{
    memset(enemies, 0, sizeof(enemies));
    memset(hostages, 0, sizeof(hostages));
    {   /* rehenes con dos guardias cada uno */
        static const int16_t ylo[3] = { 1750, 1062, 562 }, yhi[3] = { 2094, 1437, 937 };
        for (int i = 0; i < MAX_HOSTAGES; i++) {
            for (int tries = 0; tries < 20; tries++) {
                int x = rnd_range(10, FIELD_W - 70), y = rnd_range(ylo[i], yhi[i]);
                if (obs_collide(x - 4, y - 4, 64, 29) || zone_blocked(x - 8, y - 8, 72, 37)) continue;
                for (int g = 0; g < 2; g++) {
                    Enemy *e = enemy_alloc();
                    if (!e) break;
                    enemy_init(e, (float)(x + g * 44), (float)y, 0);
                    e->dormant = true; e->escort = (uint8_t)(i + 1);
                }
                hostages[i] = (Hostage){ true, false, (float)(x + 22), (float)(y + 2), 0, 0 };
                break;
            }
        }
    }
    for (int i = 0; i < obs_n; i++) {
        if (obs[i].type != OB_SANDBAG) continue;
        int n = rnd_range(0, 99) < (level == 1 ? 30 : 60) ? 2 : 1;
        for (int k = 0; k < n; k++) {
            Enemy *e = enemy_alloc();
            if (!e) return;
            float x = n == 2 ? obs[i].x + 4 + k * 24 : obs[i].x + 16;
            enemy_init(e, x, (float)(obs[i].y - EH - 1), rnd_range(0, 99) < 20 ? 1 : 0);
            e->anchored = true; e->cover = (int8_t)i;
            e->shoot_cd = rnd_range(40, 140);
        }
    }
    {   /* morteros emplazados */
        int nm = 1 + level; if (nm > 5) nm = 5;
        for (int n = 0; n < nm; n++) {
            for (int tries = 0; tries < 10; tries++) {
                int x = rnd_range(10, FIELD_W - 24), y = rnd_range(500, MAP_H - 437);
                if (obs_collide(x - 4, y - 4, 20, 24) || near_bridge(y, 15)) continue;
                Enemy *e = enemy_alloc();
                if (!e) break;
                enemy_init(e, (float)x, (float)y, 3);
                e->dormant = true; e->stat = true; e->fx = 0; e->fy = 1;
                e->rocket_cd = rnd_range(60, 160);
                break;
            }
        }
    }
    for (int y = MAP_H - 420; y > 200; y -= rnd_range(190, 260)) {
        int c = path_center(y), n = rnd_range(1, 2);
        for (int k = 0; k < n; k++) {
            Enemy *e = enemy_alloc();
            if (!e) return;
            int x = c + rnd_range(-40, 40), yy = y + rnd_range(-20, 20);
            if (obs_collide(x, yy, EW, EH) || near_bridge(yy, EH)) continue;
            enemy_init(e, (float)x, (float)yy, enemy_pick_type());
            e->dormant = true;
        }
    }
}

/* ---- Trincheras con soldados que asoman ---- */
#define MAX_TRENCH 7
typedef struct { int16_t x, y, w, h; } Trench;
typedef struct { bool active; uint8_t t, state; int16_t x; float r; int timer, shots, shot_t; } TrSoldier;
enum { TS_DOWN, TS_RISING, TS_UP, TS_LOWERING };
static Trench    trenches[MAX_TRENCH];
static TrSoldier trs[MAX_TRENCH * 2];
static int       trench_n;

static void trench_soldier_kill(TrSoldier *s)
{
    s->active = false;
    score += 150;
    expl_add(s->x, trenches[s->t].y + 4, 12);
    sound_effect_hit();
    item_drop((float)s->x - 5, (float)trenches[s->t].y, 22);
}

/* ---- Vehículos: tanques aparcados y camiones que descargan tropas ---- */
#define MAX_VEH 5
enum { VK_TANK, VK_TRUCK };
enum { VS_DRIVE, VS_UNLOAD, VS_LEAVE };
typedef struct {
    bool active, awake;
    uint8_t kind, state;
    int8_t dir;
    float x, y, hull, tur, det_ang, tread;
    int w, h, hp, maxhp, cd, charge, flash, hit, detour, timer, stop_x, cargo, unloaded;
} Vehicle;
static Vehicle veh[MAX_VEH];
static int truck_timer, trucks_sent;

static void veh_destroy(Vehicle *v)
{
    v->active = false;
    int n = v->kind == VK_TANK ? 4 : 3;
    for (int k = 0; k < n; k++)
        expl_add(v->x + frnd(4, (float)v->w - 4), v->y + frnd(4, (float)v->h - 4), frnd(14, 26));
    sound_effect_explosion();
    if (v->kind == VK_TANK) {
        score += 800;
        item_spawn((int)v->x + v->w / 2 - 5, (int)v->y + v->h / 2 - 5, IT_GRENADES);
    } else {
        bool full = v->unloaded < v->cargo;
        score += full ? 400 : 200;
        if (full && rnd_range(0, 99) < 60)
            item_spawn((int)v->x + v->w / 2 - 5, (int)v->y + v->h / 2 - 5, rnd_range(0, 1) ? IT_GRENADES : IT_SPREAD);
    }
}

static void veh_damage(Vehicle *v, int d)
{
    if (!v->active) return;
    v->hp -= d; v->hit = 4; v->awake = true;
    if (v->hp <= 0) veh_destroy(v);
}

/* ---- Granadas, balas enemigas, explosiones, objetos ---- */
static void grenade_explode(Grenade *g)
{
    expl_add(g->tx, g->ty, g->r);
    sound_effect_explosion();
    if (g->mine) {
        for (int i = 0; i < obs_n; i++) {
            const Obstacle *o = &obs[i];
            if (!IS_STRUCT(o->type)) continue;
            float nx = g->tx < o->x ? o->x : (g->tx > o->x + o->w ? o->x + o->w : g->tx);
            float ny = g->ty < o->y ? o->y : (g->ty > o->y + o->h ? o->y + o->h : g->ty);
            if (hypotf(nx - g->tx, ny - g->ty) < g->r * 0.8f) struct_damage(i, 4);
        }
        for (int i = 0; i < MAX_ENEMIES; i++) {
            Enemy *e = &enemies[i];
            if (e->active && hypotf(e->x + EW / 2.0f - g->tx, e->y + EH / 2.0f - g->ty) < g->r) enemy_kill(e);
        }
        for (int i = 0; i < MAX_VEH; i++) {
            Vehicle *v = &veh[i];
            if (v->active && hypotf(v->x + v->w / 2.0f - g->tx, v->y + v->h / 2.0f - g->ty) < g->r + 9) veh_damage(v, 8);
        }
        for (int i = 0; i < trench_n * 2; i++) {
            TrSoldier *s = &trs[i];
            if (s->active && s->r > 0.6f && hypotf(s->x - g->tx, trenches[s->t].y + 4 - g->ty) < g->r) trench_soldier_kill(s);
        }
    } else if (!pl.dying && !player_in_tunnel() && hypotf(pl_cx() - g->tx, pl_cy() - g->ty) < g->r - 4) {
        player_hit();
    }
}

static float ang_diff(float a, float b);

static Vehicle *veh_alloc(void)
{
    for (int i = 0; i < MAX_VEH; i++) if (!veh[i].active) return &veh[i];
    return NULL;
}

static void tank_add(int x, int y)
{
    Vehicle *v = veh_alloc();
    if (!v) return;
    memset(v, 0, sizeof(*v));
    v->active = true; v->kind = VK_TANK; v->x = (float)x; v->y = (float)y; v->w = v->h = 32;
    v->hp = v->maxhp = 24; v->hull = -1.5708f; v->tur = 1.5708f; v->cd = rnd_range(100, 180);
}

static bool veh_blocked(const Vehicle *v, float nx, float ny)
{
    return obs_collide((int)nx, (int)ny, v->w, v->h) ||
           rects_hit((int)nx, (int)ny, v->w, v->h, UNFX(pl.x), UNFX(pl.y), PLAYER_W, PLAYER_H);
}

static void tank_update(Vehicle *v)
{
    int camy = cam_px();
    float cx = v->x + v->w / 2.0f, cy = v->y + v->h / 2.0f, pcx = pl_cx(), pcy = pl_cy();
    float dist = hypotf(pcx - cx, pcy - cy);
    if (!v->awake) { if (dist < 269) v->awake = true; else return; }
    if (v->y > camy + SCR_H + 90) { v->active = false; return; }
    float ang = atan2f(pcy - cy, pcx - cx);
    float d = ang_diff(v->tur, ang);
    v->tur += d > 0.035f ? 0.035f : (d < -0.035f ? -0.035f : d);

    float mx = 0, my = 0;
    if (v->detour > 0) { v->detour--; mx = cosf(v->det_ang); my = sinf(v->det_ang); }
    else if (dist > 119) { mx = cosf(ang); my = sinf(ang); }
    else if (dist < 81) { mx = -cosf(ang); my = -sinf(ang); }
    if (mx != 0 || my != 0) {
        float nx = v->x + mx * 0.25f, ny = v->y + my * 0.25f;
        if (nx < 0) nx = 0;
        if (nx > FIELD_W - v->w) nx = (float)(FIELD_W - v->w);
        bool moved = false;
        if (!veh_blocked(v, nx, v->y)) { if (nx != v->x) moved = true; v->x = nx; }
        if (!veh_blocked(v, v->x, ny)) { if (ny != v->y) moved = true; v->y = ny; }
        if (!moved && v->detour <= 0) { v->detour = rnd_range(40, 80); v->det_ang = atan2f(my, mx) + ((rng() & 1) ? 1.5708f : -1.5708f); }
        if (moved) {
            float hd = ang_diff(v->hull, atan2f(my, mx));
            v->hull += hd > 0.06f ? 0.06f : (hd < -0.06f ? -0.06f : hd);
            v->tread += 0.5f;
        }
    }
    if (v->charge > 0) {                      /* aviso: la punta del cañón parpadea en rojo */
        if (--v->charge == 0) {
            float a = v->tur + frnd(-0.06f, 0.06f);
            if (can_enemy_shoot()) {
                ebullet_fire_sp(cx + cosf(a) * 28, cy + sinf(a) * 28, a, -1, false, true, 1.06f);
                v->flash = 8;
            }
        }
    } else if (--v->cd <= 0) {
        if (!pl.dying && dist < 206 && dist > 44 && fabsf(d) < 0.18f && !los_blocked(cx, cy, pcx, pcy)) {
            if (can_enemy_shoot()) { v->charge = 28; v->cd = 180 + rnd_range(0, 80) - level * 10; if (v->cd < 110) v->cd = 110; }
            else v->cd = rnd_range(10, 30);
        } else v->cd = 10;
    }
}

static void truck_update(Vehicle *v)
{
    float sp = 0;
    if (v->state == VS_DRIVE) {
        sp = 0.72f;
        float mid = v->x + v->w / 2.0f;
        if ((v->dir > 0 && mid >= (float)v->stop_x) || (v->dir < 0 && mid <= (float)v->stop_x)) { v->state = VS_UNLOAD; v->timer = 0; sp = 0; }
    } else if (v->state == VS_UNLOAD) {
        v->timer++;
        if (v->timer % 24 == 12 && v->unloaded < v->cargo) {
            int rx = v->dir > 0 ? (int)v->x + 2 : (int)v->x + v->w - 14;
            int ry[2] = { (int)v->y + v->h + 2, (int)v->y - 17 };
            for (int k = 0; k < 2; k++) {
                if (obs_collide(rx, ry[k], EW, EH)) continue;
                Enemy *e = enemy_alloc();
                if (!e) break;
                enemy_init(e, (float)rx, (float)ry[k], enemy_pick_type());
                e->state = E_CHASE; e->shoot_cd = rnd_range(40, 90);
                v->unloaded++;
                break;
            }
        }
        if ((v->unloaded >= v->cargo && v->timer > 24 * v->cargo + 20) || v->timer > 24 * v->cargo + 200) v->state = VS_LEAVE;
    } else sp = 0.94f;
    if (sp > 0) {
        float nx = v->x + v->dir * sp;
        if (!rects_hit((int)nx, (int)v->y, v->w, v->h, UNFX(pl.x), UNFX(pl.y), PLAYER_W, PLAYER_H)) { v->x = nx; v->tread += sp; }
    }
    if (v->x < -(v->w + 30) || v->x > FIELD_W + v->w + 30) v->active = false;
}

static void trucks_spawn(void)
{
    if (fort_active || pl.dying || trucks_sent >= (1 + level > 5 ? 5 : 1 + level)) return;
    if (--truck_timer > 0) return;
    int camy = cam_px(), py = UNFX(pl.y);
    if (py > MAP_H - 437 || py < 406) { truck_timer = 60; return; }
    for (int t = 0; t < 14; t++) {
        int row = camy + rnd_range(56, SCR_H - 94);
        if (obs_collide(0, row - 12, FIELD_W, 24)) continue;
        bool clash = false;
        for (int k = 0; k < trench_n && !clash; k++) clash = row + 12 > trenches[k].y - 4 && row - 12 < trenches[k].y + trenches[k].h + 4;
        if (clash || abs(row - (py + PLAYER_H / 2)) < 39) continue;
        Vehicle *v = veh_alloc();
        if (!v) break;
        memset(v, 0, sizeof(*v));
        v->active = true; v->kind = VK_TRUCK; v->dir = (rng() & 1) ? 1 : -1; v->w = 47; v->h = 23;
        v->x = v->dir > 0 ? (float)-(v->w + 8) : (float)(FIELD_W + 8); v->y = (float)(row - v->h / 2);
        v->hp = v->maxhp = 8; v->stop_x = rnd_range(60, FIELD_W - 60);
        v->cargo = 3 + (level >= 3 ? 1 : 0);
        trucks_sent++;
        truck_timer = 1100 + rnd_range(0, 600) - level * 80; if (truck_timer < 700) truck_timer = 700;
        sound_effect_alarm();
        return;
    }
    truck_timer = 60;
}

static void vehicles_update(void)
{
    for (int i = 0; i < MAX_VEH; i++) {
        Vehicle *v = &veh[i];
        if (!v->active) continue;
        if (v->hit > 0) v->hit--;
        if (v->flash > 0) v->flash--;
        if (v->kind == VK_TANK) tank_update(v); else truck_update(v);
    }
    trucks_spawn();
}

static float ang_diff(float a, float b)
{
    float d = b - a;
    while (d > 3.14159f) d -= 6.28318f;
    while (d < -3.14159f) d += 6.28318f;
    return d;
}

static void structures_update(void)
{
    int camy = cam_px();
    float pcx = pl_cx(), pcy = pl_cy();
    for (int i = 0; i < obs_n; i++) {
        Obstacle *o = &obs[i];
        ObAux *a = &aux[i];
        if (o->type == OB_BUNKER) {
            if (a->flash > 0) a->flash--;
            float cx = o->x + o->w / 2.0f, cy = o->y + 15.0f;
            float dist = hypotf(pcx - cx, pcy - cy);
            if (!(o->y > camy - 40 && o->y < camy + SCR_H + 40) || pl.dying || dist > 200) { a->charge = 0; continue; }
            float d = ang_diff(a->ang, atan2f(pcy - cy, pcx - cx));
            a->ang += d > 0.03f ? 0.03f : (d < -0.03f ? -0.03f : d);   /* la torreta apunta despacio */
            if (a->charge > 0) {
                if (--a->charge == 0) {
                    if (!can_enemy_shoot()) { a->cd = 40; continue; }
                    a->flash = 8; a->cd = rnd_range(170, 250);
                    ebullet_fire_ex(cx + cosf(a->ang) * 19, cy + sinf(a->ang) * 19, a->ang, i, false, true);
                }
                continue;
            }
            if (a->cd > 0) a->cd--;
            else if (fabsf(d) < 0.2f && dist > 31) a->charge = 32;      /* aviso parpadeante antes de disparar */
        } else if (o->type == OB_BARRACKS) {
            bool on = o->y > camy - 25 && o->y < camy + SCR_H;
            if (a->cd > 0) { if (on) a->cd--; continue; }
            int act = 0;
            for (int k = 0; k < MAX_ENEMIES; k++)
                if (enemies[k].active && !enemies[k].anchored && !enemies[k].dormant && !enemies[k].stat && !enemies[k].escort) act++;
            if (a->spawned >= 3 + level || act >= 12 || pl.dying) { a->cd = 90; continue; }
            int ex = o->x + o->w / 2 - 6 + rnd_range(-5, 5), ey = o->y + o->h + 2;
            Enemy *e = enemy_alloc();
            if (!e || obs_collide(ex, ey, EW, EH)) { a->cd = 50; continue; }
            enemy_init(e, (float)ex, (float)ey, enemy_pick_type());
            e->state = E_CHASE; e->shoot_cd = rnd_range(50, 100);
            a->spawned++;
            a->cd = rnd_range(320, 520) - level * 20; if (a->cd < 160) a->cd = 160;
        }
    }
}

static void trenches_generate(void)
{
    trench_n = 0;
    memset(trs, 0, sizeof(trs));
    int want = 4 + level / 2; if (want > MAX_TRENCH) want = MAX_TRENCH;
    for (int n = 0; n < want; n++) {
        for (int tries = 0; tries < 12; tries++) {
            int y = rnd_range(406, MAP_H - 320);
            int x = rnd_range(8, FIELD_W - 68);          /* se puede cruzar: da igual que caiga sobre el sendero */
            if (near_bridge(y, 18)) continue;
            bool clash = obs_collide(x - 6, y - 6, 72, 30);
            for (int k = 0; k < trench_n && !clash; k++)
                clash = rects_hit(x - 6, y - 30, 72, 78, trenches[k].x, trenches[k].y, trenches[k].w, trenches[k].h);
            if (clash) continue;
            trenches[trench_n] = (Trench){ (int16_t)x, (int16_t)y, 60, 18 };
            for (int k = 0; k < 2; k++)
                trs[trench_n * 2 + k] = (TrSoldier){ true, (uint8_t)trench_n, TS_DOWN, (int16_t)(x + 15 + k * 30), 0,
                                                       rnd_range(60, 260), 0, 0 };
            trench_n++;
            break;
        }
    }
}

static void trenches_update(void)
{
    int camy = cam_px();
    float pcx = pl_cx(), pcy = pl_cy();
    for (int i = 0; i < trench_n * 2; i++) {
        TrSoldier *s = &trs[i];
        if (!s->active) continue;
        const Trench *t = &trenches[s->t];
        float dist = hypotf(pcx - s->x, pcy - (t->y + 9));
        bool on = t->y > camy - 25 && t->y < camy + SCR_H + 25;
        switch (s->state) {
        case TS_DOWN:
            s->r -= 0.06f; if (s->r < 0) s->r = 0;
            if (--s->timer <= 0) { if (dist < 187 && on) s->state = TS_RISING; else s->timer = 30; }
            break;
        case TS_RISING:
            s->r += 0.045f;
            if (s->r >= 1) { s->r = 1; s->state = TS_UP; s->timer = rnd_range(85, 115); s->shots = 2; s->shot_t = 12; }
            break;
        case TS_UP:
            s->timer--; s->shot_t--;
            if (s->shots > 0 && s->shot_t <= 0 && !pl.dying && can_enemy_shoot()) {
                s->shots--; s->shot_t = 32;
                ebullet_fire((float)s->x, (float)t->y, atan2f(pcy - t->y, pcx - s->x) + frnd(-0.25f, 0.25f), -1);
            }
            if (s->timer <= 0) s->state = TS_LOWERING;
            break;
        default:
            s->r -= 0.07f;
            if (s->r <= 0) { s->r = 0; s->state = TS_DOWN; s->timer = rnd_range(130, 270); }
            break;
        }
    }
}

static bool player_in_tunnel(void)
{
    float cx = pl_cx(), cy = pl_cy();
    return cx > tunnel_x && cx < tunnel_x + TUNNEL_W && cy > bridge_y && cy < bridge_y + BR_H;
}

static void fort_update(void)
{
    if (!fort_active && UNFX(pl.y) < FORT_TRIGGER_Y) {            /* se abre la puerta */
        fort_active = true;
        obs[gate_idx].type = OB_RUBBLE; obs[gate_idx].hp = 0;
        sound_effect_alarm();
    }
    if (!fort_active) return;
    fort_timer++;
    if (fort_spawned < FORT_ENEMIES && fort_timer >= 85) {
        /* 1/3 por la puerta, 1/3 por cada lado del campo (bajo los muros, a la vista) */
        int side = rnd_range(0, 2);
        float sx, sy;
        if (side == 0) { sx = (float)(fort_x + 4) + frnd(0, FORT_DOOR_W - 20); sy = 52; }
        else { sx = (side == 1) ? 3.0f : (float)(FIELD_W - EW - 3); sy = frnd(118, 205); }
        bool blocked = (side != 0 && obs_collide((int)sx, (int)sy, EW, EH));
        for (int i = 0; i < MAX_ENEMIES; i++)
            if (enemies[i].active && fabsf(enemies[i].x - sx) < 12 && fabsf(enemies[i].y - sy) < 16) blocked = true;
        Enemy *e = enemy_alloc();
        if (!blocked && e) {
            fort_timer = 0; fort_spawned++;
            enemy_init(e, sx, sy, enemy_pick_type());
            e->fort = true; e->state = E_CHASE; e->alerted = true;
            e->rusher = rnd_range(0, 99) < 25;
            e->nade_cd = rnd_range(150, 300);
            e->hold_dir = (rng() & 1) ? 1.0f : -1.0f; e->hold_flip = rnd_range(40, 100);
        }
    }
    if (fort_killed >= FORT_ENEMIES && state == ST_PLAYING) {
        state = ST_WIN; win_t = 0;
        sound_start_scramble_victory();
        score += 5000 + (uint32_t)pl.lives * 500;
    }
}

/* ---- Moto bombardera sobre el puente ---- */
typedef struct { float x; int dir, wait, bomb_cd, g_cd, throw_anim; } Bike;
static Bike bike;

static void bike_update(void)
{
    float by = bridge_y + BR_H / 2.0f;
    if (bike.wait > 0) bike.wait--;
    else {
        bike.x += bike.dir * 0.94f;
        if (bike.x > FIELD_W + 30) { bike.dir = -1; bike.wait = 120; }
        else if (bike.x < -30) { bike.dir = 1; bike.wait = 120; }
    }
    if (bike.bomb_cd > 0) bike.bomb_cd--;
    if (bike.throw_anim > 0) bike.throw_anim--;
    if (bike.g_cd > 0) bike.g_cd--;
    bool near = abs(UNFX(pl.y) - bridge_y) < 206 && !pl.dying && !player_in_tunnel();
    bool on = bike.wait <= 0 && bike.x > 5 && bike.x < FIELD_W - 5 && near;
    if (on && bike.bomb_cd <= 0 && can_enemy_throw()) {
        float tx = pl_cx() + frnd(-19, 19), ty = pl_cy() + frnd(-19, 19);
        if (tx < 10) tx = 10;
        if (tx > FIELD_W - 10) tx = (float)(FIELD_W - 10);
        grenade_throw_ex(bike.x, by, tx, ty, false, 0.011f, 16.0f, 27.0f);
        bike.bomb_cd = rnd_range(110, 180);
    }
    if (on && bike.g_cd <= 0 && can_enemy_throw()) {
        float gx = pl_cx() + pl.vx * 16 + frnd(-15, 15), gy = pl_cy() + pl.vy * 16 + frnd(-15, 15);
        if (gx < 10) gx = 10;
        if (gx > FIELD_W - 10) gx = (float)(FIELD_W - 10);
        grenade_throw_ex(bike.x, by + 12, gx, gy, false, 0.012f, 22.0f, 24.0f);
        bike.throw_anim = 14;
        bike.g_cd = rnd_range(150, 230);
    }
}

static void hostages_update(void)
{
    for (int i = 0; i < MAX_HOSTAGES; i++) {
        Hostage *h = &hostages[i];
        if (!h->active) continue;
        if (h->freed) { if (++h->t > 170) h->active = false; continue; }
        int alive = 0; float ax = 0, ay = 0;
        for (int k = 0; k < MAX_ENEMIES; k++) {
            const Enemy *e = &enemies[k];
            if (!e->active || e->escort != i + 1) continue;
            alive++;
            if (!e->dormant) { ax += e->x + EW / 2.0f; ay += e->y + EH - 15; }
            else { ax += h->x + 6; ay += h->y; }
        }
        if (alive > 0) {
            ax = ax / alive - 6; ay /= alive;
            float nx = h->x + (ax - h->x) * 0.1f, ny = h->y + (ay - h->y) * 0.1f;
            if (nx > 3 && nx < FIELD_W - 15 && !obs_collide((int)nx, (int)ny, 12, 15)) { h->x = nx; h->y = ny; }
        } else if (h->killed >= 2) {
            h->freed = true; h->t = 0; score += 1000;
            sound_effect_extra_life();
        } else h->active = false;                    /* los guardias huyeron: rehén perdido */
    }
}

static void update_world(void)
{
    hostages_update();
    fort_update();
    bike_update();
    structures_update();
    trenches_update();
    vehicles_update();
    enemies_spawn();
    for (int i = 0; i < MAX_ENEMIES; i++)
        if (enemies[i].active) enemy_update(&enemies[i], i);

    for (int i = 0; i < MAX_EBULLETS; i++) {
        EBullet *b = &ebullets[i];
        if (!b->active) continue;
        b->x += b->vx; b->y += b->vy;
        b->dist += b->sp;
        int camy = cam_px();
        float range = b->rocket ? 181.0f : (b->shell ? 212.0f : EB_RANGE);
        if (b->dist >= range || point_solid((int)b->x, (int)b->y, b->ignore)) {
            b->active = false;
            if (b->rocket) rocket_detonate(b);
            else if (b->shell) { expl_add(b->x, b->y, 12); sound_effect_explosion(); }
            continue;
        }
        if (b->x < 0 || b->x >= FIELD_W || b->y < camy - 12 || b->y > camy + SCR_H + 12) { b->active = false; continue; }
        if (!pl.dying && pl.invuln <= 0 &&
            rects_hit((int)b->x - 1, (int)b->y - 1, 3, 3, UNFX(pl.x) + 1, UNFX(pl.y) + 1, PLAYER_W - 2, PLAYER_H - 2)) {
            b->active = false;
            if (b->rocket) rocket_detonate(b);
            else { if (b->shell) { expl_add(b->x, b->y, 12); sound_effect_explosion(); } player_hit(); }
        }
    }

    for (int i = 0; i < MAX_GRENADES; i++) {
        Grenade *g = &grenades[i];
        if (!g->active) continue;
        g->prog += g->speed;
        if (g->prog >= 1.0f) { g->active = false; grenade_explode(g); }
    }

    for (int i = 0; i < MAX_EXPL; i++) {
        Explosion *x = &booms[i];
        if (!x->active) continue;
        x->r += 1.1f;
        if (x->r >= x->maxr) x->active = false;
    }

    for (int i = 0; i < MAX_ITEMS; i++) {
        Item *it = &items[i];
        if (!it->active || pl.dying) continue;
        if (!rects_hit(UNFX(pl.x), UNFX(pl.y), PLAYER_W, PLAYER_H, it->x, it->y, 10, 10)) continue;
        it->active = false;
        if (it->type == IT_SPREAD) { pl.spread = 450; sound_effect_powerup(); }
        else if (it->type == IT_GRENADES) { pl.grenades += 5; sound_effect_powerup(); }
        else { if (pl.lives < 10) pl.lives++; sound_effect_extra_life(); }
    }
}

static void world_init(void)
{
    obs_generate();
    trenches_generate();
    memset(veh, 0, sizeof(veh));
    {
        int nt = 1 + (level - 1) / 2; if (nt > 3) nt = 3;
        int ys[3], placed = 0;
        for (int tries = 0; tries < 40 && placed < nt; tries++) {
            int x = rnd_range(10, FIELD_W - 42), y = rnd_range(562, MAP_H - 500);
            if (obs_collide(x - 4, y - 4, 40, 40) || near_bridge(y, 32)) continue;
            bool far = true;
            for (int k = 0; k < placed; k++) if (abs(ys[k] - y) < 312) far = false;
            if (!far) continue;
            bool clash = false;
            for (int k = 0; k < trench_n && !clash; k++)
                clash = rects_hit(x - 4, y - 4, 40, 40, trenches[k].x - 4, trenches[k].y - 4, trenches[k].w + 8, trenches[k].h + 8);
            if (clash) continue;
            ys[placed++] = y; tank_add(x, y);
        }
    }
    truck_timer = rnd_range(500, 900); trucks_sent = 0;
    fort_active = false; fort_spawned = fort_killed = fort_timer = 0;
    bike = (Bike){ -25.0f, 1, 60, 120, 200, 0 };
    memset(ebullets, 0, sizeof(ebullets));
    memset(grenades, 0, sizeof(grenades));
    memset(booms, 0, sizeof(booms));
    memset(items, 0, sizeof(items));
    enemies_populate();
    for (int y = MAP_H - 500, n = 0; y > 220 && n < 6; y -= rnd_range(280, 380), n++) {
        int x = path_center(y) + rnd_range(-30, 30);
        if (obs_collide(x, y, 10, 10) || near_bridge(y, 10)) continue;
        items[n] = (Item){ true, (int16_t)x, (int16_t)y, IT_GRENADES };
    }
    spawn_timer = 0;
}


/* ---------------------------------------------------------
 * Inicio de partida
 * --------------------------------------------------------- */
static void game_init(bool next)
{
    const int keep_lives = pl.lives, keep_nades = pl.grenades;
    const uint32_t keep_score = score;
    path_generate(level);
    theme = &THEMES[(level - 1) % 3];

    memset(&pl, 0, sizeof(pl));
    pl.x = FX(FIELD_W / 2 - PLAYER_W / 2);
    pl.y = FX(MAP_H - 75);
    pl.dir_x = 0; pl.dir_y = -1;
    pl.lives = 10;
    pl.grenades = 8;
    pl.fire_cd = 0; fire_buf = 0;

    memset(pbullets, 0, sizeof(pbullets));
    world_init();
    gameover_t = 0;
    cam_q8 = FX(MAP_H - SCR_H);
    score = 0;
    if (next) {                         /* siguiente fase: una vida de premio y 4 granadas */
        pl.lives = keep_lives + 1 > 10 ? 10 : keep_lives + 1;
        pl.grenades = keep_nades + 4;
        score = keep_score;
    }
    panels_ready = false;
}

/* ---------------------------------------------------------
 * Lógica
 * --------------------------------------------------------- */
static void pbullet_add(int32_t vx, int32_t vy)
{
    for (int i = 0; i < MAX_PBULLETS; i++) {
        Bullet *b = &pbullets[i];
        if (b->active) continue;
        b->active = true;
        b->x = pl.x + FX(PLAYER_W / 2);
        b->y = pl.y + FX(PLAYER_H / 2 - 2);
        b->vx = vx; b->vy = vy;
        b->life = BULLET_LIFE;
        b->rolled = 0;
        return;
    }
}

static bool chomp_alt;

static void player_shoot(void)
{
    int dx = pl.dir_x, dy = pl.dir_y;
    if (dx == 0 && dy == 0) dy = -1;
    int sp = (dx && dy) ? BSPEED_D : BSPEED;
    if (pl.spread > 0) {                       /* disparo triple */
        float a = atan2f((float)dy, (float)dx);
        for (int k = -1; k <= 1; k++)
            pbullet_add((int32_t)(cosf(a + 0.25f * (float)k) * BSPEED), (int32_t)(sinf(a + 0.25f * (float)k) * BSPEED));
    } else {
        pbullet_add(dx * sp, dy * sp);
    }
    pl.noise = 40;
    sound_effect_pacman_chomp(chomp_alt);       /* el chomp de Pac-Man, alternando tic/tac */
    chomp_alt = !chomp_alt;
}

static void player_throw(void)
{
    float dx = (float)pl.dir_x, dy = (float)pl.dir_y;
    if (pl.dir_x == 0 && pl.dir_y == 0) dy = -1;
    if (pl.dir_x && pl.dir_y) { dx *= 0.7071f; dy *= 0.7071f; }
    float cx = pl_cx(), cy = pl_cy();
    grenade_throw(cx, cy, cx + dx * 69, cy + dy * 69, true);
    pl.grenades--;
    pl.noise = 70;
}

static void player_respawn(void)
{
    pl.dying = false;
    pl.invuln = INVULN_STEPS;
    pl.fire_cd = 20; pl.nade_cd = 40; pl.spread = 0;
    float px = pl_cx(), py = pl_cy();
    for (int i = 0; i < MAX_EBULLETS; i++)
        if (ebullets[i].active && hypotf(ebullets[i].x - px, ebullets[i].y - py) < 80) ebullets[i].active = false;
    for (int i = 0; i < MAX_GRENADES; i++)
        if (grenades[i].active && !grenades[i].mine && hypotf(grenades[i].tx - px, grenades[i].ty - py) < 44) grenades[i].active = false;
}

/* los vehículos son sólidos para el jugador (salvo que ya se solape con uno al aparecer) */
static bool veh_block(int nx, int ny)
{
    for (int i = 0; i < MAX_VEH; i++) {
        const Vehicle *v = &veh[i];
        if (!v->active) continue;
        if (rects_hit(nx, ny, PLAYER_W, PLAYER_H, (int)v->x, (int)v->y, v->w, v->h) &&
            !rects_hit(UNFX(pl.x), UNFX(pl.y), PLAYER_W, PLAYER_H, (int)v->x, (int)v->y, v->w, v->h)) return true;
    }
    return false;
}

static void update_player(void)
{
    if (pl.dying) {
        pl.vx = pl.vy = 0;
        if (--pl.death_t <= 0) {
            if (pl.lives <= 0) { state = ST_GAMEOVER; gameover_t = GAME_SPEED * 4; sound_effect_game_over(); }
            else player_respawn();
        }
        return;
    }
    int jx = controls_joy_x();
    int jy = controls_joy_y();
    int ax = abs(jx), ay = abs(jy);
    int m = ax > ay ? ax : ay;
    pl.vx = pl.vy = 0;

    if (m > 0) {
        /* Dirección de apuntado en 8 sentidos: un eje cuenta si supera
         * el 40% del mayor. */
        pl.dir_x = (ax * 5 > m * 2) ? (jx > 0 ? 1 : -1) : 0;
        pl.dir_y = (ay * 5 > m * 2) ? (jy > 0 ? 1 : -1) : 0;

        int32_t vx = (int32_t)jx * PLAYER_SPEED / JOY_MAX;
        int32_t vy = (int32_t)jy * PLAYER_SPEED / JOY_MAX;
        int32_t mag = (ax > ay ? ax : ay) + (ax > ay ? ay : ax) * 3 / 8;
        if (mag > JOY_MAX) { vx = vx * JOY_MAX / mag; vy = vy * JOY_MAX / mag; }

        int32_t nx = pl.x + vx;
        int32_t ny = pl.y + vy;
        int camy = cam_px();

        if (UNFX(nx) >= 3 && UNFX(nx) + PLAYER_W <= FIELD_W - 3 &&
            !obs_collide(UNFX(nx), UNFX(pl.y), PLAYER_W, PLAYER_H) && !veh_block(UNFX(nx), UNFX(pl.y))) { pl.vx = (float)vx / 256.0f; pl.x = nx; }
        if (UNFX(ny) >= camy + (SIDE_PANELS ? 0 : HUD_H) + 2 && UNFX(ny) + PLAYER_H <= camy + SCR_H - 8 &&
            !obs_collide(UNFX(pl.x), UNFX(ny), PLAYER_W, PLAYER_H) && !veh_block(UNFX(pl.x), UNFX(ny))) { pl.vy = (float)vy / 256.0f; pl.y = ny; }

        pl.anim += 2;
    }

    if (pl.fire_cd > 0) pl.fire_cd--;
    if (pl.nade_cd > 0) pl.nade_cd--;
    if (pl.invuln > 0) pl.invuln--;
    if (pl.noise > 0) pl.noise--;
    if (pl.spread > 0) pl.spread--;

    if (fire_buf > 0) fire_buf--;
    if (fire_buf > 0 && pl.fire_cd <= 0) {
        fire_buf = 0;
        player_shoot();
        pl.fire_cd = FIRE_CD;
    }
    if (in_nade_down && pl.nade_cd <= 0 && pl.grenades > 0) {
        player_throw();
        pl.nade_cd = 40;
    }
}

static void update_bullets(void)
{
    int camy = cam_px();
    for (int i = 0; i < MAX_PBULLETS; i++) {
        Bullet *b = &pbullets[i];
        if (!b->active) continue;
        b->x += b->vx;
        b->y += b->vy;
        int px = UNFX(b->x), py = UNFX(b->y);
        if (--b->life <= 0 || px < 0 || px >= FIELD_W || py < camy - 10 || py > camy + SCR_H + 10) { b->active = false; continue; }
        {
            int oi = point_solid_idx(px, py, -1);
            if (oi >= 0) {
                if (obs[oi].type == OB_TENT) struct_damage(oi, 1);   /* las tiendas aguantan balas: 1/4 de punto */
                b->active = false;
                continue;
            }
        }
        for (int k = 0; k < MAX_VEH; k++) {
            Vehicle *v = &veh[k];
            if (v->active && rects_hit(px - 2, py - 2, 4, 4, (int)v->x, (int)v->y, v->w, v->h)) {
                veh_damage(v, 1);
                expl_add((float)px, (float)py, 6);
                b->active = false;
                break;
            }
        }
        if (!b->active) continue;
        for (int k = 0; k < trench_n * 2; k++) {
            TrSoldier *s = &trs[k];
            if (s->active && s->r > 0.6f && rects_hit(px - 2, py - 2, 4, 4, s->x - 5, trenches[s->t].y + 1, 10, 9)) {
                trench_soldier_kill(s);
                b->active = false;
                break;
            }
        }
        if (!b->active) continue;
        for (int k = 0; k < MAX_ENEMIES; k++) {
            Enemy *e = &enemies[k];
            if (e->active && !e->dormant &&
                rects_hit(px - 2, py - 2, 4, 4, (int)e->x, (int)e->y + (e->anchored && e->peek == 0 ? 99 : 0), EW, EH)) {
                enemy_kill(e);
                b->active = false;
                break;
            }
        }
    }
}

static void update_camera(void)
{
    /* Solo avanza hacia arriba, suavizado (~0.05 por paso como en el HTML). */
    int32_t target = pl.y - FX((SCR_H * 65) / 100);
    if (target < cam_q8) cam_q8 += (target - cam_q8) * 13 / 256;
    if (cam_q8 < 0) cam_q8 = 0;
    if (cam_q8 > FX(MAP_H - SCR_H)) cam_q8 = FX(MAP_H - SCR_H);
}

static void logic_step(void)
{
    if (state == ST_TITLE) {
        title_t++;
        if (in_start_edge) {
            level = 1;
            game_init(false);
            state = ST_PLAYING;
        }
        return;
    }
    if (state == ST_PLAYING) {
        update_camera();
        update_player();
        update_bullets();
        update_world();
        return;
    }
    if (state == ST_WIN) {
        update_world();
        if (++win_t > GAME_SPEED * 3 && in_start_edge) { sound_stop_scramble_victory(); level++; game_init(true); state = ST_PLAYING; }
        return;
    }
    if (state == ST_GAMEOVER) {
        update_world();
        if (--gameover_t <= 0) {
            if (!g_demo && highscores_is_top(CMD_GAME_ID, score))
                highscores_enter(CMD_GAME_ID, score);          /* bloqueante, como en Scramble */
            state = ST_SCORES; scores_t = 0; scores_drawn = false;
        }
        return;
    }
    if (state == ST_SCORES) {
        if (++scores_t > GAME_SPEED * 8 || (scores_t > GAME_SPEED && in_start_edge)) g_done = true;   /* vuelta al menú */
    }
}

/* ---------------------------------------------------------
 * Dibujo
 * --------------------------------------------------------- */
/* Coordenadas absolutas de pantalla, recortadas a pantalla (el driver no
 * recorta por arriba/izquierda). */
static inline void rect_abs(int x, int y, int w, int h, uint16_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCR_W) w = SCR_W - x;
    if (y + h > SCR_H) h = SCR_H - y;
    if (w <= 0 || h <= 0) return;
    renderer_fill_rect((uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h, c);
}

/* Coordenadas del campo de juego (0..FIELD_W), recortadas al campo y
 * desplazadas FIELD_X: nada de lo dibujado con rect() se sale a los paneles. */
static inline void rect(int x, int y, int w, int h, uint16_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (x + w > FIELD_W) w = FIELD_W - x;
    if (w <= 0) return;
    rect_abs(x + FIELD_X, y, w, h, c);
}

/* Fondo simplificado (CMD_SIMPLE_BG 1): zonas planas grandes y pocos detalles.
 * Con scroll vertical, una zona de color uniforme no cambia de un frame al
 * siguiente, así que el volcado solo reenvía los bordes y los detalles que se
 * mueven. 0 = fondo detallado original. */
#ifndef CMD_SIMPLE_BG
#define CMD_SIMPLE_BG 1
#endif

#if CMD_SIMPLE_BG
static void draw_terrain(void)
{
    const Theme *t = theme;
    const int camy = cam_px();
    const int STEP = 4;

    rect(0, 0, FIELD_W, SCR_H, t->grass);

    /* Pocas manchas de hierba, rectángulos sencillos de un solo tono */
    const int CELL = 48;
    int c0 = camy / CELL - 1, c1 = (camy + SCR_H) / CELL + 1;
    for (int cy = c0; cy <= c1; cy++) {
        for (int cx = 0; cx < (FIELD_W + CELL - 1) / CELL; cx++) {
            int r = hash8(cx, cy);
            if (r > 110) continue;
            uint16_t col = (r < 60) ? t->pA : t->pB;
            int px = cx * CELL + hash8(cx + 11, cy) * CELL / 256;
            int py = cy * CELL + hash8(cx, cy + 7) * CELL / 256 - camy;
            rect(px - 6, py - 3, 12, 6, col);
        }
    }

    /* Sendero: borde + tierra, sin irregularidades; las franjas contiguas con
     * los mismos límites se dibujan como un solo rectángulo. */
    int startY = (camy / STEP) * STEP;
    int run_y = 0, run_h = 0, run_l = 0, run_w = 0;
    for (int sy = startY; sy < camy + SCR_H + STEP; sy += STEP) {
        int c = path_center(sy), h = path_half(sy);
        int left = c - h, w = 2 * h;
        int y = sy - camy;
        if (run_h && left == run_l && w == run_w) { run_h += STEP; continue; }
        if (run_h) {
            rect(run_l - 3, run_y, run_w + 6, run_h, t->edge1);
            rect(run_l,     run_y, run_w,     run_h, t->dirt);
        }
        run_y = y; run_h = STEP; run_l = left; run_w = w;
    }
    if (run_h) {
        rect(run_l - 3, run_y, run_w + 6, run_h, t->edge1);
        rect(run_l,     run_y, run_w,     run_h, t->dirt);
    }
}
#else
static void draw_terrain(void)
{
    const Theme *t = theme;
    const int camy = cam_px();
    const int STEP = 3;

    rect(0, 0, FIELD_W, SCR_H, t->grass);

    /* Manchas de hierba clara/oscura y tierra suelta: celdas fijas del mundo
     * (30 px = 48 * 0.625), dibujadas como "óvalos" de 2 rectángulos. */
    const int CELL = 30;
    int c0 = camy / CELL - 1, c1 = (camy + SCR_H) / CELL + 1;
    for (int cy = c0; cy <= c1; cy++) {
        for (int cx = 0; cx < (FIELD_W + CELL - 1) / CELL; cx++) {
            int r = hash8(cx, cy);
            uint16_t col;
            if (r < 90) col = t->pA;
            else if (r < 159) col = t->pB;
            else if (r < 192) col = t->pC;
            else continue;
            int px = cx * CELL + hash8(cx + 11, cy) * CELL / 256;
            int py = cy * CELL + hash8(cx, cy + 7) * CELL / 256 - camy;
            int rw = 5 + hash8(cx + 3, cy + 5) * 11 / 256;
            int rh = 3 + hash8(cx + 9, cy + 2) * 6 / 256;
            rect(px - rw, py - rh + 1, rw * 2, rh * 2 - 2, col);
            rect(px - rw + 2, py - rh, rw * 2 - 4, rh * 2, col);
        }
    }

    /* Sendero por franjas con bordes irregulares */
    int startY = (camy / STEP) * STEP;
    for (int sy = startY; sy < camy + SCR_H + STEP; sy += STEP) {
        int k = sy / STEP;
        int c = path_center(sy), h = path_half(sy);
        int left  = c - h + (hash8(k, 1) - 128) * 6 / 256;
        int right = c + h + (hash8(k, 2) - 128) * 6 / 256;
        int w = right - left;
        int y = sy - camy;

        rect(left - 5, y, w + 10, STEP, t->edge1);
        rect(left - 2, y, w + 4,  STEP, t->edge2);
        rect(left,     y, w,      STEP, t->dirt);

        int tt = hash8(k, 3);
        if (tt > 140)
            rect(left + w * tt * 4 / 1000, y, w * 3 / 10, STEP, t->dirtL);

        if (hash8(sy / 25, 9) > 64) {             /* roderas */
            rect(c - 9, y, 2, STEP, t->rut);
            rect(c + 7, y, 2, STEP, t->rut);
        }
        if (hash8(k, 4) > 205)                    /* piedrecitas */
            rect(left + hash8(k, 5) * w / 256, y + 1, 1, 1, C_PEBBLE);
        if (hash8(k, 6) > 230) {                  /* matas que invaden el camino */
            int gx = (hash8(k, 7) > 128 ? left : right) + (hash8(k, 8) - 128) / 22;
            rect(gx, y - 1, 4, 3, t->tuft);
        }
    }
}
#endif /* CMD_SIMPLE_BG */

/* Fusil: línea de 2x2 desde el centro en la dirección de apuntado */
static void draw_rifle(int cx, int cy)
{
    int dx = pl.dir_x, dy = pl.dir_y;
    if (dx == 0 && dy == 0) dy = -1;
    for (int i = 4; i <= 10; i += 2)
        rect(cx + dx * i - 1, cy + dy * i - 1, 2, 2, C_GUN);
}

static void draw_player(void)
{
    if ((pl.invuln % 6) > 3) return;                 /* parpadeo si invulnerable */
    int cx = UNFX(pl.x) + PLAYER_W / 2;
    int cy = UNFX(pl.y) - cam_px() + PLAYER_H / 2;
    int leg = (isin((uint32_t)pl.anim * 4) * 2) / 1024;   /* -2..2 */

    rect(cx - 5, cy - 6, 10, 12, C_BODY);            /* cuerpo */
    rect(cx - 3, cy - 8, 6, 5, C_SKIN);              /* cabeza */
    rect(cx - 4, cy - 8, 8, 2, C_BAND);              /* cinta roja */
    rect(cx - 4, cy + 5, 3, 4 + leg, C_LEGS);        /* piernas */
    rect(cx + 1, cy + 5, 3, 4 - leg, C_LEGS);
    draw_rifle(cx, cy);
}

static void draw_bullets(void)
{
    int camy = cam_px();
    for (int i = 0; i < MAX_PBULLETS; i++) {
        const Bullet *b = &pbullets[i];
        if (!b->active) continue;
        rect(UNFX(b->x) - 1, UNFX(b->y) - camy - 1, 3, 3, C_BULLET);
    }
}


/* ---------------------------------------------------------
 * Dibujo de la fase 2
 * --------------------------------------------------------- */
static const uint16_t TREE_COL[3][2] = {
    { RGB(0x2c,0x5a,0x1e), RGB(0x3f,0x7a,0x2a) },
    { RGB(0x8a,0x7a,0x3a), RGB(0xa0,0x8c,0x46) },
    { RGB(0x1f,0x3d,0x18), RGB(0x2e,0x5a,0x22) },
};
#define C_TRUNK   RGB(0x4a,0x32,0x1a)
#define C_ROCK    RGB(0x7a,0x7a,0x72)
#define C_ROCK_HI RGB(0x9a,0x9a,0x92)
#define C_BAG     RGB(0xb5,0x9a,0x62)
#define C_BAG_DK  RGB(0x8a,0x72,0x44)
#define C_EBULLET RGB(0xff,0x44,0x00)

static void draw_structure(const Obstacle *o, int x, int y)
{
    int w = o->w, h = o->h;
    switch (o->type) {
    case OB_TENT:
        rect(x, y + h / 3, w, h * 2 / 3, RGB(0x6a,0x62,0x3a));
        rect(x + 3, y + 1, w - 6, h / 3, RGB(0x8a,0x82,0x52));
        rect(x + w / 2 - 3, y + h / 2, 6, h / 2, RGB(0x20,0x1c,0x10));
        break;
    case OB_HOUSE:
        rect(x, y + h * 4 / 10, w, h * 6 / 10, RGB(0xb0,0x9a,0x78));
        rect(x - 1, y, w + 2, h * 4 / 10, RGB(0x9a,0x3a,0x2a));
        rect(x + 4, y + h * 6 / 10, 6, h * 4 / 10, RGB(0x4a,0x32,0x1a));
        rect(x + w - 12, y + h * 5 / 10, 6, 6, RGB(0x2a,0x3a,0x4a));
        break;
    case OB_BARRACKS:
        rect(x, y + h * 3 / 10, w, h * 7 / 10, RGB(0x5a,0x60,0x48));
        rect(x - 1, y, w + 2, h * 3 / 10, RGB(0x3a,0x40,0x30));
        for (int k = 6; k < w - 8; k += 14) rect(x + k, y + h / 2, 7, 6, RGB(0x1a,0x22,0x2a));
        break;
    default:
        rect(x, y, w, h, RGB(0x7a,0x7a,0x76));
        rect(x, y, w, 5, RGB(0x4a,0x4a,0x48));
        for (int r = 9; r < h - 6; r += 12)
            for (int k = 5; k < w - 8; k += 12) rect(x + k, y + r, 6, 7, RGB(0x1a,0x22,0x2a));
        break;
    }
    if (o->hp < o->maxhp) {                                  /* grietas según el daño */
        int dmg = (o->maxhp - o->hp) * 4 / o->maxhp + 1;
        for (int k = 0; k < dmg; k++)
            rect(x + 3 + (o->v * (k + 3)) % (w - 6), y + 2 + (o->v * (k + 5)) % (h - 4), 3, 2, RGB(0x18,0x14,0x10));
    }
}

static bool on_screen(int y, int h, int camy) { return y + h > camy - 24 && y < camy + SCR_H + 24; }

static int g_ylim = 100000;                     /* límite inferior de dibujo (soldados tras sacos) */
static void erect(int x, int y, int w, int h, uint16_t c)
{
    if (y >= g_ylim) return;
    if (y + h > g_ylim) h = g_ylim - y;
    rect(x, y, w, h, c);
}

static void draw_bunker(const Obstacle *o, int idx, int x, int y)
{
    const ObAux *a = &aux[idx];
    rect(x, y, o->w, o->h, RGB(0x55,0x55,0x55));
    rect(x, y, o->w, 3, RGB(0x6a,0x6a,0x6a));
    rect(x, y + o->h - 3, o->w, 3, RGB(0x44,0x44,0x44));
    rect(x + 4, y + 21, 9, 5, RGB(0x22,0x22,0x22));
    rect(x + o->w - 13, y + 21, 9, 5, RGB(0x22,0x22,0x22));
    if (o->hp <= 8) { rect(x + 5, y + 5, 9, 3, RGB(0x3b,0x3b,0x3b)); rect(x + 27, y + 12, 6, 4, RGB(0x3b,0x3b,0x3b)); }
    if (o->hp <= 4) { rect(x + 16, y + 3, 2, 14, RGB(0x1f,0x1f,0x1f)); rect(x + 18, y + 15, 9, 2, RGB(0x1f,0x1f,0x1f)); }
    int cx = x + o->w / 2, cy = y + 15;
    float ca = cosf(a->ang), sa = sinf(a->ang);
    for (int i = 4; i <= 17; i += 3) rect(cx + (int)(ca * (float)i) - 2, cy + (int)(sa * (float)i) - 2, 4, 4, RGB(0x1b,0x1b,0x1b));
    if (a->charge > 0 && ((a->charge / 4) & 1) == 0) rect(cx + (int)(ca * 20.0f) - 2, cy + (int)(sa * 20.0f) - 2, 4, 4, RGB(0xff,0x3b,0x2a));
    rect(cx - 5, cy - 3, 10, 6, RGB(0x7a,0x7a,0x7a));
    rect(cx - 3, cy - 5, 6, 10, RGB(0x7a,0x7a,0x7a));
    rect(cx - 3, cy - 3, 6, 6, RGB(0x4a,0x4a,0x4a));
    if (a->flash > 0) rect(cx + (int)(ca * 22.0f) - 3, cy + (int)(sa * 22.0f) - 3, 6, 6, COLOR_YELLOW);
}

static void draw_tank(const Vehicle *v, int camy)
{
    int x = (int)v->x, y = (int)v->y - camy;
    int cx = x + 16, cy = y + 16;
    bool hz = fabsf(cosf(v->hull)) > fabsf(sinf(v->hull));
    bool hit = v->hit > 0;
    uint16_t hull = hit ? RGB(0xe0,0xe0,0xd0) : RGB(0x56,0x67,0x3a);
    int ph = ((int)v->tread) & 3;
    rect(x + 2, y + 3, 30, 30, RGB(0x14,0x14,0x10));                      /* sombra */
    if (hz) {
        rect(x, y + 1, 32, 8, RGB(0x26,0x24,0x1e)); rect(x, y + 23, 32, 8, RGB(0x26,0x24,0x1e));
        for (int k = ph; k < 32; k += 4) { rect(x + k, y + 1, 2, 8, RGB(0x4a,0x46,0x3a)); rect(x + k, y + 23, 2, 8, RGB(0x4a,0x46,0x3a)); }
        rect(x + 3, y + 5, 26, 22, hull);
        rect(x + 3, y + 5, 26, 4, RGB(0x44,0x53,0x2e));
    } else {
        rect(x + 1, y, 8, 32, RGB(0x26,0x24,0x1e)); rect(x + 23, y, 8, 32, RGB(0x26,0x24,0x1e));
        for (int k = ph; k < 32; k += 4) { rect(x + 1, y + k, 8, 2, RGB(0x4a,0x46,0x3a)); rect(x + 23, y + k, 8, 2, RGB(0x4a,0x46,0x3a)); }
        rect(x + 5, y + 3, 22, 26, hull);
        rect(x + 5, y + 3, 22, 4, RGB(0x44,0x53,0x2e));
    }
    float ca = cosf(v->tur), sa = sinf(v->tur);
    for (int i = 6; i <= 24; i += 3) rect(cx + (int)(ca * (float)i) - 2, cy + (int)(sa * (float)i) - 2, 4, 4, RGB(0x2a,0x2a,0x24));
    rect(cx + (int)(ca * 26.0f) - 3, cy + (int)(sa * 26.0f) - 3, 6, 6, RGB(0x1c,0x1c,0x18));
    if (v->charge > 0 && ((v->charge / 4) & 1) == 0) rect(cx + (int)(ca * 29.0f) - 2, cy + (int)(sa * 29.0f) - 2, 4, 4, RGB(0xff,0x3b,0x2a));
    if (v->flash > 0) rect(cx + (int)(ca * 30.0f) - 3, cy + (int)(sa * 30.0f) - 3, 6, 6, COLOR_YELLOW);
    uint16_t tur = hit ? RGB(0xf0,0xf0,0xe0) : RGB(0x62,0x7a,0x42);
    rect(cx - 8, cy - 5, 16, 10, tur);
    rect(cx - 5, cy - 8, 10, 16, tur);
    rect(cx - 6, cy - 6, 12, 12, tur);
    rect(cx - 4, cy - 4, 8, 8, RGB(0x4a,0x5c,0x32));
}

static void draw_truck(const Vehicle *v, int camy)
{
    int w = v->w, h = v->h, x = (int)v->x, y = (int)v->y - camy;
    bool hit = v->hit > 0;
    int fx = v->dir > 0 ? x + w - 16 : x;                /* cabina al lado hacia el que avanza */
    int bx = v->dir > 0 ? x : x + 16;
    rect(x + 1, y + h - 2, w, 4, RGB(0x14,0x14,0x10));
    for (int k = 4; k < w - 8; k += 14) { rect(x + k, y, 8, 3, RGB(0x11,0x11,0x11)); rect(x + k, y + h - 3, 8, 3, RGB(0x11,0x11,0x11)); }
    rect(bx, y + 3, w - 16, h - 6, hit ? RGB(0xd8,0xd8,0xc8) : RGB(0x4f,0x5a,0x2a));
    for (int k = 4; k < w - 20; k += 7) rect(bx + k, y + 3, 1, h - 6, RGB(0x3c,0x46,0x20));
    rect(fx, y + 4, 16, h - 8, hit ? RGB(0xe8,0xe8,0xd8) : RGB(0x3b,0x44,0x20));
    rect(v->dir > 0 ? fx + 11 : fx + 1, y + 6, 4, h - 12, RGB(0x9c,0xc4,0xd8));
    if (v->state == VS_UNLOAD) rect(v->dir > 0 ? x - 4 : x + w, y + 5, 4, h - 10, RGB(0x6a,0x5a,0x3a));
}

static void draw_vehicles(void)
{
    int camy = cam_px();
    for (int i = 0; i < MAX_VEH; i++) {
        const Vehicle *v = &veh[i];
        if (!v->active || !on_screen((int)v->y - 8, v->h + 16, camy)) continue;
        if (v->kind == VK_TANK) draw_tank(v, camy); else draw_truck(v, camy);
        if (v->hp < v->maxhp) {                                  /* barra de vida */
            int bw = v->w * 8 / 10 < 24 ? 24 : v->w * 8 / 10;
            int bx = (int)v->x + v->w / 2 - bw / 2, by = (int)v->y - camy - 7;
            rect(bx - 1, by, bw + 2, 4, COLOR_BLACK);
            rect(bx, by + 1, bw * (v->hp > 0 ? v->hp : 0) / v->maxhp, 2, v->hp * 10 / v->maxhp > 4 ? RGB(0x7a,0xd2,0x3a) : RGB(0xff,0x5a,0x3a));
        }
    }
}

static void draw_trenches(void)
{
    int camy = cam_px();
    for (int i = 0; i < trench_n; i++) {
        const Trench *t = &trenches[i];
        if (!on_screen(t->y - 4, t->h + 10, camy)) continue;
        int x = t->x, y = t->y - camy;
        rect(x - 3, y - 3, t->w + 6, t->h + 6, RGB(0x8b,0x6a,0x3a));
        rect(x - 3, y - 3, t->w + 6, 2, RGB(0x6f,0x53,0x29));
        rect(x, y, t->w, t->h, RGB(0x2a,0x1b,0x0b));
        rect(x, y + t->h - 3, t->w, 3, RGB(0x3a,0x28,0x10));
        for (int k = 8; k < t->w; k += 15) rect(x + k, y, 2, t->h, RGB(0x4e,0x3a,0x1c));
        for (int k = 0; k < trench_n * 2; k++) {
            const TrSoldier *s = &trs[k];
            if (s->t != i || !s->active || s->r <= 0) continue;
            int top = y + 10 - (int)(s->r * 14.0f);
            g_ylim = y + 14;
            erect(s->x - 5, top + 6, 10, 9, RGB(0x2b,0x44,0x66));
            erect(s->x - 3, top + 3, 6, 4, C_SKIN);
            erect(s->x - 4, top, 8, 4, RGB(0x1c,0x2d,0x42));
            if (s->r > 0.5f) erect(s->x + 3, top - 3, 2, 8, COLOR_BLACK);
            g_ylim = 100000;
        }
        rect(x - 3, y + t->h - 1, t->w + 6, 4, RGB(0x8b,0x6a,0x3a));   /* reborde delantero */
    }
}


/* tilde de sacos y troncos/rocas; los sacos se dibujan DESPUÉS de los soldados
 * para que los parapetados queden ocultos tras ellos */
static void draw_obstacles(bool bags)
{
    int camy = cam_px();
    int ti = (level - 1) % 3;
    for (int i = 0; i < obs_n; i++) {
        const Obstacle *o = &obs[i];
        if ((o->type == OB_SANDBAG) != bags) continue;
        if ((o->type >= OB_FORT && o->type <= OB_BRIDGE) || o->type == OB_WATER) continue;     /* se dibujan aparte */
        if (i == gate_idx) continue;
        if (!on_screen(o->y - 16, o->h + 18, camy)) continue;
        int x = o->x, y = o->y - camy;
        if (o->type == OB_SANDBAG) {
            rect(x, y, o->w, o->h, C_BAG);
            rect(x, y + o->h / 2, o->w, 1, C_BAG_DK);
            for (int k = 7; k < o->w; k += 11) rect(x + k, y, 1, o->h / 2, C_BAG_DK);
            for (int k = 2; k < o->w; k += 11) rect(x + k, y + o->h / 2 + 1, 1, o->h - o->h / 2 - 1, C_BAG_DK);
        } else if (o->type == OB_RUBBLE) {
            rect(x + 1, y + o->h / 3, o->w - 2, o->h * 2 / 3, RGB(0x3a,0x34,0x2a));
            rect(x + 3, y + 2, o->w / 3, o->h / 3, RGB(0x5a,0x52,0x44));
            rect(x + o->w / 2, y + o->h / 2, o->w / 3, o->h / 3, RGB(0x6a,0x62,0x54));
        } else if (o->type == OB_BUNKER) {
            draw_bunker(o, i, x, y);
        } else if (IS_STRUCT(o->type)) {
            draw_structure(o, x, y);
        } else if (o->type == OB_ROCK) {
            rect(x, y, o->w, o->h, C_ROCK);
            rect(x + 1, y + 1, o->w - 4, o->h / 2, C_ROCK_HI);
        } else {
            rect(x + 2, y + 1, 4, 7, C_TRUNK);
            rect(x - 5, y - 9, 18, 10, TREE_COL[ti][0]);
            rect(x - 2, y - 13, 12, 14, TREE_COL[ti][0]);
            rect(x, y - 11, 6, 5, TREE_COL[ti][1]);
        }
    }
}


static void draw_enemy(const Enemy *e, int camy)
{
    int x = (int)e->x, y = (int)e->y - camy;
    uint16_t coat = e->type == 1 ? RGB(0x8a,0x1c,0x1c) : (e->dodge_t > 0 ? RGB(0x4d,0x6f,0x9e) : RGB(0x2b,0x44,0x66));
    uint16_t helm = e->type == 1 ? RGB(0xbd,0x24,0x24) : RGB(0x1c,0x2d,0x42);
    if (e->type == 3) { coat = RGB(0x4a,0x5a,0x3a); helm = RGB(0x2f,0x3a,0x24); }
    if (e->type == 2) { coat = e->dodge_t > 0 ? RGB(0x8a,0x74,0x40) : RGB(0x6a,0x52,0x30); helm = RGB(0x3a,0x2c,0x18); }
    g_ylim = 100000;
    if (e->anchored) {
        int p = e->peek, amt;
        if (p > 28) amt = (34 - p) * 100 / 6;
        else if (p > 0 && p < 6) amt = p * 100 / 6;
        else amt = p > 0 ? 100 : 0;
        y += (100 - amt) * 8 / 100;
        g_ylim = obs[e->cover].y - camy;
    }
    int leg = (isin((uint32_t)e->anim * 3) * 2) / 1024;
    erect(x + 1, y + 4, 10, 7, coat);
    erect(x + 3, y + 2, 6, 3, C_SKIN);
    erect(x + 2, y, 8, 3, helm);
    erect(x + 2, y + 11, 3, 4 + leg, RGB(0x18,0x24,0x33));
    erect(x + 7, y + 11, 3, 4 - leg, RGB(0x18,0x24,0x33));
    {
        int dx = e->fx > 0.38f ? 1 : (e->fx < -0.38f ? -1 : 0);
        int dy = e->fy > 0.38f ? 1 : (e->fy < -0.38f ? -1 : 0);
        if (e->type == 3 && e->stat) {                        /* mortero: placa base y tubo inclinado */
            erect(x - 1, y + 11, 14, 4, RGB(0x2a,0x2a,0x2a));
            for (int i = 0; i < 4; i++) erect(x + 11 - i * 2, y + 9 - i * 3, 3, 3, RGB(0x3a,0x3f,0x35));
            if (e->mflash > 0) erect(x + 3, y - 6, 5 + e->mflash / 2, 5 + e->mflash / 2, COLOR_YELLOW);
        } else if (e->type == 2) {                            /* tubo del bazooka, más largo */
            for (int i = 3; i <= 13; i += 2) erect(x + 6 + dx * i - 1, y + 7 + dy * i - 1, 3, 3, RGB(0x4a,0x50,0x40));
            if (e->aim > 0 && ((e->aim / 3) & 1) == 0) erect(x + 6 + dx * 15 - 1, y + 7 + dy * 15 - 1, 3, 3, RGB(0xff,0x3b,0x2a));
        } else {
            for (int i = 4; i <= 8; i += 2) erect(x + 6 + dx * i - 1, y + 7 + dy * i - 1, 2, 2, C_GUN);
        }
    }
    g_ylim = 100000;
    if (e->state == E_CHASE && !e->anchored) { rect(x + 5, y - 6, 2, 3, COLOR_RED); rect(x + 5, y - 2, 2, 1, COLOR_RED); }
    else if (e->state == E_SEARCH) rect(x + 5, y - 6, 2, 4, COLOR_YELLOW);
}

static void draw_hostages(void)
{
    int camy = cam_px();
    for (int i = 0; i < MAX_HOSTAGES; i++) {
        const Hostage *h = &hostages[i];
        if (!h->active || !on_screen((int)h->y, 15, camy)) continue;
        int x = (int)h->x, y = (int)h->y - camy;
        if (h->freed) y += ((h->t / 6) & 1) ? -1 : 0;
        rect(x + 1, y + 5, 10, 6, RGB(0xd9,0xd0,0x90));                 /* camisa */
        rect(x + 3, y + 2, 6, 3, C_SKIN);
        rect(x + 2, y, 8, 3, h->freed ? RGB(0xd0,0x20,0x20) : RGB(0x4a,0x3a,0x20));
        rect(x + 2, y + 11, 3, 4, RGB(0x4a,0x5a,0x7a));
        rect(x + 7, y + 11, 3, 4, RGB(0x4a,0x5a,0x7a));
        if (h->freed) { rect(x - 1, y - 3, 2, 8, C_SKIN); rect(x + 11, y - 3, 2, 8, C_SKIN); }
        else { rect(x + 1, y + 8, 10, 2, RGB(0x6a,0x4a,0x2a)); }        /* atado */
    }
}

static void draw_enemies(void)
{
    int camy = cam_px();
    for (int i = 0; i < MAX_ENEMIES; i++) {                              /* nidos de mortero (sacos en U) */
        const Enemy *e = &enemies[i];
        if (!e->active || !e->stat || !on_screen((int)e->y, EH, camy)) continue;
        int x = (int)e->x, y = (int)e->y - camy;
        rect(x - 5, y + 8, 22, 6, C_BAG); rect(x - 5, y + 8, 22, 1, C_BAG_DK);
        rect(x - 5, y + 1, 5, 10, C_BAG); rect(x + 12, y + 1, 5, 10, C_BAG);
        rect(x - 5, y + 5, 5, 1, C_BAG_DK); rect(x + 12, y + 5, 5, 1, C_BAG_DK);
    }
    for (int i = 0; i < MAX_ENEMIES; i++) {
        const Enemy *e = &enemies[i];
        if (!e->active || e->dormant || !on_screen((int)e->y, EH, camy)) continue;
        if (e->leaving > 0 && ((e->leaving / 3) & 1)) continue;     /* parpadea al irse */
        draw_enemy(e, camy);
    }
}

static void draw_items(void)
{
    int camy = cam_px();
    for (int i = 0; i < MAX_ITEMS; i++) {
        const Item *it = &items[i];
        if (!it->active || !on_screen(it->y, 10, camy)) continue;
        int x = it->x, y = it->y - camy;
        if (it->type == IT_GRENADES) { rect(x, y, 10, 10, RGB(0x2a,0x3a,0x1a)); rect(x + 3, y + 3, 4, 4, COLOR_YELLOW); }
        else if (it->type == IT_LIFE) { rect(x, y, 10, 10, RGB(0xb0,0x10,0x10)); rect(x + 4, y + 2, 2, 6, COLOR_WHITE); rect(x + 2, y + 4, 6, 2, COLOR_WHITE); }
        else { rect(x, y, 10, 10, RGB(0x20,0x40,0xb0)); rect(x + 2, y + 5, 6, 1, COLOR_WHITE); rect(x + 2, y + 2, 1, 3, COLOR_WHITE); rect(x + 7, y + 2, 1, 3, COLOR_WHITE); rect(x + 4, y + 2, 2, 3, COLOR_WHITE); }
    }
}

static void draw_ebullets(void)
{
    int camy = cam_px();
    for (int i = 0; i < MAX_EBULLETS; i++) {
        const EBullet *b = &ebullets[i];
        if (!b->active) continue;
        if (b->shell) {
            int x = (int)b->x, y = (int)b->y - camy;
            rect(x - 3, y - 3, 6, 6, RGB(0xff,0x9a,0x1a));
            rect(x - 1, y - 1, 2, 2, RGB(0x2a,0x2a,0x2a));
        } else if (b->rocket) {
            int x = (int)b->x, y = (int)b->y - camy;
            rect(x - 2, y - 2, 5, 5, RGB(0xc0,0xc0,0xb8));
            rect((int)(b->x - b->vx * 5) - 1, (int)(b->y - b->vy * 5) - camy - 1, 3, 3, RGB(0xff,0xb0,0x2e));
        } else rect((int)b->x - 1, (int)b->y - camy - 1, 3, 3, C_EBULLET);
    }
}

static void draw_grenades(void)
{
    int camy = cam_px();
    for (int i = 0; i < MAX_GRENADES; i++) {
        const Grenade *g = &grenades[i];
        if (!g->active) continue;
        float t = g->prog;
        float x = g->sx + (g->tx - g->sx) * t, y = g->sy + (g->ty - g->sy) * t;
        float h = sinf(t * 3.14159f) * g->arc;
        if (!g->mine) {                       /* marca de impacto: 8 puntos en círculo */
            int r = (int)g->r - 4, tx = (int)g->tx, ty = (int)g->ty - camy;
            static const int8_t ox[8] = { 100, 71, 0, -71, -100, -71, 0, 71 };
            static const int8_t oy[8] = { 0, 71, 100, 71, 0, -71, -100, -71 };
            for (int k = 0; k < 8; k++) rect(tx + ox[k] * r / 100 - 1, ty + oy[k] * r / 100 - 1, 2, 2, RGB(0xff,0x3c,0x28));
        }
        rect((int)x - 1, (int)(y - h) - camy - 1, 4, 4, g->mine ? RGB(0x22,0x22,0x22) : RGB(0x3b,0x4a,0x2a));
        rect((int)x - 1, (int)y - camy, 3, 1, RGB(0x10,0x10,0x10));   /* sombra en el suelo */
    }
}

static void draw_explosions(void)
{
    int camy = cam_px();
    for (int i = 0; i < MAX_EXPL; i++) {
        const Explosion *x = &booms[i];
        if (!x->active) continue;
        int r = (int)x->r, cx = (int)x->x, cy = (int)x->y - camy;
        uint16_t c = x->r > x->maxr * 0.6f ? RGB(0x55,0x55,0x55) : (((int)x->r % 4) > 1 ? COLOR_YELLOW : RGB(0xff,0x33,0x00));
        int a = r * 85 / 100;
        rect(cx - r, cy - r * 6 / 10, 2 * r, r * 12 / 10 + 1, c);
        rect(cx - r * 6 / 10, cy - r, r * 12 / 10 + 1, 2 * r, c);
        rect(cx - a, cy - a, 2 * a, 2 * a, c);
    }
}

static void draw_player_dead(void)
{
    int camy = cam_px();
    int cx = UNFX(pl.x) + PLAYER_W / 2, cy = UNFX(pl.y) - camy + PLAYER_H / 2;
    if (pl.death_t < 30 && (pl.death_t % 6) < 3) return;          /* parpadeo final */
    int t = DEATH_STEPS - pl.death_t;
    if (t < 20) {                                                   /* cae de lado */
        rect(cx - 5, cy - 6 + t / 4, 10, 12 - t / 4, C_BODY);
        rect(cx - 3, cy - 8 + t / 3, 6, 5, C_SKIN);
    } else {                                                        /* tumbado */
        int d = pl.death_dir;
        rect(cx - 8, cy - 3, 14, 7, C_BODY);
        rect(cx + (d > 0 ? 6 : -11), cy - 3, 5, 5, C_SKIN);
        rect(cx + (d > 0 ? -12 : 7), cy - 2, 4, 5, C_LEGS);
    }
    if (t < 20) for (int k = 0; k < 6; k++) {                       /* chispas */
        int a = k * 42 + pl.death_t * 7, rr = 6 + t;
        rect(cx + isin((uint32_t)a) * rr / 1024 - 1, cy + isin((uint32_t)a + 64) * rr / 1024 - 1, 2, 2, COLOR_YELLOW);
    }
}

/* ---------------------------------------------------------
 * Medidor de rendimiento (visible en pantalla)
 * --------------------------------------------------------- */
static int perf_fps, perf_draw_ms10, perf_flush_ms10;
/* Estadísticas del volcado SPI (medias por frame): S = ms de bus ocupado,
 * W = ms que el juego espera al hilo SPI, R = rectángulos, K = KB enviados. */
static int perf_spi_ms10, perf_wait_ms10, perf_rects, perf_kb;

#if SIDE_PANELS
#define C_PANEL RGB(0x10, 0x14, 0x08)
#define C_FRAME RGB(0x6a, 0x6a, 0x5a)

#ifndef CMD_SHOW_STATS
#define CMD_SHOW_STATS 0                   /* 1 = muestra FPS/tiempos de depuración en el panel derecho */
#endif

/* Estrella de 11x11 para la decoración de los paneles */
static const char *const STAR_BMP[11] = {
    "     X     ", "     X     ", "    XXX    ", "XXXXXXXXXXX", " XXXXXXXXX ", "  XXXXXXX  ",
    "  XXXXXXX  ", " XXXXXXXXX ", " XXXX XXXX ", " XXX   XXX ", "XX       XX" };

static void panel_star(int cx, int cy, int sc, uint16_t col, uint16_t shadow)
{
    for (int pass = 0; pass < 2; pass++)
        for (int y = 0; y < 11; y++)
            for (int x = 0; x < 11; x++)
                if (STAR_BMP[y][x] == 'X')
                    rect_abs(cx - 11 * sc / 2 + x * sc + (pass == 0 ? 1 : 0), cy - 11 * sc / 2 + y * sc + (pass == 0 ? 1 : 0),
                             sc, sc, pass == 0 ? shadow : col);
}

static void panel_bullet(int x, int y)
{
    rect_abs(x + 1, y, 4, 3, RGB(0xb8, 0x6a, 0x30));          /* punta de cobre */
    rect_abs(x, y + 3, 6, 8, RGB(0xd8, 0xa8, 0x20));          /* vaina de latón */
    rect_abs(x + 4, y + 3, 2, 8, RGB(0x9a, 0x78, 0x18));
    rect_abs(x, y + 10, 6, 1, RGB(0x5a, 0x44, 0x10));
}

static void panel_text_c(int x0, int w, int y, const char *t, uint16_t col)
{
    renderer_draw_text((uint16_t)(x0 + (w - (int)st7789_text_width(t, 1)) / 2), (uint16_t)y, t, col, C_PANEL, 1);
}

static void draw_hud(void)
{
    char buf[24];
    const int rx = FIELD_X + FIELD_W;                 /* inicio del panel derecho */
    const int lw = FIELD_X - 2;                       /* ancho útil del panel izquierdo */
    const int rw = SCR_W - rx - 2, rx0 = rx + 2;      /* ídem, panel derecho */

    if (!panels_ready) {
        rect_abs(0, 0, FIELD_X, SCR_H, C_PANEL);
        rect_abs(rx, 0, SCR_W - rx, SCR_H, C_PANEL);
        rect_abs(FIELD_X - 2, 0, 2, SCR_H, C_FRAME);   /* marco del campo */
        rect_abs(rx, 0, 2, SCR_H, C_FRAME);
        panel_text_c(0, lw, 8, "PUNTOS", COLOR_YELLOW);
        panel_text_c(0, lw, 44, "GRANADAS", COLOR_YELLOW);
        panel_text_c(0, lw, SCR_H - 30, "VIDAS", COLOR_YELLOW);

        /* decoración (se pinta una sola vez por partida, no cuesta fps) */
        panel_star(lw / 2, 112, 3, RGB(0xd8, 0xa8, 0x20), RGB(0x5a, 0x44, 0x10));
        for (int r = 0; r < 2; r++)                                    /* cinta de balas */
            for (int i = 0; i < 5; i++) panel_bullet(lw / 2 - 25 + i * 10, 150 + r * 16);
        rect_abs(4, 80, lw - 8, 1, C_FRAME); rect_abs(4, 188, lw - 8, 1, C_FRAME);

        panel_text_c(rx0, rw, 8, "COMMANDO", RGB(0xf8, 0x48, 0x28));
        panel_text_c(rx0, rw, 20, "1985", COLOR_WHITE);
        rect_abs(rx0 + 4, 34, rw - 8, 1, C_FRAME);
        {   /* bloque de camuflaje */
            static const uint16_t camo[4] = { RGB(0x2a,0x3a,0x18), RGB(0x3a,0x4a,0x20), RGB(0x1a,0x24,0x0e), RGB(0x4a,0x40,0x22) };
            const int cx0 = rx0 + 5, cw = rw - 10, cy0 = 44, ch = SCR_H - 44 - 40;
            rect_abs(cx0, cy0, cw, ch, camo[0]);
            uint32_t q = 12345;
            for (int i = 0; i < 26; i++) {
                q = q * 1103515245u + 12345u;
                int bw = 8 + (int)((q >> 16) % 14), bh = 6 + (int)((q >> 20) % 12);
                q = q * 1103515245u + 12345u;
                int bx = cx0 + (int)((q >> 16) % (uint32_t)(cw - bw)), by = cy0 + (int)((q >> 20) % (uint32_t)(ch - bh));
                rect_abs(bx, by, bw, bh, camo[1 + (i % 3)]);
            }
            rect_abs(cx0 - 1, cy0 - 1, cw + 2, 1, C_FRAME); rect_abs(cx0 - 1, cy0 + ch, cw + 2, 1, C_FRAME);
            rect_abs(cx0 - 1, cy0 - 1, 1, ch + 2, C_FRAME); rect_abs(cx0 + cw, cy0 - 1, 1, ch + 2, C_FRAME);
            panel_star(rx0 + rw / 2, cy0 + ch / 2, 2, RGB(0xd8, 0xa8, 0x20), RGB(0x1a, 0x14, 0x04));
        }
        for (int i = 0; i < 4; i++) panel_bullet(rx0 + rw / 2 - 20 + i * 10, SCR_H - 30);
        panels_ready = true;
    }

    snprintf(buf, sizeof(buf), "%06lu", (unsigned long)score);
    panel_text_c(0, lw, 20, buf, COLOR_WHITE);
    snprintf(buf, sizeof(buf), "%3d", pl.grenades);
    panel_text_c(0, lw, 56, buf, COLOR_WHITE);

#if CMD_SHOW_STATS
    snprintf(buf, sizeof(buf), "%2dFPS", perf_fps);
    renderer_draw_text(rx + 8, 8, buf, COLOR_YELLOW, C_PANEL, 1);
    snprintf(buf, sizeof(buf), "D%2d.%d F%2d.%d", perf_draw_ms10 / 10, perf_draw_ms10 % 10, perf_flush_ms10 / 10, perf_flush_ms10 % 10);
    renderer_draw_text(rx + 8, 20, buf, COLOR_YELLOW, C_PANEL, 1);
    snprintf(buf, sizeof(buf), "S%2d.%d W%2d.%d", perf_spi_ms10 / 10, perf_spi_ms10 % 10, perf_wait_ms10 / 10, perf_wait_ms10 % 10);
    renderer_draw_text(rx + 8, 32, buf, COLOR_CYAN, C_PANEL, 1);
    snprintf(buf, sizeof(buf), "R%3d K%3d", perf_rects, perf_kb);
    renderer_draw_text(rx + 8, 44, buf, COLOR_CYAN, C_PANEL, 1);
#endif

    const int lx = (lw - (10 * 5 - 2)) / 2;           /* vidas centradas */
    rect_abs(lx, SCR_H - 18, 10 * 5, 4, C_PANEL);     /* borra y repinta las vidas */
    for (int i = 0; i < pl.lives && i < 10; i++)
        rect_abs(lx + i * 5, SCR_H - 18, 3, 4, C_BAND);
}
#else
static void draw_hud(void)
{
    char buf[40];
    rect(0, 0, FIELD_W, HUD_H, RGB(0, 0, 0));
    snprintf(buf, sizeof(buf), "PTS %06lu", (unsigned long)score);
    renderer_draw_text(FIELD_X + 3, 3, buf, COLOR_WHITE, COLOR_BLACK, 1);
    for (int i = 0; i < pl.lives && i < 10; i++)
        rect(3 + i * 5, SCR_H - 6, 3, 4, C_BAND);
}
#endif

static void draw_title(void)
{
    uint16_t row[CMD_TITLE_W];
    for (int y = 0; y < CMD_TITLE_H; y++) {                 /* cartel a pantalla completa, fila a fila */
        const uint8_t *p = &cmd_title_pixels[y * CMD_TITLE_W];
        for (int x = 0; x < CMD_TITLE_W; x++) row[x] = cmd_title_palette[p[x] & 63];
        renderer_blit_to_buffer(0, (uint16_t)y, CMD_TITLE_W, 1, row);
    }
    {   /* el título va en el recuadro oscuro de arriba a la derecha del cartel */
        const char *t1 = "MISSING IN", *t2 = "ARCADE";
        const int cx = 228;
        renderer_draw_text((uint16_t)(cx - (int)st7789_text_width(t1, 2) / 2), 24, t1, RGB(0xf8, 0x48, 0x28), COLOR_BLACK, 2);
        renderer_draw_text((uint16_t)(cx - (int)st7789_text_width(t2, 4) / 2), 46, t2, RGB(0xf8, 0xb8, 0x00), COLOR_BLACK, 4);
    }
    if ((title_t / 25) & 1) {
        const char *c = "PULSA FUEGO";
        renderer_draw_text((SCR_W - (int)st7789_text_width(c, 2)) / 2, SCR_H - 26, c, COLOR_WHITE, COLOR_BLACK, 2);
    }
}


/* ---------------------------------------------------------
 * Fortaleza, puente con túnel y moto bombardera
 * --------------------------------------------------------- */
static void draw_wall(int x, int y, int w, int h)
{
    rect(x, y, w, h, RGB(0x4d,0x4a,0x45));
    for (int yy = 8, row = 0; yy < h; yy += 8, row++) {
        rect(x, y + yy, w, 1, RGB(0x34,0x32,0x2e));
        for (int xx = (row & 1) * 12 + 12; xx < w; xx += 24) rect(x + xx, y + yy - 8, 1, 8, RGB(0x34,0x32,0x2e));
    }
}

static void draw_fort(void)
{
    int camy = cam_px();
    if (camy > 130) return;
    int fx = fort_x;
    draw_wall(0, 12 - camy, FIELD_W, 32);
    draw_wall(0, 44 - camy, fx, 68);
    draw_wall(fx + FORT_DOOR_W, 44 - camy, FIELD_W - fx - FORT_DOOR_W, 68);
    for (int x = 0; x < FIELD_W; x += 15) rect(x + 1, 6 - camy, 10, 8, RGB(0x6a,0x66,0x5e));   /* almenas */
    rect(fx, 44 - camy, FORT_DOOR_W, 30, RGB(0x14,0x11,0x0c));
    rect(fx, 74 - camy, FORT_DOOR_W, 38, RGB(0x2a,0x21,0x18));
    if (!fort_active) {                                    /* portón cerrado */
        rect(fx, 70 - camy, FORT_DOOR_W, 14, RGB(0x5a,0x3a,0x1a));
        for (int k = 5; k < FORT_DOOR_W; k += 10) rect(fx + k, 70 - camy, 2, 14, RGB(0x2b,0x1b,0x0a));
        rect(fx, 75 - camy, FORT_DOOR_W, 2, RGB(0x9a,0xa0,0xa6));
    }
    rect(fx - 3, 44 - camy, 3, 68, RGB(0x2d,0x2b,0x27));
    rect(fx + FORT_DOOR_W, 44 - camy, 3, 68, RGB(0x2d,0x2b,0x27));
    rect(fx + 19, -4 - camy, 2, 18, RGB(0x8a,0x8a,0x8a));          /* bandera */
    rect(fx + 21, -4 - camy, 9, 6, RGB(0xc4,0x16,0x1c));
}

static void draw_rivers(void)
{
    int camy = cam_px();
    for (int i = 0; i < river_n; i++) {
        const River *r = &rivers[i];
        if (!on_screen(r->y, r->h, camy)) continue;
        int sy = r->y - camy;
        rect(0, sy, FIELD_W, r->h, RGB(0x1b,0x5e,0x85));
        for (int y = 5; y < r->h - 3; y += 9)
            for (int x = ((y / 9) & 1) * 11 + 3; x < FIELD_W; x += 22) rect(x, sy + y, 8, 1, RGB(0x29,0x80,0xb9));
        rect(0, sy, FIELD_W, 2, RGB(0x4a,0x3a,0x22)); rect(0, sy + r->h - 2, FIELD_W, 2, RGB(0x4a,0x3a,0x22));
        rect(r->bx, sy - 2, r->bw, r->h + 4, RGB(0x8a,0x5a,0x2b));                /* puente de madera */
        for (int y = 1; y < r->h + 3; y += 5) rect(r->bx, sy - 2 + y, r->bw, 1, RGB(0x4a,0x2f,0x13));
        rect(r->bx, sy - 2, 2, r->h + 4, RGB(0x5a,0x3a,0x18)); rect(r->bx + r->bw - 2, sy - 2, 2, r->h + 4, RGB(0x5a,0x3a,0x18));
    }
}

static void draw_tunnel_floor(void)
{
    int camy = cam_px();
    if (!on_screen(bridge_y, BR_H, camy)) return;
    rect(tunnel_x, bridge_y - camy, TUNNEL_W, BR_H, RGB(0x2a,0x21,0x18));
}

static void draw_bridge(void)
{
    int camy = cam_px();
    if (!on_screen(bridge_y, BR_H, camy)) return;
    int sy = bridge_y - camy, rx = tunnel_x + TUNNEL_W;
    rect(0, sy + BR_H, tunnel_x, 5, RGB(0x1c,0x30,0x14));                    /* sombra */
    rect(rx, sy + BR_H, FIELD_W - rx, 5, RGB(0x1c,0x30,0x14));
    for (int side = 0; side < 2; side++) {                                    /* calzada */
        int a = side ? rx : 0, b = side ? FIELD_W : tunnel_x;
        rect(a, sy, b - a, BR_H, RGB(0x6f,0x6f,0x6f));
        for (int x = a; x < b; x += 10) rect(x, sy + 3, 1, BR_H - 6, RGB(0x5b,0x5b,0x5b));
    }
    if (player_in_tunnel()) {                                                 /* dentro se ve al jugador: solo los bordes del techo */
        rect(tunnel_x, sy, TUNNEL_W, 4, RGB(0x30,0x2c,0x26));
        rect(tunnel_x, sy + BR_H - 4, TUNNEL_W, 4, RGB(0x30,0x2c,0x26));
    } else {
        rect(tunnel_x, sy, TUNNEL_W, BR_H, RGB(0x30,0x2c,0x26));
    }
    for (int x = 3; x < FIELD_W; x += 14) rect(x, sy + BR_H / 2 - 1, 7, 2, RGB(0xd9,0xc4,0x41));   /* línea central */
    rect(0, sy, FIELD_W, 3, RGB(0xb5,0xb5,0xb5));                              /* barandillas */
    rect(0, sy + BR_H - 3, FIELD_W, 3, RGB(0xb5,0xb5,0xb5));
    for (int x = 0; x < FIELD_W; x += 15) { rect(x, sy - 1, 3, 5, RGB(0x8a,0x8a,0x8a)); rect(x, sy + BR_H - 4, 3, 5, RGB(0x8a,0x8a,0x8a)); }
    rect(tunnel_x - 4, sy - 1, 4, 9, RGB(0x4a,0x4a,0x4a));                     /* bocas del túnel */
    rect(rx, sy - 1, 4, 9, RGB(0x4a,0x4a,0x4a));
    rect(tunnel_x - 4, sy + BR_H - 8, 4, 9, RGB(0x4a,0x4a,0x4a));
    rect(rx, sy + BR_H - 8, 4, 9, RGB(0x4a,0x4a,0x4a));
    rect(tunnel_x, sy, TUNNEL_W, 4, RGB(0x1e,0x1c,0x18));
    rect(tunnel_x, sy + BR_H - 4, TUNNEL_W, 4, RGB(0x1e,0x1c,0x18));
}

static void bike_rect(int dx, int dy, int w, int h, uint16_t c)
{
    int sy = bridge_y + BR_H / 2 - cam_px();
    int x = bike.dir > 0 ? (int)bike.x + dx : (int)bike.x - dx - w;
    rect(x, sy + dy, w, h, c);
}

static void draw_bike(void)
{
    if (bike.wait > 0 && (bike.x < -30 || bike.x > FIELD_W + 30)) return;
    int sy = bridge_y + BR_H / 2 - cam_px();
    if (sy < -30 || sy > SCR_H + 30) return;
    bike_rect(-14, -3, 6, 5, RGB(0x11,0x11,0x11)); bike_rect(9, -3, 6, 5, RGB(0x11,0x11,0x11));          /* ruedas */
    bike_rect(-5, 4, 3, 5, RGB(0x55,0x55,0x55)); bike_rect(4, 4, 3, 5, RGB(0x55,0x55,0x55));              /* unión sidecar */
    bike_rect(-2, 17, 7, 3, RGB(0x11,0x11,0x11));
    bike_rect(-10, 8, 21, 10, RGB(0x4f,0x5a,0x2a)); bike_rect(-10, 8, 21, 2, RGB(0x3b,0x44,0x20));
    bike_rect(-6, 10, 12, 6, RGB(0x1d,0x24,0x10));
    bike_rect(-3, 10, 8, 6, RGB(0x2b,0x44,0x66)); bike_rect(-2, 9, 5, 4, RGB(0x1c,0x2d,0x42)); bike_rect(-1, 12, 3, 2, C_SKIN);
    if (bike.throw_anim > 0) { bike_rect(3, 14, 3, 7, RGB(0x2b,0x44,0x66)); bike_rect(4, 21, 3, 3, RGB(0x3b,0x4a,0x2a)); }
    else { bike_rect(3, 11, 3, 4, RGB(0x2b,0x44,0x66)); bike_rect(5, 15, 2, 2, RGB(0x3b,0x4a,0x2a)); }
    bike_rect(-10, -3, 21, 7, RGB(0xd9,0x53,0x1e)); bike_rect(-10, -3, 21, 2, RGB(0xa6,0x3c,0x12));       /* moto */
    bike_rect(8, -6, 3, 12, RGB(0x8c,0x8c,0x8c));
    bike_rect(-5, -5, 8, 10, RGB(0x2f,0x2f,0x2f)); bike_rect(-3, -2, 5, 5, RGB(0x1a,0x1a,0x1a));
    bike_rect(-12, -5, 6, 3, RGB(0xc0,0x26,0x1b));
}

static void render(void)
{
    if (state == ST_TITLE) { draw_title(); return; }
    if (state == ST_SCORES) {
        if (!scores_drawn) { renderer_clear(COLOR_BLACK); highscores_draw(CMD_GAME_ID, "COMMANDO", 20); scores_drawn = true; }
        return;
    }
    draw_terrain();
    draw_rivers();
    draw_trenches();
    draw_fort();
    draw_tunnel_floor();
    draw_items();
    draw_hostages();
    draw_obstacles(false);
    draw_vehicles();
    draw_enemies();
    if (pl.dying) draw_player_dead(); else draw_player();
    draw_obstacles(true);
    draw_bullets();
    draw_ebullets();
    draw_bridge();
    draw_bike();
    draw_grenades();
    draw_explosions();
    if (state == ST_GAMEOVER) {
        const char *g = "GAME OVER";
        int w = (int)st7789_text_width(g, 2);
        renderer_draw_text(FIELD_X + (FIELD_W - w) / 2, SCR_H / 2 - 7, g, COLOR_RED, COLOR_BLACK, 2);
    }
    if (state == ST_WIN) {
        char buf[24];
        const char *t1 = "FELICIDADES!";
        snprintf(buf, sizeof buf, "FASE %d SUPERADA", level);
        int w1 = (int)st7789_text_width(t1, 2), w2 = (int)st7789_text_width(buf, 1);
        renderer_fill_rect((uint16_t)(FIELD_X + 10), (uint16_t)(SCR_H / 2 - 30), (uint16_t)(FIELD_W - 20), 60, COLOR_BLACK);
        renderer_draw_text((uint16_t)(FIELD_X + (FIELD_W - w1) / 2), SCR_H / 2 - 24, t1, COLOR_YELLOW, COLOR_BLACK, 2);
        renderer_draw_text((uint16_t)(FIELD_X + (FIELD_W - w2) / 2), SCR_H / 2 - 2, buf, COLOR_WHITE, COLOR_BLACK, 1);
        if (win_t > GAME_SPEED * 3 && ((win_t / 25) & 1)) {
            const char *c = "PULSA FUEGO";
            int w3 = (int)st7789_text_width(c, 1);
            renderer_draw_text((uint16_t)(FIELD_X + (FIELD_W - w3) / 2), SCR_H / 2 + 14, c, COLOR_RED, COLOR_BLACK, 1);
        }
    }
    draw_hud();
}

static void flush_frame(void)
{
#if CMD_FLUSH_BUDGET > 0
    if (state == ST_PLAYING) { renderer_flush_limited(CMD_FLUSH_BUDGET); return; }
#endif
    renderer_flush();
}

/* ---------------------------------------------------------
 * Bucle principal: paso fijo + render, con medida de tiempos
 * --------------------------------------------------------- */
void game_commando_run(game_mode_t mode)
{
    /* 1 jugador; 2P se trata como 1P. En DEMO (attract del menú) aún no hay IA:
     * se muestra el título unos segundos y se vuelve al menú. */
    const bool is_demo = (mode == GAME_MODE_DEMO);
    g_demo = is_demo;
    uint64_t demo_t0 = now_us();

    math_init();
    rng_state ^= (uint32_t)now_us();
    if (!rng_state) rng_state = 1;
    state = ST_TITLE;
    g_done = false; fire_buf = 0;
    title_t = 0;

    st7789_set_rotation(CMD_ROTATION);
    renderer_clear(COLOR_BLACK);
    renderer_flush();

    const TickType_t period_ticks = pdMS_TO_TICKS(1000 / TARGET_FPS);
    TickType_t last_wake = xTaskGetTickCount();

    uint64_t last = now_us();
    int64_t  accum = 0;
    uint64_t fps_t0 = last;
    int      frames = 0;
    uint64_t draw_acc = 0, flush_acc = 0, spi_acc = 0, wait_acc = 0, rects_acc = 0, bytes_acc = 0;
#ifdef COMMANDO_HOST_FRAMES
    int host_frames = 0;
#endif

    while (!g_done) {
        uint64_t now = now_us();
        accum += (int64_t)(now - last);
        last = now;
        if (accum > 250000) accum = 250000;

        controls_update();
        sound_update();

        in_fire_down  = controls_button_down(BTN_FIRE);
        in_nade_down  = controls_button_down(BTN_NADE);
        const bool fire_edge = controls_button_pressed(BTN_FIRE);
        in_start_edge = fire_edge || controls_button_pressed(BTN_NADE);
        if (fire_edge) fire_buf = 8;                 /* cada pulsación = un disparo (se guarda unos pasos si aún recarga) */
        in_quit_edge  = controls_button_pressed(BTN_IDX_J1_SW) || controls_button_pressed(BTN_IDX_J2_SW);
        if (in_quit_edge) break;
        if (is_demo && (now - demo_t0 > 5000000ULL || in_start_edge || in_fire_down)) break;

        int steps = 0;
        while (accum >= STEP_US && steps < MAX_STEPS) {
            logic_step();
            in_start_edge = false;   /* el flanco solo vale para el primer paso */
            accum -= STEP_US;
            steps++;
        }
        if (steps == MAX_STEPS) accum = 0;

        uint64_t t0 = now_us();
        render();
        uint64_t t1 = now_us();
        flush_frame();
        uint64_t t2 = now_us();
        {
            st7789_flush_stats_t fs;
            st7789_get_flush_stats(&fs);
            spi_acc += fs.spi_us; wait_acc += fs.wait_us;
            rects_acc += fs.rects; bytes_acc += fs.bytes;
        }

#ifdef COMMANDO_HOST_FRAMES
        if (++host_frames >= COMMANDO_HOST_FRAMES) break;
#endif
        draw_acc  += t1 - t0;
        flush_acc += t2 - t1;
        frames++;
        if (t2 - fps_t0 >= 1000000) {
            perf_fps = frames;
            perf_draw_ms10  = (int)(draw_acc  / (uint64_t)frames / 100);
            perf_flush_ms10 = (int)(flush_acc / (uint64_t)frames / 100);
            perf_spi_ms10  = (int)(spi_acc  / (uint64_t)frames / 100);
            perf_wait_ms10 = (int)(wait_acc / (uint64_t)frames / 100);
            perf_rects     = (int)(rects_acc / (uint64_t)frames);
            perf_kb        = (int)(bytes_acc / (uint64_t)frames / 1024);
            frames = 0; draw_acc = flush_acc = 0;
            spi_acc = wait_acc = rects_acc = bytes_acc = 0;
            fps_t0 = t2;
        }

        vTaskDelayUntil(&last_wake, period_ticks ? period_ticks : 1);
    }

    sound_stop_scramble_victory();
    highscores_flush();

    /* Vuelta al menú: rotación horizontal y entrada limpia. */
    st7789_set_rotation(MENU_ROTATION);
    renderer_clear(COLOR_BLACK);
    renderer_flush();
    for (int k = 0; k < 2; k++) {
        controls_update();
        (void)controls_button_pressed(BTN_FIRE);
        (void)controls_button_pressed(BTN_NADE);
        (void)controls_button_pressed(BTN_IDX_J1_SW);
        (void)controls_button_pressed(BTN_IDX_J2_SW);
        (void)controls_get_raw_delta(0);
        (void)controls_get_raw_delta_x(0);
    }
}