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
#include "../renderer.h"
#include "../controls.h"
#include "../sound.h"

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
static bool in_fire_down, in_start_edge, in_quit_edge;
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
typedef enum { ST_TITLE, ST_PLAYING, ST_GAMEOVER } GameState;

typedef struct {
    int32_t x, y;            /* esquina superior izquierda, Q8 */
    int     dir_x, dir_y;    /* dirección de apuntado: -1,0,1 */
    int     fire_cd;
    int     lives;
    int     grenades;
    int     anim;            /* fase de las piernas, Q4 */
    int     invuln;
} Player;

typedef struct {
    bool    active;
    int32_t x, y;            /* Q8, mundo */
    int32_t vx, vy;          /* Q8 por paso */
    int     life;
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

/* ---------------------------------------------------------
 * Inicio de partida
 * --------------------------------------------------------- */
static void game_init(void)
{
    path_generate(level);
    theme = &THEMES[(level - 1) % 3];

    memset(&pl, 0, sizeof(pl));
    pl.x = FX(FIELD_W / 2 - PLAYER_W / 2);
    pl.y = FX(MAP_H - 75);
    pl.dir_x = 0; pl.dir_y = -1;
    pl.lives = 10;
    pl.grenades = 8;
    pl.fire_cd = 0;

    memset(pbullets, 0, sizeof(pbullets));
    cam_q8 = FX(MAP_H - SCR_H);
    score = 0;
    panels_ready = false;
}

/* ---------------------------------------------------------
 * Lógica
 * --------------------------------------------------------- */
static void player_shoot(void)
{
    for (int i = 0; i < MAX_PBULLETS; i++) {
        Bullet *b = &pbullets[i];
        if (b->active) continue;
        int dx = pl.dir_x, dy = pl.dir_y;
        if (dx == 0 && dy == 0) dy = -1;
        int sp = (dx && dy) ? BSPEED_D : BSPEED;
        b->active = true;
        b->x = pl.x + FX(PLAYER_W / 2);
        b->y = pl.y + FX(PLAYER_H / 2 - 2);
        b->vx = dx * sp;
        b->vy = dy * sp;
        b->life = BULLET_LIFE;
        sound_effect_shoot();
        return;
    }
}

static void update_player(void)
{
    int jx = controls_joy_x();
    int jy = controls_joy_y();
    int ax = abs(jx), ay = abs(jy);
    int m = ax > ay ? ax : ay;

    if (m > 0) {
        /* Dirección de apuntado en 8 sentidos: un eje cuenta si supera
         * el 40% del mayor. */
        pl.dir_x = (ax * 5 > m * 2) ? (jx > 0 ? 1 : -1) : 0;
        pl.dir_y = (ay * 5 > m * 2) ? (jy > 0 ? 1 : -1) : 0;

        /* Velocidad proporcional al empuje; en diagonal limitamos la
         * longitud con una aproximación barata de hypot. */
        int32_t vx = (int32_t)jx * PLAYER_SPEED / JOY_MAX;
        int32_t vy = (int32_t)jy * PLAYER_SPEED / JOY_MAX;
        int32_t mag = (ax > ay ? ax : ay) + (ax > ay ? ay : ax) * 3 / 8;
        if (mag > JOY_MAX) { vx = vx * JOY_MAX / mag; vy = vy * JOY_MAX / mag; }

        int32_t nx = pl.x + vx;
        int32_t ny = pl.y + vy;
        int camy = cam_px();

        if (UNFX(nx) >= 3 && UNFX(nx) + PLAYER_W <= FIELD_W - 3) pl.x = nx;
        if (UNFX(ny) >= camy + (SIDE_PANELS ? 0 : HUD_H) + 2 && UNFX(ny) + PLAYER_H <= camy + SCR_H - 8) pl.y = ny;

        pl.anim += 2;
    }

    if (pl.fire_cd > 0) pl.fire_cd--;
    if (pl.invuln > 0) pl.invuln--;

    if (in_fire_down && pl.fire_cd <= 0) {
        player_shoot();
        pl.fire_cd = FIRE_CD;
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
        if (--b->life <= 0 || px < 0 || px >= FIELD_W || py < camy - 10 || py > camy + SCR_H + 10)
            b->active = false;
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
            game_init();
            state = ST_PLAYING;
        }
        return;
    }
    if (state == ST_PLAYING) {
        update_camera();
        update_player();
        update_bullets();
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
 * Medidor de rendimiento (visible en pantalla)
 * --------------------------------------------------------- */
static int perf_fps, perf_draw_ms10, perf_flush_ms10;

#if SIDE_PANELS
#define C_PANEL RGB(0x10, 0x14, 0x08)
#define C_FRAME RGB(0x6a, 0x6a, 0x5a)

static void draw_hud(void)
{
    char buf[24];
    const int rx = FIELD_X + FIELD_W;                 /* inicio del panel derecho */

    if (!panels_ready) {
        rect_abs(0, 0, FIELD_X, SCR_H, C_PANEL);
        rect_abs(rx, 0, SCR_W - rx, SCR_H, C_PANEL);
        rect_abs(FIELD_X - 2, 0, 2, SCR_H, C_FRAME);   /* marco del campo */
        rect_abs(rx, 0, 2, SCR_H, C_FRAME);
        renderer_draw_text(6, 8, "PUNTOS", COLOR_YELLOW, C_PANEL, 1);
        renderer_draw_text(6, SCR_H - 30, "VIDAS", COLOR_YELLOW, C_PANEL, 1);
        panels_ready = true;
    }

    snprintf(buf, sizeof(buf), "%06lu", (unsigned long)score);
    renderer_draw_text(6, 20, buf, COLOR_WHITE, C_PANEL, 1);

    snprintf(buf, sizeof(buf), "%2dFPS", perf_fps);
    renderer_draw_text(rx + 8, 8, buf, COLOR_YELLOW, C_PANEL, 1);
    snprintf(buf, sizeof(buf), "D%2d.%d", perf_draw_ms10 / 10, perf_draw_ms10 % 10);
    renderer_draw_text(rx + 8, 20, buf, COLOR_YELLOW, C_PANEL, 1);
    snprintf(buf, sizeof(buf), "F%2d.%d", perf_flush_ms10 / 10, perf_flush_ms10 % 10);
    renderer_draw_text(rx + 8, 32, buf, COLOR_YELLOW, C_PANEL, 1);

    rect_abs(6, SCR_H - 18, 10 * 5, 4, C_PANEL);      /* borra y repinta las vidas */
    for (int i = 0; i < pl.lives && i < 10; i++)
        rect_abs(6 + i * 5, SCR_H - 18, 3, 4, C_BAND);
}
#else
static void draw_hud(void)
{
    char buf[40];
    rect(0, 0, FIELD_W, HUD_H, RGB(0, 0, 0));
    snprintf(buf, sizeof(buf), "PTS %06lu", (unsigned long)score);
    renderer_draw_text(FIELD_X + 3, 3, buf, COLOR_WHITE, COLOR_BLACK, 1);
    snprintf(buf, sizeof(buf), "%2dFPS D%d.%d F%d.%d", perf_fps,
             perf_draw_ms10 / 10, perf_draw_ms10 % 10,
             perf_flush_ms10 / 10, perf_flush_ms10 % 10);
    renderer_draw_text(FIELD_X + FIELD_W - (int)st7789_text_width(buf, 1) - 3, 3, buf, COLOR_YELLOW, COLOR_BLACK, 1);
    for (int i = 0; i < pl.lives && i < 10; i++)
        rect(3 + i * 5, SCR_H - 6, 3, 4, C_BAND);
}
#endif

static void draw_title(void)
{
    renderer_clear(RGB(0x10, 0x14, 0x08));
    const char *a = "MISSING IN";
    const char *b = "ARCADE";
    const char *c = "PULSA FUEGO";
    renderer_draw_text((SCR_W - (int)st7789_text_width(a, 3)) / 2, SCR_H * 28 / 100, a, RGB(0xf8, 0x48, 0x28), COLOR_BLACK, 3);
    renderer_draw_text((SCR_W - (int)st7789_text_width(b, 4)) / 2, SCR_H * 39 / 100, b, RGB(0xf8, 0xb8, 0x00), COLOR_BLACK, 4);
    if ((title_t / 25) & 1)
        renderer_draw_text((SCR_W - (int)st7789_text_width(c, 2)) / 2, SCR_H * 75 / 100, c, COLOR_WHITE, COLOR_BLACK, 2);
}

static void render(void)
{
    if (state == ST_TITLE) { draw_title(); return; }
    draw_terrain();
    draw_bullets();
    draw_player();
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
    uint64_t demo_t0 = now_us();

    math_init();
    rng_state ^= (uint32_t)now_us();
    if (!rng_state) rng_state = 1;
    state = ST_TITLE;
    g_done = false;
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
    uint64_t draw_acc = 0, flush_acc = 0;
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
        in_start_edge = controls_button_pressed(BTN_FIRE) || controls_button_pressed(BTN_NADE);
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
            frames = 0; draw_acc = flush_acc = 0;
            fps_t0 = t2;
        }

        vTaskDelayUntil(&last_wake, period_ticks ? period_ticks : 1);
    }

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