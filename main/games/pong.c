#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include "pico/stdlib.h"
#include "pong.h"
#include "renderer.h"
#include "controls.h"
#include "highscores.h"
#include "sound.h"

/*
 * Pong -- portado de ArcadePi (https://github.com/JuanLPerea/ArcadePi),
 * mismo concepto (IA adaptativa, subida de nivel, marcador, récords),
 * adaptado a:
 *
 *  - Resolución: campo de juego proporcional a la pantalla real
 *    (320x240 apaisada) en vez de los 768x576 del vídeo compuesto.
 *  - Controles: controls_get_raw_delta() para el movimiento continuo
 *    de las palas (con inercia) y controls_button_pressed() para los
 *    botones, en vez de los globales de interrupción de ArcadePi
 *    (enc1_count, btn1_pressed...).
 *  - Render incremental: en vez de limpiar toda la pantalla cada
 *    frame (inasumible por SPI a este tamaño), se borra solo el
 *    rectángulo anterior de cada objeto que se mueve y se dibuja el
 *    nuevo -- igual que ya hace menu.c para sus animaciones.
 *  - Bucle propio: ArcadePi registra callbacks de dibujo/tick sobre
 *    un bucle principal común dirigido por timer; aquí game_pong_run()
 *    es una función que contiene su PROPIO bucle y no devuelve hasta
 *    que la partida (o la demo) termina, como el resto de juegos de
 *    ArcadeColor.
 *  - Sin sonido por ahora: ArcadeColor no tiene aún un driver de
 *    audio (solo el pin GP4 de salida PWM reservado). Los puntos
 *    donde ArcadePi reproducía efectos están marcados con TODO.
 *  - Entrada de iniciales: en vez de la máquina de estados no
 *    bloqueante de ArcadePi (S_ENTER_NAME + hs_input_tick), se usa
 *    la highscores_enter() bloqueante que ya tenemos, que hace su
 *    propio bucle de dibujo/espera internamente.
 */

// ---------------------------------------------------------------------------
// Área de juego, proporcional a la pantalla real (320x240 apaisada)
// ---------------------------------------------------------------------------
// Literales fijos (no TFT_WIDTH/TFT_HEIGHT): esos son en realidad
// st7789_screen_w/h, variables en tiempo de ejecución (cambian según
// la rotación) -- no sirven como inicializador de AI_BASE_VISION[],
// que es un array static const y exige constantes reales en tiempo
// de compilación. La rotación está fijada a 320x240 en st7789_init()
// y no cambia en marcha, así que hardcodear aquí es seguro; si algún
// día cambiara la rotación del proyecto, hay que actualizar esto.
#define SCREEN_W 320
#define SCREEN_H 240

#define PLAY_X   4
#define PLAY_Y   3
#define PLAY_W   (SCREEN_W - 2 * PLAY_X)                 // 312
#define PLAY_H   (SCREEN_H - 2 * PLAY_Y)                 // 234
#define CX       (PLAY_X + PLAY_W / 2)
#define CY       (PLAY_Y + PLAY_H / 2)

#define PADDLE_W      6
#define PADDLE_MARGIN 6
#define BALL_SZ       8

// Cada pala en un color distinto, para diferenciarlas de un vistazo
// (sobre todo en 2 jugadores). La bola se queda en blanco.
#define COLOR_P1 COLOR_CYAN
#define COLOR_P2 COLOR_YELLOW

// Tamaño de pala del jugador según su nivel (se reduce cada 15 puntos).
// Mismas proporciones que ArcadePi (72/56/44/34 sobre un campo de 400px
// de alto), aplicadas al alto real del campo aquí.
#define PADDLE_H      (PLAY_H * 72 / 400)   // pala fija de la IA / P2
#define PADDLE_H_LV0  (PLAY_H * 72 / 400)
#define PADDLE_H_LV1  (PLAY_H * 56 / 400)
#define PADDLE_H_LV2  (PLAY_H * 44 / 400)
#define PADDLE_H_LV3  (PLAY_H * 34 / 400)

/*
 * Movimiento de la bola: velocidad (magnitud) + ángulo, NO bx/by
 * sueltos e independientes. Con dos componentes independientes, la
 * velocidad diagonal real es sqrt(bx²+by²) -- con by pudiendo llegar
 * casi tan alto como bx, una bola muy angulada se movía hasta 9x más
 * rápido en diagonal que una plana. Aquí la magnitud (BALL_SPEED_*)
 * se mantiene constante para un ángulo dado, y solo sube un poco en
 * cada rebote en pala (como antes), no según el ángulo.
 */
#define BALL_BASE_SPEED  2.4f   // velocidad (px/tick) al sacar
#define BALL_SPEED_INC   0.35f  // cuánto sube la velocidad en cada rebote en pala
#define BALL_SPEED_MAX   6.0f
#define BALL_MAX_ANGLE   1.0f   // radianes (~57°), ángulo máximo en rebote de pala
#define BALL_SERVE_ANGLE 0.35f  // radianes (~20°), ángulo máximo al sacar (más plano)
#define BALL_DEMO_MIN_ANGLE  0.30f   // ~17°
#define BALL_DEMO_MAX_ANGLE  0.75f   // ~43°

#define SCORE_WIN     15   // 2P: primero en llegar gana
#define SCORE_AI_WIN  15   // 1P: la IA gana cuando llega a 15

// Control de pala: PAD_ACCEL/PAD_VEL_MAX ajustados para un campo más
// pequeño que el de ArcadePi -- son los primeros valores a tocar si
// la pala se siente demasiado lenta/rápida en tu mando real.
#define PAD_ACCEL     2
#define PAD_VEL_MAX   8
#define PAD_DECAY_NUM 6
#define PAD_DECAY_DEN 10

// "Ticks" = vueltas del bucle principal de este juego, no ms fijos
// (nuestro bucle no está atado a un timer de periodo constante como
// el de ArcadePi). Ajusta estos valores si las pausas se sienten
// demasiado cortas/largas en tu hardware real.
#define PAUSE_TICKS   40
#define BLINK_HALF    14
#define LEVELUP_TICKS (PAUSE_TICKS * 3)
#define DEMO_TIMEOUT_TICKS (PAUSE_TICKS * 30)

// ---------------------------------------------------------------------------
// IA Adaptativa -- idéntico a ArcadePi, no depende de resolución ni de
// la fuente de input, así que se porta literal.
// ---------------------------------------------------------------------------
static const int AI_BASE_SPEED[4]  = { 1, 2, 3, 5 };
#define AI_SPEED_CAP 11
static const int AI_BASE_VISION[4] = {
    PLAY_X + PLAY_W*4/5,
    PLAY_X + PLAY_W*3/5,
    PLAY_X + PLAY_W*2/5,
    0,
};
static const int AI_BASE_ERROR[4] = { 32, 24, 18, 10 };

typedef struct {
    int  avg_center_y;
    int  zone_bias;
    int  streak;
    int  player_total_pts;
    int  ai_total_pts;
    int  ai_error;
    int  ai_error_dir;
    int  ai_speed;
    int  target_y;
    bool target_valid;
} AIProfile;

static AIProfile ai_prof;

typedef enum { S_SELECT, S_SERVE, S_PLAYING, S_PAUSE, S_OVER, S_SCORES } State;
typedef struct { int x, y, bx, by; } Ball;
typedef struct { int x, y, score, acc; } Pad;

static Ball  ball;
static Pad   p1, p2;
static State state;
static float ball_speed; // magnitud actual de la velocidad de la bola (ver set_ball_velocity)
static bool  two_p;
static bool  demo;
static int   demo_ticks;
static int   serve_side;
static int   pause_cnt;
static int   blink;
static int   player_level;
static int   levelup_timer;
static int   paddle_h;
static bool  g_done;
static int   menu_enc_acc = 0;

// Rastro de la última posición dibujada, para el borrado incremental
// (ver draw_playing_frame). -1 = "aún no dibujado, no borrar nada".
static int prev_ball_x = -1, prev_ball_y = -1;
static int prev_p1_y = -1, prev_p2_y = -1;
static int prev_paddle_h = -1;
static int prev_p1_score = -1, prev_p2_score = -1;
static bool field_needs_redraw = true;

// Rastro del texto inferior parpadeante y del mensaje LEVEL UP, para
// redibujarlos solo cuando su contenido realmente cambia (no en cada
// frame mientras el parpadeo está "encendido" -- eso era trabajo, y
// por tanto tráfico SPI, de más).
static char prev_bottom_msg[32] = "";
static bool prev_levelup_shown = false;

// ---------------------------------------------------------------------------
// Minianimación del menú -- un pequeño peloteo (bola + 2 palas en
// miniatura) corriendo en bucle debajo del título de S_SELECT, a modo
// de "logo vivo" mientras el jugador decide 1P/2P. Usa el mismo estilo
// de borrado incremental (erase rect anterior + draw rect nuevo) que
// el resto del juego, así que su coste por tick es mínimo. (Definida
// más abajo, después de clamp()/iabs(), que usa internamente.)
// ---------------------------------------------------------------------------

static int clamp(int v, int lo, int hi) { return v<lo?lo:v>hi?hi:v; }
static int iabs(int v) { return v<0?-v:v; }

#define MENU_ANIM_Y0   (CY-33)
#define MENU_ANIM_Y1   (CY-12)
#define MENU_ANIM_H    (MENU_ANIM_Y1 - MENU_ANIM_Y0)
#define MENU_PAD_W     3
#define MENU_PAD_H     12
#define MENU_BALL_SZ   4
#define MENU_ANIM_X0   (CX-50)                    // borde izq. de la pala izquierda
#define MENU_ANIM_X1   (CX+50-MENU_PAD_W)          // borde izq. de la pala derecha
#define MENU_BALL_MINX (MENU_ANIM_X0+MENU_PAD_W)
#define MENU_BALL_MAXX (MENU_ANIM_X1-MENU_BALL_SZ)

static bool  menu_anim_active = false;
static float menu_ball_x, menu_ball_y, menu_ball_vx, menu_ball_vy;
static int   menu_p1_y, menu_p2_y;
static int   menu_prev_ball_x = -1, menu_prev_ball_y = -1;
static int   menu_prev_p1_y = -1, menu_prev_p2_y = -1;

static void init_menu_anim(void) {
    menu_ball_x = (float)(CX - MENU_BALL_SZ/2);
    menu_ball_y = (float)(MENU_ANIM_Y0 + MENU_ANIM_H/2 - MENU_BALL_SZ/2);
    menu_ball_vx = 1.1f;
    menu_ball_vy = 0.5f;
    menu_p1_y = menu_p2_y = MENU_ANIM_Y0 + MENU_ANIM_H/2 - MENU_PAD_H/2;
    menu_prev_ball_x = menu_prev_ball_y = -1;
    menu_prev_p1_y = menu_prev_p2_y = -1;
    menu_anim_active = true;
}

// Llamado una vez por tick mientras state==S_SELECT. Mueve la bola,
// hace que las dos palas la sigan con un pequeño margen de reacción
// (para que parezca una IA jugando en vez de un objeto que la sigue
// pegada), borra las posiciones anteriores y dibuja las nuevas.
static void menu_anim_tick(void) {
    if (!menu_anim_active) return;

    menu_ball_x += menu_ball_vx;
    menu_ball_y += menu_ball_vy;

    if (menu_ball_y <= MENU_ANIM_Y0 || menu_ball_y >= MENU_ANIM_Y1 - MENU_BALL_SZ)
        menu_ball_vy = -menu_ball_vy;
    if (menu_ball_x <= MENU_BALL_MINX) { menu_ball_x = MENU_BALL_MINX; menu_ball_vx =  fabsf(menu_ball_vx); }
    if (menu_ball_x >= MENU_BALL_MAXX) { menu_ball_x = MENU_BALL_MAXX; menu_ball_vx = -fabsf(menu_ball_vx); }

    int target = (int)menu_ball_y - (MENU_PAD_H-MENU_BALL_SZ)/2;
    target = clamp(target, MENU_ANIM_Y0, MENU_ANIM_Y1-MENU_PAD_H);
    if      (menu_p1_y < target) menu_p1_y++;
    else if (menu_p1_y > target) menu_p1_y--;
    if      (menu_p2_y < target) menu_p2_y++;
    else if (menu_p2_y > target) menu_p2_y--;

    if (menu_prev_ball_x >= 0)
        renderer_fill_rect(menu_prev_ball_x, menu_prev_ball_y, MENU_BALL_SZ, MENU_BALL_SZ, COLOR_BLACK);
    if (menu_prev_p1_y >= 0)
        renderer_fill_rect(MENU_ANIM_X0, menu_prev_p1_y, MENU_PAD_W, MENU_PAD_H, COLOR_BLACK);
    if (menu_prev_p2_y >= 0)
        renderer_fill_rect(MENU_ANIM_X1, menu_prev_p2_y, MENU_PAD_W, MENU_PAD_H, COLOR_BLACK);

    renderer_fill_rect((int)menu_ball_x, (int)menu_ball_y, MENU_BALL_SZ, MENU_BALL_SZ, COLOR_YELLOW);
    renderer_fill_rect(MENU_ANIM_X0, menu_p1_y, MENU_PAD_W, MENU_PAD_H, COLOR_WHITE);
    renderer_fill_rect(MENU_ANIM_X1, menu_p2_y, MENU_PAD_W, MENU_PAD_H, COLOR_WHITE);

    menu_prev_ball_x = (int)menu_ball_x; menu_prev_ball_y = (int)menu_ball_y;
    menu_prev_p1_y = menu_p1_y; menu_prev_p2_y = menu_p2_y;

    renderer_flush();
}

static void pad_move(Pad *p, int dy) {
    int ph = (p == &p1) ? paddle_h : PADDLE_H;
    p->y = clamp(p->y + dy, PLAY_Y, PLAY_Y + PLAY_H - ph);
}

// Sistema de momentum: 'vel' es la velocidad actual de la pala. enc_raw:
// transiciones crudas del encoder este frame (controls_get_raw_delta).
static int enc_momentum(int enc_raw, int *vel) {
    if (enc_raw > 0) {
        *vel += PAD_ACCEL * enc_raw;
        if (*vel > PAD_VEL_MAX) *vel = PAD_VEL_MAX;
    } else if (enc_raw < 0) {
        *vel += PAD_ACCEL * enc_raw;
        if (*vel < -PAD_VEL_MAX) *vel = -PAD_VEL_MAX;
    } else {
        *vel = *vel * PAD_DECAY_NUM / PAD_DECAY_DEN;
    }
    return *vel;
}

static int ai_lv(void) {
    int total = p1.score + p2.score;
    return total >= 20 ? 3 : total >= 12 ? 2 : total >= 5 ? 1 : 0;
}

static int player_paddle_h(void) {
    if (player_level >= 3) return PADDLE_H_LV3;
    if (player_level >= 2) return PADDLE_H_LV2;
    if (player_level >= 1) return PADDLE_H_LV1;
    return PADDLE_H_LV0;
}

static int ai_predict_ball_y(void) {
    int bx = ball.bx, by = ball.by;
    int x  = ball.x,  y  = ball.y;
    int target_x = p2.x;

    if (bx <= 0) return y + BALL_SZ / 2;

    for (int step = 0; step < 200; step++) {
        x += bx;
        y += by;
        if (y <= PLAY_Y)                    { y = PLAY_Y;              by = -by; }
        if (y + BALL_SZ >= PLAY_Y + PLAY_H) { y = PLAY_Y+PLAY_H-BALL_SZ; by = -by; }
        if (x + BALL_SZ >= target_x) return y + BALL_SZ / 2;
    }
    return y + BALL_SZ / 2;
}

static void ai_profile_update(bool player_scored) {
    int lv = ai_lv();

    if (player_scored) {
        ai_prof.streak = ai_prof.streak > 0 ? ai_prof.streak + 1 : 1;
        ai_prof.player_total_pts++;
    } else {
        ai_prof.streak = ai_prof.streak < 0 ? ai_prof.streak - 1 : -1;
        ai_prof.ai_total_pts++;
    }

    int p1_center = p1.y + paddle_h / 2;
    int play_mid  = PLAY_Y + PLAY_H / 2;
    ai_prof.avg_center_y = (ai_prof.avg_center_y * 4 + p1_center) / 5;
    if (p1_center > play_mid + 10)       ai_prof.zone_bias++;
    else if (p1_center < play_mid - 10)  ai_prof.zone_bias--;
    if (ai_prof.zone_bias >  8) ai_prof.zone_bias =  8;
    if (ai_prof.zone_bias < -8) ai_prof.zone_bias = -8;

    int base_err = AI_BASE_ERROR[lv];
    if (ai_prof.streak >= 5)       base_err = base_err * 58 / 100;
    else if (ai_prof.streak >= 4)  base_err = base_err * 70 / 100;
    else if (ai_prof.streak >= 3)  base_err = base_err * 82 / 100;
    else if (ai_prof.streak >= 2)  base_err = base_err * 92 / 100;

    if (lv < 3) {
        if (ai_prof.streak <= -4)       base_err = base_err * 140 / 100;
        else if (ai_prof.streak <= -2)  base_err = base_err * 120 / 100;
    }

    if (base_err < 4)  base_err = 4;
    if (base_err > 80) base_err = 80;
    ai_prof.ai_error = base_err;

    int r = (int)(rand() % 100);
    if (ai_prof.zone_bias > 2)       ai_prof.ai_error_dir = (r < 65) ? -1 : 1;
    else if (ai_prof.zone_bias < -2) ai_prof.ai_error_dir = (r < 65) ?  1 : -1;
    else                             ai_prof.ai_error_dir = (r < 50) ? 1 : -1;

    int spd = AI_BASE_SPEED[lv];
    if (ai_prof.streak >= 3) spd = spd * 115 / 100;
    if (ai_prof.streak >= 5) spd = spd * 120 / 100;
    if (spd > AI_SPEED_CAP) spd = AI_SPEED_CAP;
    ai_prof.ai_speed = spd;

    ai_prof.target_valid = false;
}

static void ai_profile_init(void) {
    ai_prof.avg_center_y  = CY;
    ai_prof.zone_bias     = 0;
    ai_prof.streak        = 0;
    ai_prof.player_total_pts = 0;
    ai_prof.ai_total_pts     = 0;
    ai_prof.ai_error      = AI_BASE_ERROR[0];
    ai_prof.ai_error_dir  = 1;
    ai_prof.ai_speed      = AI_BASE_SPEED[0];
    ai_prof.target_y      = CY;
    ai_prof.target_valid  = false;
}

static void ai_move_adaptive(Pad *p) {
    int lv     = ai_lv();
    int vis_x  = AI_BASE_VISION[lv];
    int speed  = ai_prof.ai_speed;

    if (ball.x < vis_x) {
        int default_y = CY;
        if (ai_prof.zone_bias > 2)       default_y = CY - 30;
        else if (ai_prof.zone_bias < -2) default_y = CY + 30;
        int diff = default_y - (p->y + PADDLE_H / 2);
        if (iabs(diff) > 4)
            pad_move(p, clamp(diff, -2, 2));
        return;
    }

    if (!ai_prof.target_valid) {
        int pred_y = ai_predict_ball_y();
        ai_prof.target_y   = pred_y + ai_prof.ai_error * ai_prof.ai_error_dir;
        ai_prof.target_y   = clamp(ai_prof.target_y, PLAY_Y + PADDLE_H/2, PLAY_Y + PLAY_H - PADDLE_H/2);
        ai_prof.target_valid = true;
    }

    int diff = ai_prof.target_y - (p->y + PADDLE_H / 2);
    if (iabs(diff) <= 1) return;
    pad_move(p, clamp(diff, -speed, speed));
}

// IA simple para el modo demo (ambas palas siguen la pelota sin error).
static void ai_move(Pad *p, int vis_x, int speed) {
    if (ball.x < vis_x) return;
    int diff = (ball.y + BALL_SZ/2) - (p->y + PADDLE_H/2);
    if (iabs(diff) <= 1) return;
    pad_move(p, clamp(diff, -speed, speed));
}

static void reset_pads(void) {
    p1.x = PLAY_X + PADDLE_MARGIN;
    p1.y = CY - paddle_h/2;
    p1.acc = 0;
    p2.x = PLAY_X + PLAY_W - PADDLE_MARGIN - PADDLE_W;
    p2.y = CY - PADDLE_H/2;
    p2.acc = 0;
}

static void ball_centre(void) {
    ball.x = CX - BALL_SZ/2;
    ball.y = CY - BALL_SZ/2;
    ball.bx = ball.by = 0;
}

// Fija bx/by a partir de una magnitud de velocidad y un ángulo (rad),
// en la dirección "dir" (+1 hacia p2, -1 hacia p1). Redondea al
// entero más cercano en cada componente por separado; a estas
// velocidades el redondeo no introduce una variación de magnitud
// perceptible (a diferencia del viejo modelo de bx/by sueltos).
static void set_ball_velocity(float speed, float angle_rad, int dir) {
    float fbx = cosf(angle_rad) * speed;
    float fby = sinf(angle_rad) * speed;

    int bx = (int)(fbx + 0.5f);
    if (bx < 1) bx = 1; // nunca una bola completamente vertical
    ball.bx = dir * bx;

    ball.by = (fby >= 0.0f) ? (int)(fby + 0.5f) : (int)(fby - 0.5f);
}

static void ball_launch(void) {
    ball_speed = BALL_BASE_SPEED;

    float angle;

    if (demo) {
        // En demo: ángulo siempre inclinado y aleatorio.
        // Primero elegimos una magnitud entre MIN y MAX.
        float r = (float)(rand() % 1000) / 1000.0f;
        float magnitude = BALL_DEMO_MIN_ANGLE +
                          r * (BALL_DEMO_MAX_ANGLE - BALL_DEMO_MIN_ANGLE);

        // 50% hacia arriba, 50% hacia abajo.
        if (rand() & 1)
            angle = magnitude;
        else
            angle = -magnitude;
    } else {
        // Partida normal: conserva el saque relativamente plano.
        float r = (float)(rand() % 1000) / 1000.0f;
        angle = (r * 2.0f - 1.0f) * BALL_SERVE_ANGLE;
    }

    int dir = (serve_side == 0) ? 1 : -1;
    set_ball_velocity(ball_speed, angle, dir);
}

static bool hits(Pad *p) {
    int ph = (p == &p1) ? paddle_h : PADDLE_H;
    int hx0 = (p == &p1) ? p->x - 4 : p->x;
    int hx1 = (p == &p1) ? p->x + PADDLE_W : p->x + PADDLE_W + 4;
    return ball.x          < hx1          &&
           ball.x + BALL_SZ > hx0          &&
           ball.y          < p->y + ph     &&
           ball.y + BALL_SZ > p->y;
}

static void bounce(Pad *p, bool left) {
    int ph = (p == &p1) ? paddle_h : PADDLE_H;
    int diff = (ball.y + BALL_SZ/2) - (p->y + ph/2);

    // -1..1 según dónde golpeó en la pala (centro = 0, extremos = ±1)
    float norm = (float)diff / (float)(ph/2);
    if (norm > 1.0f) norm = 1.0f;
    if (norm < -1.0f) norm = -1.0f;
    float angle = norm * BALL_MAX_ANGLE;

    if (!left && !two_p) {
        // Jitter pequeño en el ÁNGULO (no en un componente suelto),
        // así sigue sin afectar a la magnitud de la velocidad.
        float jitter = ((float)(rand() % 100) / 100.0f - 0.5f) * 0.2f; // ±0.1 rad
        angle += jitter;
        if (angle > BALL_MAX_ANGLE)  angle = BALL_MAX_ANGLE;
        if (angle < -BALL_MAX_ANGLE) angle = -BALL_MAX_ANGLE;
    }

    ball_speed += BALL_SPEED_INC;
    if (ball_speed > BALL_SPEED_MAX) ball_speed = BALL_SPEED_MAX;

    set_ball_velocity(ball_speed, angle, left ? 1 : -1);
    ball.x = left ? p->x + PADDLE_W : p->x - BALL_SZ;
    sound_effect_shoot(); // rebote en pala
}

// ---------------------------------------------------------------------------
// Dibujo
// ---------------------------------------------------------------------------
static int centered_x(const char *text, int scale) {
    int w = (int)st7789_text_width(text, (uint8_t)scale);
    int x = (TFT_WIDTH - w) / 2;
    return (x < 0) ? 0 : x;
}

// Campo estático (bordes + línea central punteada). Se dibuja una
// sola vez al entrar a S_SERVE/S_PLAYING, no en cada frame.
static void draw_field_static(void) {
    renderer_clear(COLOR_BLACK);
    renderer_fill_rect(PLAY_X, PLAY_Y,          PLAY_W, 2, COLOR_WHITE);
    renderer_fill_rect(PLAY_X, PLAY_Y+PLAY_H-2, PLAY_W, 2, COLOR_WHITE);
    for (int y = PLAY_Y+4; y < PLAY_Y+PLAY_H-10; y += 16)
        renderer_fill_rect(CX-1, y, 2, 8, COLOR_WHITE);

    prev_ball_x = prev_ball_y = -1;
    prev_p1_y = prev_p2_y = -1;
    prev_paddle_h = -1;
    prev_p1_score = prev_p2_score = -1;
    prev_bottom_msg[0] = '\0';
    prev_levelup_shown = false;
    field_needs_redraw = false;
}

// Devuelve true si dibujó algo (el marcador cambió).
static bool draw_score_if_changed(void) {
    char buf[4];
    bool changed = false;
    if (p1.score != prev_p1_score) {
        renderer_fill_rect(CX-90, PLAY_Y+6, 40, 24, COLOR_BLACK);
        snprintf(buf, sizeof(buf), "%d", p1.score);
        renderer_draw_text(CX-90, PLAY_Y+6, buf, COLOR_WHITE, COLOR_BLACK, 3);
        prev_p1_score = p1.score;
        changed = true;
    }
    if (p2.score != prev_p2_score) {
        renderer_fill_rect(CX+50, PLAY_Y+6, 40, 24, COLOR_BLACK);
        snprintf(buf, sizeof(buf), "%d", p2.score);
        renderer_draw_text(CX+50, PLAY_Y+6, buf, COLOR_WHITE, COLOR_BLACK, 3);
        prev_p2_score = p2.score;
        changed = true;
    }
    return changed;
}

// Frame de S_SERVE/S_PLAYING: borra solo lo que se movió y dibuja lo
// nuevo -- nada de limpiar toda la pantalla cada vuelta.
//
// IMPORTANTE: se hacen DOS flush() separados, no uno. Si el marcador
// o un mensaje parpadeante cambian el mismo tick en que se mueve la
// bola, un único flush() transmitiría el rectángulo que ENVUELVE
// ambas zonas (pueden estar en extremos opuestos de la pantalla),
// disparando el tamaño de esa transmisión y haciendo ese tick mucho
// más lento que el resto -- eso es lo que se percibía como que la
// bola "acelera y frena". Separando los flush, el camino de la
// bola/palas (que se ejecuta CADA tick) se mantiene siempre pequeño
// y constante; el del marcador/mensajes solo añade una transmisión
// extra los pocos ticks en los que de verdad cambia algo.
static void draw_playing_frame(void) {
    if (field_needs_redraw) draw_field_static();

    // --- Camino rápido: bola y palas, cada tick ---
    //
    // Un flush() POR OBJETO, no uno combinado: si P1 y la bola se
    // mueven el mismo tick pero están lejos verticalmente, un flush
    // combinado transmitiría el rectángulo que envuelve a ambos --
    // mucho más grande que cualquiera de los dos por separado, y ese
    // tick se nota como un frenazo. Esto es justo lo que hacía que
    // "girar el encoder" (que solo mueve la pala) pareciera afectar
    // a la velocidad de la bola: al moverse juntas en el mismo tick,
    // el flush de la bola heredaba el tamaño del salto de la pala.
    // Con un flush por objeto, el de la bola es SIEMPRE del tamaño
    // de la bola, muevas la pala o no.
    if (prev_p1_y != p1.y || prev_paddle_h != paddle_h) {
        if (prev_p1_y >= 0)
            renderer_fill_rect(p1.x, prev_p1_y, PADDLE_W,
                                (prev_paddle_h > 0) ? prev_paddle_h : paddle_h, COLOR_BLACK);
        renderer_fill_rect(p1.x, p1.y, PADDLE_W, paddle_h, COLOR_P1);
        prev_p1_y = p1.y;
        prev_paddle_h = paddle_h;
        renderer_flush();
    }
    if (prev_p2_y != p2.y) {
        if (prev_p2_y >= 0)
            renderer_fill_rect(p2.x, prev_p2_y, PADDLE_W, PADDLE_H, COLOR_BLACK);
        renderer_fill_rect(p2.x, p2.y, PADDLE_W, PADDLE_H, COLOR_P2);
        prev_p2_y = p2.y;
        renderer_flush();
    }
    if (prev_ball_x != ball.x || prev_ball_y != ball.y) {
        if (prev_ball_x >= 0)
            renderer_fill_rect(prev_ball_x, prev_ball_y, BALL_SZ, BALL_SZ, COLOR_BLACK);
        renderer_fill_rect(ball.x, ball.y, BALL_SZ, BALL_SZ, COLOR_WHITE);
        prev_ball_x = ball.x;
        prev_ball_y = ball.y;
        renderer_flush();
    }

    // --- Camino lento: marcador y mensajes, solo si cambian ---
    bool ui_changed = draw_score_if_changed();

    bool bon = (blink / BLINK_HALF) % 2 == 0;
    bool levelup_active = (levelup_timer > 0);

    if (levelup_active != prev_levelup_shown) {
        renderer_fill_rect(CX-70, CY-30, 140, 44, COLOR_BLACK);
        if (levelup_active) {
            renderer_draw_text(centered_x("LEVEL UP!", 2), CY-24, "LEVEL UP!", COLOR_YELLOW, COLOR_BLACK, 2);
            char lv_msg[16];
            snprintf(lv_msg, sizeof(lv_msg), "NIVEL %d", player_level);
            renderer_draw_text(centered_x(lv_msg, 1), CY-2, lv_msg, COLOR_WHITE, COLOR_BLACK, 1);
        }
        prev_levelup_shown = levelup_active;
        ui_changed = true;
    }

    // Mensaje inferior: decide qué texto TOCA mostrar ahora mismo
    // (según estado + parpadeo) y solo redibuja si es distinto del
    // último que se dibujó -- no en cada frame mientras se mantiene
    // igual.
    const char *target_msg = "";
    if (state == S_SERVE && !demo && bon)      target_msg = "PULSA PARA SACAR";
    else if (demo && bon)                       target_msg = "DEMO - PULSA PARA JUGAR";

    if (strcmp(target_msg, prev_bottom_msg) != 0) {
        renderer_fill_rect(0, PLAY_Y+PLAY_H-24, TFT_WIDTH, 18, COLOR_BLACK);
        if (target_msg[0]) {
            renderer_draw_text(centered_x(target_msg, 2), PLAY_Y+PLAY_H-22,
                                target_msg, COLOR_WHITE, COLOR_BLACK, 2);
        }
        strncpy(prev_bottom_msg, target_msg, sizeof(prev_bottom_msg) - 1);
        prev_bottom_msg[sizeof(prev_bottom_msg) - 1] = '\0';
        ui_changed = true;
    }

    if (ui_changed) renderer_flush();
}

// Pantallas "estáticas" (se redibujan enteras, pero solo al entrar en
// el estado -- no en cada frame, así que un renderer_clear() aquí es
// barato).
static void draw_select_screen(void) {
    renderer_clear(COLOR_BLACK);
    renderer_draw_text(centered_x("PONG", 3), CY-60, "PONG", COLOR_CYAN, COLOR_BLACK, 3);
    renderer_draw_text(centered_x(two_p ? "- 2 JUGADORES -" : "  2 JUGADORES  ", 2),
                        CY-10, two_p ? "- 2 JUGADORES -" : "  2 JUGADORES  ", COLOR_WHITE, COLOR_BLACK, 2);
    renderer_draw_text(centered_x(!two_p ? "- 1 JUGADOR   -" : "  1 JUGADOR    ", 2),
                        CY+16, !two_p ? "- 1 JUGADOR   -" : "  1 JUGADOR    ", COLOR_WHITE, COLOR_BLACK, 2);
    renderer_draw_text(centered_x("GIRA PARA CAMBIAR - PULSA PARA JUGAR", 1),
                        CY+60, "GIRA PARA CAMBIAR - PULSA PARA JUGAR", COLOR_WHITE, COLOR_BLACK, 1);
    renderer_flush();
    init_menu_anim();
}

static void draw_over_screen(void) {
    renderer_clear(COLOR_BLACK);
    const char *msg;
    if (two_p)
        msg = p1.score >= SCORE_WIN ? "JUGADOR 1 GANA" : "JUGADOR 2 GANA";
    else
        msg = "FIN";
    renderer_draw_text(centered_x(msg, 2), CY-30, msg, COLOR_YELLOW, COLOR_BLACK, 2);

    char sc[20];
    if (two_p) snprintf(sc, sizeof(sc), "%d - %d", p1.score, p2.score);
    else       snprintf(sc, sizeof(sc), "TUS PUNTOS: %d", p1.score);
    renderer_draw_text(centered_x(sc, 2), CY+4, sc, COLOR_WHITE, COLOR_BLACK, 2);

    if (!demo)
        renderer_draw_text(centered_x("PULSA PARA CONTINUAR", 1), CY+40,
                            "PULSA PARA CONTINUAR", COLOR_WHITE, COLOR_BLACK, 1);
    renderer_flush();
}

static void draw_scores_screen(void) {
    renderer_clear(COLOR_BLACK);
    highscores_draw(PONG_GAME_ID, "PONG", 20);
    renderer_flush();
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------
static void pong_tick(void) {
    blink++;

    if (demo) {
        bool any = controls_menu_select()
                || controls_get_raw_delta(0) != 0
                || controls_get_raw_delta(1) != 0;
        if (any || ++demo_ticks >= DEMO_TIMEOUT_TICKS) {
            g_done = true;
            return;
        }
    }

    switch (state) {

    case S_SELECT:

        menu_anim_tick();

        int d = controls_get_raw_delta(0);
         if (d) {
            two_p = !two_p;
            menu_enc_acc += d;
            if (menu_enc_acc >= 2)  { two_p = !two_p; menu_enc_acc = 0; draw_select_screen(); }
            if (menu_enc_acc <= -2) { two_p = !two_p; menu_enc_acc = 0; draw_select_screen(); }
         }

        if (controls_menu_select()) {
            p1.score = p2.score = 0;
            player_level = 0;
            paddle_h = PADDLE_H_LV0;
            levelup_timer = 0;
            serve_side = 0;
            ai_profile_init();
            reset_pads(); ball_centre();
            field_needs_redraw = true;
            sound_stop_menu_music(); // se acaba la música de inicio, empieza la partida
            state = S_SERVE;
        }
        break;

    case S_SERVE:
        pad_move(&p1, enc_momentum(controls_get_raw_delta(0), &p1.acc));
        if (two_p)
            pad_move(&p2, enc_momentum(controls_get_raw_delta(1), &p2.acc));
        if (demo || controls_menu_select() || (!two_p && serve_side==1)) {
            ball_launch(); state = S_PLAYING;
        }
        draw_playing_frame();
        break;

    case S_PLAYING:
        if (demo) {
            ai_move(&p1, 0, 6);
            ai_move(&p2, 0, 6);
        } else {
            pad_move(&p1, enc_momentum(controls_get_raw_delta(0), &p1.acc));
            if (two_p)
                pad_move(&p2, enc_momentum(controls_get_raw_delta(1), &p2.acc));
            else
                ai_move_adaptive(&p2);
        }

        ball.x += ball.bx;
        ball.y += ball.by;

        if (ball.y <= PLAY_Y)                { ball.y = PLAY_Y;              ball.by = -ball.by; ai_prof.target_valid = false; sound_effect_move(); }
        if (ball.y+BALL_SZ >= PLAY_Y+PLAY_H) { ball.y = PLAY_Y+PLAY_H-BALL_SZ; ball.by = -ball.by; ai_prof.target_valid = false; sound_effect_move(); }

        if (ball.bx < 0 && hits(&p1)) { bounce(&p1, true);  ai_prof.target_valid = false; }
        if (ball.bx > 0 && hits(&p2)) { bounce(&p2, false); ai_prof.target_valid = false; }

        if (ball.x + BALL_SZ < PLAY_X) {
            p2.score++;
            sound_effect_explosion(); // se pierde la bola por la izquierda
            if (!two_p && !demo) ai_profile_update(false);
            serve_side = 1; pause_cnt = 0; state = S_PAUSE;
        } else if (ball.x > PLAY_X + PLAY_W) {
            p1.score++;
            sound_effect_explosion(); // se pierde la bola por la derecha
            if (!two_p && !demo) ai_profile_update(true);
            serve_side = 0; pause_cnt = 0; state = S_PAUSE;
        }

        draw_playing_frame();
        break;

    case S_PAUSE:
        if (levelup_timer > 0) levelup_timer--;
        draw_playing_frame();

        if (++pause_cnt >= PAUSE_TICKS) {
            if (!two_p) {
                int new_level = p1.score / 15;
                if (new_level > player_level) {
                    player_level = new_level;
                    paddle_h = player_paddle_h();
                    levelup_timer = LEVELUP_TICKS;
                    sound_effect_success(); // subida de nivel
                }
            }
            if (( two_p && (p1.score >= SCORE_WIN  || p2.score >= SCORE_WIN)) ||
                (!two_p && p2.score >= SCORE_AI_WIN)) {
                sound_stop_menu_music(); // por si acaso siguiera sonando
                if (two_p) {
                    sound_effect_success();  // alguien ha ganado la partida
                } else {
                    sound_effect_game_over(); // en 1P solo se llega aquí si gana la IA
                }
                if (!demo && !two_p && highscores_is_top(PONG_GAME_ID, p1.score)) {
                    highscores_enter(PONG_GAME_ID, (uint32_t)p1.score); // bloqueante
                }
                pause_cnt = 0; state = S_OVER;
                draw_over_screen();
            } else {
                reset_pads(); ball_centre();
                state = S_SERVE;
                field_needs_redraw = true;
            }
        }
        break;

    case S_OVER:
        if (demo) {
            if (++pause_cnt >= PAUSE_TICKS*3) g_done = true;
        } else {
            pause_cnt++;
            if (pause_cnt >= PAUSE_TICKS * 3) {
                if (controls_menu_select()) {
                    pause_cnt = 0; state = S_SCORES;
                    draw_scores_screen();
                } else if (pause_cnt >= PAUSE_TICKS * 10) {
                    pause_cnt = 0; state = S_SCORES;
                    draw_scores_screen();
                }
            }
        }
        break;

    case S_SCORES:
        pause_cnt++;
        if (controls_menu_select()) {
            g_done = true;
        }
        if (pause_cnt >= PAUSE_TICKS * 6) g_done = true;
        break;
    }
}

// ---------------------------------------------------------------------------
// API pública
// ---------------------------------------------------------------------------
void game_pong_run(game_mode_t mode) {
    srand(time_us_32());

    p1.score = p2.score = 0;
    player_level = 0; paddle_h = PADDLE_H_LV0; levelup_timer = 0;
    two_p = (mode == GAME_MODE_2P);
    demo = (mode == GAME_MODE_DEMO);
    demo_ticks = 0;
    serve_side = 0;
    blink = 0;
    g_done = false;
    field_needs_redraw = true;

    ai_profile_init();
    reset_pads();
    ball_centre();

    if (demo) {
        ball_launch();
        state = S_PLAYING;
    } else {
        state = S_SELECT;
        draw_select_screen();
        sound_start_menu_music(); // música de inicio, mientras se elige 1P/2P
    }

    while (!g_done) {
        controls_update();
        pong_tick();
        sound_update();
        sleep_ms(8);
    }

    highscores_flush();
}