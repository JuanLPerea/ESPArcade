#include "sound.h"

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "driver/dac_continuous.h"

/* ============================================================
 * PORT A ESP32 de la versión ampliada del sound.c de la Pico.
 *
 * Solo cambia la capa de hardware/temporización:
 *
 *  - Salida: PWM en GP4 + repeating_timer (interrupción a 22050 Hz)
 *    -> DAC continuo con DMA en GPIO25 (DAC1) y una tarea de
 *    FreeRTOS en el core 1 que genera bloques de muestras.
 *  - Sección crítica: save_and_disable_interrupts()/restore_interrupts()
 *    -> portENTER_CRITICAL()/portEXIT_CRITICAL() con un spinlock.
 *  - Temporización: uint32_t / timeout_ms() /
 *    time_reached_ms() -> milisegundos de esp_timer_get_time()
 *    (uint32_t, con comparación segura ante desbordamiento).
 *
 * TODO LO DEMÁS (DDS, formas de onda, mezcla, todas las melodías,
 * el secuenciador y el barrido del canal 3, los efectos) es la
 * MISMA lógica que el sound.c de la Pico: matemáticas y datos puros.
 * ============================================================ */


/* ============================================================
 * CONFIGURACIÓN
 * ============================================================ */

#define SOUND_DAC_GPIO 25   // DAC1 (canal 0 del DAC continuo)

/*
 * Frecuencia de actualización del audio.
 *
 * 22050 Hz es suficiente para efectos y música arcade
 * y mantiene una carga de CPU razonable.
 */
#define AUDIO_SAMPLE_RATE 22050



/*
 * Volumen máximo de cada canal.
 *
 * CANAL1/CANAL2 subidos tras corregir wave_triangle(): con el fallo
 * de escala, la triangular saturaba casi de inmediato y sonaba en
 * la práctica como una cuadrada a plena amplitud (más alta). Ya
 * corregida, es una rampa lineal de verdad, con una amplitud media
 * ~42% menor que una cuadrada del mismo pico (RMS triangular/RMS
 * cuadrada = 1/√3 ≈ 0.577) -- así que con el MISMO volumen sonaba
 * más floja que antes. Estos valores compensan esa diferencia.
 *
 * Es al gusto: súbelos más si los quieres más fuertes (el limitador
 * de la mezcla ya protege contra desbordamiento, aunque a volúmenes
 * muy altos con los 3 canales sonando a la vez notarás algo más de
 * saturación/recorte -- en música chiptune de arcade normalmente no
 * es un problema, hasta le da carácter).
 */
#define CHANNEL1_VOLUME 110
#define CHANNEL2_VOLUME 80
#define CHANNEL3_VOLUME 100

/*
 * Volumen específico de la música de menú y de la música in-game de
 * Tetris (canales 1+2 en ambos casos, WAVE_TRIANGLE/WAVE_TRIANGLE en
 * el menú vs WAVE_TRIANGLE/WAVE_SQUARE en Tetris).
 *
 * Con CHANNEL1/2_VOLUME "a secas" sonaban distinto de volumen aun
 * usando el mismo numero: el bajo de Tetris usa WAVE_SQUARE (RMS
 * pleno), mientras que el menú usa WAVE_TRIANGLE en los dos canales
 * (RMS ~42% menor, ver nota de CHANNEL1/2_VOLUME más arriba) -- de ahí
 * que el menú se oyera más flojo con el mismo volumen nominal.
 *
 * Estas constantes solo se usan en sound_start_menu_music()/
 * sound_start_tetris_music() y en el avance de cada una dentro de
 * sound_update(); CHANNEL1_VOLUME/CHANNEL2_VOLUME se mantienen para
 * el resto de usos de esos canales (intro de Pac-Man, motor, derrape).
 * Los efectos (canal 3, CHANNEL3_VOLUME) no se tocan.
 */
#define MENU_MUSIC_CH1_VOLUME   130   // antes CHANNEL1_VOLUME (110) -- más alto
#define MENU_MUSIC_CH2_VOLUME   100   // antes CHANNEL2_VOLUME (80)  -- más alto
#define TETRIS_MUSIC_CH1_VOLUME  50   // antes CHANNEL1_VOLUME (110) -- más bajo
#define TETRIS_MUSIC_CH2_VOLUME  55   // antes CHANNEL2_VOLUME (80)  -- más bajo


/* ============================================================
 * FORMAS DE ONDA
 * ============================================================ */

#define WAVE_SQUARE   0
#define WAVE_TRIANGLE 1
#define WAVE_SAW      2
#define WAVE_NOISE    3


/* ============================================================
 * CANAL DE AUDIO
 * ============================================================ */

typedef struct {
    volatile bool active;

    uint32_t phase;
    uint32_t phase_increment;

    uint16_t frequency;

    uint16_t volume;

    uint8_t waveform;

    uint32_t noise_state;
} audio_channel_t;


/* ============================================================
 * HARDWARE
 * ============================================================ */

static dac_continuous_handle_t s_dac;

/* Spinlock de ESP-IDF: equivalente a save_and_disable_interrupts()/
 * restore_interrupts() del Pico SDK. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
#define SOUND_ENTER_CRITICAL() portENTER_CRITICAL(&s_lock)
#define SOUND_EXIT_CRITICAL()  portEXIT_CRITICAL(&s_lock)

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Instante "dentro de ms milisegundos" y comprobación de si ya pasó.
 * La resta con signo es correcta aunque now_ms() dé la vuelta. */
static inline uint32_t timeout_ms(uint32_t ms)
{
    return now_ms() + ms;
}

static inline bool time_reached_ms(uint32_t t)
{
    return (int32_t)(now_ms() - t) >= 0;
}

static volatile bool sound_initialized = false;
static volatile bool sound_enabled = true;


/* ============================================================
 * CANALES
 * ============================================================ */

static volatile audio_channel_t channel1;
static volatile audio_channel_t channel2;
static volatile audio_channel_t channel3;


/* ============================================================
 * MÚSICA
 * GREENSLEEVES
 *
 * Adaptación de la partitura:
 * - Tonalidad: Mi menor
 * - Compás: 6/4
 * - Extensión: 16 compases
 * - Adaptada a sintetizador de 2 canales
 * ============================================================ */

#define MUSIC_NOTE_COUNT 75

#define NOTE_C3  131
#define NOTE_D3  147
#define NOTE_E3  165
#define NOTE_F3  175
#define NOTE_G3  196
#define NOTE_A3  220
#define NOTE_B3  247

#define NOTE_C4  262
#define NOTE_D4  294
#define NOTE_E4  330
#define NOTE_F4  349
#define NOTE_G4  392
#define NOTE_A4  440
#define NOTE_B4  494

#define NOTE_C5  523
#define NOTE_D5  587
#define NOTE_E5  659
#define NOTE_F5  698
#define NOTE_G5  784
#define NOTE_A5  880
#define NOTE_B5  988

#define NOTE_GS4 415
#define NOTE_CS5 554
#define NOTE_FS5 740
#define NOTE_DS4 311


/*
 * Canal 1: Melodía principal
 *
 * Greensleeves
 * Tonalidad: Mi menor
 * Compás: 6/4
 *
 * Las notas siguen la línea melódica de la guitarra
 * de la partitura original.
 */
static const uint16_t melody_notes[MUSIC_NOTE_COUNT] = {

    // ========================================================
    // COMPÁS 1
    // ========================================================
    NOTE_E4, NOTE_G4, NOTE_A4, NOTE_B4,
    NOTE_C5, NOTE_B4,

    // ========================================================
    // COMPÁS 2
    // ========================================================
    NOTE_A4, NOTE_F4, NOTE_D4,
    NOTE_E4, NOTE_F4,

    // ========================================================
    // COMPÁS 3
    // ========================================================
    NOTE_G4, NOTE_F4, NOTE_E4,
    NOTE_DS4, NOTE_E4,

    // ========================================================
    // COMPÁS 4
    // ========================================================
    NOTE_F4, NOTE_DS4, NOTE_B3,
    NOTE_E4,

    // ========================================================
    // COMPÁS 5
    // ========================================================
    NOTE_E4, NOTE_G4, NOTE_A4, NOTE_B4,
    NOTE_C5, NOTE_B4,

    // ========================================================
    // COMPÁS 6
    // ========================================================
    NOTE_A4, NOTE_F4, NOTE_D4,
    NOTE_E4, NOTE_F4,

    // ========================================================
    // COMPÁS 7
    // ========================================================
    NOTE_G4, NOTE_F4, NOTE_E4,
    NOTE_DS4, NOTE_E4,

    // ========================================================
    // COMPÁS 8
    // ========================================================
    NOTE_F4, NOTE_E4, NOTE_D4,
    0,

    // ========================================================
    // COMPÁS 9
    // ========================================================
    NOTE_D5, NOTE_D5, NOTE_C5, NOTE_B4,

    // ========================================================
    // COMPÁS 10
    // ========================================================
    NOTE_A4, NOTE_F4, NOTE_D4,
    NOTE_E4, NOTE_F4,

    // ========================================================
    // COMPÁS 11
    // ========================================================
    NOTE_G4, NOTE_E4, NOTE_E4,
    NOTE_DS4, NOTE_E4,

    // ========================================================
    // COMPÁS 12
    // ========================================================
    NOTE_F4, NOTE_DS4, NOTE_B3,
    0,

    // ========================================================
    // COMPÁS 13
    // ========================================================
    NOTE_D5, NOTE_D5, NOTE_C5, NOTE_B4,

    // ========================================================
    // COMPÁS 14
    // ========================================================
    NOTE_A4, NOTE_F4, NOTE_D4,
    NOTE_E4, NOTE_F4,

    // ========================================================
    // COMPÁS 15
    // ========================================================
    NOTE_G4, NOTE_F4, NOTE_E4,
    NOTE_DS4, NOTE_E4,

    // ========================================================
    // COMPÁS 16 - FINAL
    // ========================================================
    NOTE_E4, NOTE_E4
};


/*
 * Duraciones de la melodía (ms)
 *
 * Referencia:
 *
 * 150 ms = corchea
 * 300 ms = negra
 * 450 ms = negra con puntillo
 * 600 ms = blanca
 * 900 ms = blanca con puntillo
 *
 * El último compás se alarga para simular el ritardando.
 */
static const uint16_t melody_duration[MUSIC_NOTE_COUNT] = {

    // ========================================================
    // COMPÁS 1
    // ========================================================
    600, 300, 300, 450,
    150, 300,

    // ========================================================
    // COMPÁS 2
    // ========================================================
    600, 300, 450,
    150, 300,

    // ========================================================
    // COMPÁS 3
    // ========================================================
    600, 300, 450,
    150, 300,

    // ========================================================
    // COMPÁS 4
    // ========================================================
    600, 300, 600,
    300,

    // ========================================================
    // COMPÁS 5
    // ========================================================
    600, 300, 300, 450,
    150, 300,

    // ========================================================
    // COMPÁS 6
    // ========================================================
    600, 300, 450,
    150, 300,

    // ========================================================
    // COMPÁS 7
    // ========================================================
    450, 150, 300,
    450, 150, 300,

    // ========================================================
    // COMPÁS 8
    // ========================================================
    900, 600,
    300,

    // ========================================================
    // COMPÁS 9
    // ========================================================
    900, 450, 150, 300,

    // ========================================================
    // COMPÁS 10
    // ========================================================
    600, 300, 450,
    150, 300,

    // ========================================================
    // COMPÁS 11
    // ========================================================
    600, 300, 450,
    150, 300,

    // ========================================================
    // COMPÁS 12
    // ========================================================
    600, 300, 600,
    300,

    // ========================================================
    // COMPÁS 13
    // ========================================================
    900, 450, 150, 300,

    // ========================================================
    // COMPÁS 14
    // ========================================================
    600, 300, 450,
    150, 300,

    // ========================================================
    // COMPÁS 15
    // ========================================================
    450, 150, 300,
    450, 150, 300,

    // ========================================================
    // COMPÁS 16 - RITARDANDO / FINAL
    // ========================================================
    900, 1400
};


/*
 * Canal 2: Acompañamiento / Bajo
 *
 * Simplificado a partir de la armonía del piano.
 *
 * Se mantiene deliberadamente sencillo para que el canal
 * de melodía sea el protagonista.
 */
static const uint16_t bass_notes[MUSIC_NOTE_COUNT] = {

    // ========================================================
    // COMPÁS 1 - Em
    // ========================================================
    NOTE_E3, 0,       NOTE_B3, 0,
    NOTE_E3, 0,

    // ========================================================
    // COMPÁS 2 - D / Am
    // ========================================================
    NOTE_A3, 0,       NOTE_D3,
    0,       NOTE_A3,

    // ========================================================
    // COMPÁS 3 - Em
    // ========================================================
    NOTE_E3, 0,       NOTE_B3,
    0,       NOTE_E3,

    // ========================================================
    // COMPÁS 4 - B7 / Em
    // ========================================================
    NOTE_B3, 0,       NOTE_B3,
    NOTE_E3,

    // ========================================================
    // COMPÁS 5 - Em
    // ========================================================
    NOTE_E3, 0,       NOTE_B3, 0,
    NOTE_E3, 0,

    // ========================================================
    // COMPÁS 6 - D / Am
    // ========================================================
    NOTE_A3, 0,       NOTE_D3,
    0,       NOTE_A3,

    // ========================================================
    // COMPÁS 7 - Em / B7
    // ========================================================
    NOTE_E3, 0,       NOTE_B3,
    0,       NOTE_B3, 0,

    // ========================================================
    // COMPÁS 8 - Em
    // ========================================================
    NOTE_E3, 0,
    0,

    // ========================================================
    // COMPÁS 9 - G
    // ========================================================
    NOTE_G3, 0,       NOTE_D4, NOTE_G3,

    // ========================================================
    // COMPÁS 10 - D
    // ========================================================
    NOTE_D3, 0,       NOTE_A3,
    0,       NOTE_D3,

    // ========================================================
    // COMPÁS 11 - Em
    // ========================================================
    NOTE_E3, 0,       NOTE_B3,
    0,       NOTE_E3,

    // ========================================================
    // COMPÁS 12 - B7
    // ========================================================
    NOTE_B3, 0,       NOTE_B3,
    0,

    // ========================================================
    // COMPÁS 13 - G
    // ========================================================
    NOTE_G3, 0,       NOTE_D4, NOTE_G3,

    // ========================================================
    // COMPÁS 14 - D
    // ========================================================
    NOTE_D3, 0,       NOTE_A3,
    0,       NOTE_D3,

    // ========================================================
    // COMPÁS 15 - B7
    // ========================================================
    NOTE_B3, 0,       NOTE_B3,
    0,       NOTE_E3, 0,

    // ========================================================
    // COMPÁS 16 - Em / FINAL
    // ========================================================
    NOTE_E3, 0
};


/* ============================================================
 * MÚSICA DE MENÚ -- PISTAS ADICIONALES
 *
 * Además de Greensleeves (pista 0, los arrays melody_notes/
 * bass_notes/melody_duration de arriba), el menú puede sonar con
 * dos temas más de estilo arcade, compuestos para este proyecto.
 * Los tres comparten formato: melodía, bajo y duración con el
 * MISMO número de eventos, así que un único índice (music_index)
 * vale para los tres canales de datos.
 *
 * La pista activa se elige con sound_set_menu_track() /
 * sound_next_menu_track() -- ver la tabla menu_tracks[] más abajo.
 * ============================================================ */

/*
 * Pista 1 -- "NEON CIRCUIT"
 * La menor, 16 compases de corcheas a 150 BPM (200ms por evento,
 * 12.8s de bucle). Arpegios ascendentes sobre Am-F-C-G con un giro
 * a Mi mayor en el compás 15, al estilo de las melodías de ataque
 * de los arcades de los 80.
 */
#define NEON_TRACK_LEN 64
static const uint16_t neon_melody[NEON_TRACK_LEN] = {
     440,  523,  659,  523,  440,  659,  880,  659,
     349,  440,  523,  440,  523,  440,  349,    0,
     523,  659,  784,  659,  784,  659,  523,  659,
     494,  587,  784,  587,  784,  587,  494,    0,
     880,  784,  659,  523,  880,  698,  523,  440,
     784,  659,  523,  659,  494,  587,  784,  988,
     880,  659,  440,  659,  698,  523,  440,  523,
     494,  659,  831,  659,  440,    0,  440,    0,
};
static const uint16_t neon_bass[NEON_TRACK_LEN] = {
     110,  110,   82,  110,  110,  110,   82,  110,
      87,   87,   65,   87,   87,   87,   65,   87,
     131,  131,   98,  131,  131,  131,   98,  131,
      98,   98,   73,   98,   98,   98,   73,   98,
     110,  110,   82,  110,   87,   87,   65,   87,
     131,  131,   98,  131,   98,   98,   73,   98,
     110,  110,   82,  110,   87,   87,   65,   87,
      82,   82,   62,   82,  110,  110,   82,  110,
};
static const uint16_t neon_dur[NEON_TRACK_LEN] = {
     200,  200,  200,  200,  200,  200,  200,  200,
     200,  200,  200,  200,  200,  200,  200,  200,
     200,  200,  200,  200,  200,  200,  200,  200,
     200,  200,  200,  200,  200,  200,  200,  200,
     200,  200,  200,  200,  200,  200,  200,  200,
     200,  200,  200,  200,  200,  200,  200,  200,
     200,  200,  200,  200,  200,  200,  200,  200,
     200,  200,  200,  200,  200,  200,  200,  200,
};

/*
 * Pista 2 -- "STAR PATROL"
 * Do mayor, más rápida (150ms por evento, 9.6s de bucle) y con la
 * melodía una octava más arriba que NEON CIRCUIT: suena a marcha
 * espacial/fanfarria, para contrastar con el tono menor de la otra.
 */
#define STAR_TRACK_LEN 64
static const uint16_t star_melody[STAR_TRACK_LEN] = {
     523,  659,  784, 1047,  784,  659,  523,  659,
     587,  784,  988,  784,  587,  494,  392,  494,
     440,  523,  659,  880,  659,  523,  440,  523,
     698,  880, 1047,  880,  784,  988, 1175,    0,
    1047,  988,  880,  784,  659,  784,  988,  784,
     698,  880,  698,  523,  784,  587,  494,  587,
     523,  659,  784, 1047,  494,  587,  784,  587,
    1047,  784,  659,  523,  523,    0,  523,    0,
};
static const uint16_t star_bass[STAR_TRACK_LEN] = {
     131,  131,   98,  131,  131,  131,   98,  131,
      98,   98,   73,   98,   98,   98,   73,   98,
     110,  110,   82,  110,  110,  110,   82,  110,
      87,   87,   65,   87,   98,   98,   73,   98,
     131,  131,   98,  131,   82,   82,   62,   82,
      87,   87,   65,   87,   98,   98,   73,   98,
     131,  131,   98,  131,   98,   98,   73,   98,
     131,  131,   98,  131,  131,  131,   98,  131,
};
static const uint16_t star_dur[STAR_TRACK_LEN] = {
     150,  150,  150,  150,  150,  150,  150,  150,
     150,  150,  150,  150,  150,  150,  150,  150,
     150,  150,  150,  150,  150,  150,  150,  150,
     150,  150,  150,  150,  150,  150,  150,  150,
     150,  150,  150,  150,  150,  150,  150,  150,
     150,  150,  150,  150,  150,  150,  150,  150,
     150,  150,  150,  150,  150,  150,  150,  150,
     150,  150,  150,  150,  150,  150,  150,  150,
};

/*
 * Tabla de pistas del menú. La 0 es Greensleeves (arrays de arriba).
 * Todas se recorren igual, con music_index y un solo temporizador.
 */
typedef struct {
    const uint16_t *melody;
    const uint16_t *bass;
    const uint16_t *durations;
    uint16_t        count;
} MenuTrack;

#define MENU_TRACK_COUNT 3

static const MenuTrack menu_tracks[MENU_TRACK_COUNT] = {
    { melody_notes, bass_notes,  melody_duration, MUSIC_NOTE_COUNT },
    { neon_melody,  neon_bass,   neon_dur,        NEON_TRACK_LEN   },
    { star_melody,  star_bass,   star_dur,        STAR_TRACK_LEN   },
};

static volatile uint8_t menu_track = 0;


/* ============================================================
 * TOCCATA Y FUGA EN RE MENOR, BWV 565 (J. S. BACH)
 *
 * Adaptación del compás de apertura -- el más reconocible de la
 * obra -- para el juego Paratrooper. Dominio público (Bach, 1704).
 *
 * Estructura de la adaptación:
 *   1. Mordente LA-SOL-LA agudo y caída SOL-FA-MI-RE-DO#-RE
 *   2. La misma figura repetida una octava más abajo (el "eco"
 *      entre manuales del órgano original)
 *   3. Figura descendente rápida sobre pedal de RE
 *   4. Arpegio ascendente de séptima disminuida
 *   5. Acorde final de RE menor sostenido
 *
 * Canal 1 lleva la voz superior y canal 2 la inferior a distancia
 * de octava (así suena a registro de órgano lleno). Igual que el
 * jingle de inicio de Pac-Man, es de UNA SOLA PASADA (no en bucle,
 * ~8.6s) -- suena una vez al empezar la partida y luego se apaga
 * sola, dejando sonar solo los efectos del canal 3. Los dos canales
 * comparten el MISMO array de duraciones, así que basta un índice
 * para los dos y no pueden desfasarse.
 * ============================================================ */
#define TOCCATA_LEN 43
static const uint16_t toccata_ch1_freq[TOCCATA_LEN] = {
     880,  784,  880,    0,  784,  698,  659,  587,
     554,  587,    0,  440,  392,  440,    0,  392,
     349,  330,  294,  277,  294,    0,  587,  554,
     587,  659,  587,  554,  587,    0,  294,  349,
     440,  523,  587,  698,  880,    0,  587,    0,
     698,  587,    0,
};
static const uint16_t toccata_ch2_freq[TOCCATA_LEN] = {
     440,  392,  440,    0,  392,  349,  330,  294,
     277,  294,    0,  220,  196,  220,    0,  196,
     175,  165,  147,  139,  147,    0,  147,  147,
     147,  165,  147,  139,  147,    0,  147,  175,
     220,  262,  294,  349,  440,    0,  147,    0,
     175,  147,    0,
};
static const uint16_t toccata_dur[TOCCATA_LEN] = {
      90,   90,  520,   70,   85,   85,   85,   85,
     300,  430,  130,   90,   90,  520,   70,   85,
      85,   85,   85,  300,  520,  170,   75,   75,
      75,   75,   75,   75,  320,  110,  115,  115,
     115,  115,  115,  115,  420,  130,  720,   90,
     300,  940,  320,
};

/* Volúmenes: el canal 2 casi igual que el 1 -- en el órgano las dos
 * voces suenan a la par, no es un bajo de acompañamiento. */
#define TOCCATA_CH1_VOLUME 115
#define TOCCATA_CH2_VOLUME 100

static volatile bool toccata_playing = false;
static uint16_t toccata_index;
static uint32_t toccata_next;

/* ============================================================
 * ESTADO DE LA MÚSICA
 * ============================================================ */

static volatile bool menu_music_playing = false;

static volatile uint32_t music_index = 0;

static uint32_t music_next_change;

/*
 * Sirena del platillo volante (Space Invaders y lo que la necesite):
 * un silbido continuo de dos tonos que se alternan, típico del OVNI
 * clásico. Usa el CANAL 2 -- libre durante la partida, ya que la
 * música de menú (canales 1+2) se para en cuanto empieza a jugarse
 * (sound_stop_menu_music()) -- así no interfiere con los efectos de
 * disparo/explosión del canal 3, que siguen sonando encima sin
 * cortar la sirena.
 */
static volatile bool channel2_siren_active = false;
static uint8_t channel2_siren_phase = 0;
static uint32_t channel2_siren_next_change;

#define SIREN_FREQ_LOW   600
#define SIREN_FREQ_HIGH  900
#define SIREN_STEP_MS     90
#define SIREN_VOLUME      40   // más bajo que los efectos, para que no los tape

/*
 * Motor -- zumbido continuo cuyo tono sube con la velocidad. Comparte
 * el CANAL 2 con la sirena y la música de menú (los tres son "algo
 * continuo de fondo", nunca suenan dos a la vez): quien se active el
 * último se queda el canal, igual que ya hacía sound_start_menu_music()
 * con channel2_siren_active. Pensado para juegos de conducción
 * (Night Driver): se llama sound_engine_set_speed() una vez por frame
 * con la velocidad actual, y sound_engine_stop() al salir del juego.
 */
static volatile bool   channel2_engine_active = false;
static volatile uint16_t channel2_engine_last_freq = 0;

#define ENGINE_FREQ_MIN   55    // ralentí
#define ENGINE_FREQ_MAX  260    // a fondo
#define ENGINE_VOLUME     50    // de fondo -- más flojo que un efecto, no debe tapar el resto

/*
 * Derrape -- ruido continuo mientras el coche patina (velocidad alta +
 * volante girado a fondo, típicamente). Usa el CANAL 1 -- libre
 * durante la partida por el mismo motivo que el canal 2 con la sirena:
 * la música de menú (canales 1+2) ya se ha parado con
 * sound_stop_menu_music() antes de que empiece a jugarse.
 *
 * Es un tono agudo en diente de sierra (WAVE_SAW, la forma más áspera/
 * brillante de las que hay) que además "tiembla" de frecuencia muy
 * deprisa -- channel1_skid_next_change, avanzado desde sound_update()
 * igual que hace la sirena con el canal 2, pero con un paso mucho más
 * corto (SKID_STEP_MS) y un salto de frecuencia aleatorio en vez de
 * alternar entre dos notas fijas. Sin ese temblor sonaría a un pitido
 * limpio y musical; con él suena áspero e inestable, más parecido a un
 * chirrido de neumático de verdad.
 */
static volatile bool channel1_skid_active = false;
static uint32_t channel1_skid_rng = 1;
static uint32_t channel1_skid_next_change;

#define SKID_FREQ_BASE     2000   // centro del chirrido -- agudo
#define SKID_FREQ_JITTER    700   // salto máximo +/- en cada paso
#define SKID_STEP_MS         18   // cada cuánto tiembla -- rápido y áspero
#define SKID_VOLUME           60

/*
 * Expiración del tono genérico de sound_play_tone() (canal 3).
 * Antes, duration_ms se ignoraba por completo y el tono se quedaba
 * sonando indefinidamente. Se comprueba en sound_update(), que ya
 * se llama periódicamente para avanzar la música.
 */
static volatile bool channel3_tone_has_expiry = false;
static uint32_t channel3_tone_expiry;

/*
 * Declaraciones adelantadas: configure_channel() y disable_channel()
 * se definen más abajo en este mismo archivo (junto al resto de la
 * mezcla de audio), pero el secuenciador de aquí las necesita antes.
 * Sin esto, el compilador las trata como declaradas implícitamente
 * (con tipo de retorno "int" por defecto), y luego choca con la
 * definición real más abajo -- error de "static declaration follows
 * non-static declaration".
 */
static void configure_channel(volatile audio_channel_t *channel, uint16_t frequency, uint16_t volume, uint8_t waveform);
static void disable_channel(volatile audio_channel_t *channel);
static void channel_set_frequency(volatile audio_channel_t *channel, uint16_t frequency);

/* ============================================================
 * BARRIDO DE FRECUENCIA (CANAL 3)
 *
 * El secuenciador de más abajo (channel3_seq) salta de nota en
 * nota; esto en cambio desliza la frecuencia de forma continua
 * entre dos valores. Es lo que hace falta para los "pew" de láser,
 * los power-up ascendentes y los teletransportes -- con
 * channel3_seq_start() sonarían a escalera de 4 peldaños en vez de
 * a barrido.
 *
 * Usa channel_set_frequency() en vez de configure_channel() para NO
 * resetear la fase en cada paso: con reset se oiría un chasquido por
 * paso en vez de un deslizamiento limpio (mismo motivo que el
 * chirrido de derrape del canal 1).
 *
 * Comparte canal 3 con channel3_seq y con el tono suelto de
 * sound_play_tone(): los tres se excluyen mutuamente, manda el
 * último en arrancar. Declarado ANTES que channel3_seq_start() (más
 * abajo) porque esa función necesita poder apartar el barrido al
 * arrancar una secuencia -- las funciones channel3_sweep_start()/
 * _update() están más abajo, junto al resto de la lógica de barrido.
 * ============================================================ */
#define CHANNEL3_SWEEP_MAX_STEPS 40

typedef struct {
    bool     active;
    uint16_t freq_start;
    uint16_t freq_end;
    uint8_t  steps;
    uint8_t  index;
    uint16_t step_ms;
    uint32_t next_change;
} Channel3Sweep;

static volatile Channel3Sweep channel3_sweep = { .active = false };

/*
 * Secuenciador simple de canal 3, para melodías cortas no
 * bloqueantes (2-4 notas): "pierdes la bola" descendente, "victoria"
 * ascendente. Igual que channel3_tone_has_expiry, se avanza desde
 * sound_update(). También usado internamente por los sound_effect_*
 * de una sola nota, como apagado automático -- antes NINGUNO de
 * ellos paraba el canal 3 por su cuenta, así que en Pong el tono del
 * último rebote se quedaba sonando sostenido hasta el siguiente, en
 * vez de ser un "beep" corto. Con secuencia de 1 sola nota, es
 * exactamente ese apagado automático.
 */
#define CHANNEL3_SEQ_MAX_NOTES 4

typedef struct {
    uint16_t freqs[CHANNEL3_SEQ_MAX_NOTES];
    uint16_t durations_ms[CHANNEL3_SEQ_MAX_NOTES];
    uint8_t  count;
    uint8_t  index;
    bool     active;
    uint16_t volume;
    uint8_t  waveform;
    uint32_t next_change;
} Channel3Sequence;

static volatile Channel3Sequence channel3_seq = { .active = false };

static void channel3_seq_start(
    const uint16_t *freqs,
    const uint16_t *durations_ms,
    uint8_t count,
    uint16_t volume,
    uint8_t waveform
)
{
    if (count > CHANNEL3_SEQ_MAX_NOTES) {
        count = CHANNEL3_SEQ_MAX_NOTES;
    }

    for (uint8_t i = 0; i < count; i++) {
        channel3_seq.freqs[i] = freqs[i];
        channel3_seq.durations_ms[i] = durations_ms[i];
    }

    channel3_seq.count    = count;
    channel3_seq.index    = 0;
    channel3_seq.volume   = volume;
    channel3_seq.waveform = waveform;
    channel3_seq.active   = true;

    // La secuencia, el temporizador de "un solo tono" y el barrido
    // comparten el canal 3 -- se excluyen mutuamente, solo uno de los
    // tres manda en cada momento.
    channel3_tone_has_expiry = false;
    channel3_sweep.active    = false;

    configure_channel(&channel3, channel3_seq.freqs[0], volume, waveform);
    channel3_seq.next_change = timeout_ms(channel3_seq.durations_ms[0]);
}

static void channel3_seq_update(void)
{
    if (!channel3_seq.active) {
        return;
    }
    if (!time_reached_ms(channel3_seq.next_change)) {
        return;
    }

    channel3_seq.index++;

    if (channel3_seq.index >= channel3_seq.count) {
        channel3_seq.active = false;
        disable_channel(&channel3);
        return;
    }

    configure_channel(
        &channel3,
        channel3_seq.freqs[channel3_seq.index],
        channel3_seq.volume,
        channel3_seq.waveform
    );
    channel3_seq.next_change =
        timeout_ms(channel3_seq.durations_ms[channel3_seq.index]);
}

/* ============================================================
 * BARRIDO DE FRECUENCIA (CANAL 3) -- continuación
 *
 * El tipo Channel3Sweep y la variable channel3_sweep ya están
 * declarados más arriba (antes de channel3_seq_start(), que
 * necesita poder apartar el barrido al arrancar una secuencia).
 * Aquí van las dos funciones que lo manejan.
 * ============================================================ */

static void channel3_sweep_start(
    uint16_t freq_start,
    uint16_t freq_end,
    uint8_t  steps,
    uint16_t step_ms,
    uint16_t volume,
    uint8_t  waveform
)
{
    if (steps < 2) {
        steps = 2;
    }
    if (steps > CHANNEL3_SWEEP_MAX_STEPS) {
        steps = CHANNEL3_SWEEP_MAX_STEPS;
    }

    channel3_sweep.freq_start = freq_start;
    channel3_sweep.freq_end   = freq_end;
    channel3_sweep.steps      = steps;
    channel3_sweep.index      = 0;
    channel3_sweep.step_ms    = step_ms;
    channel3_sweep.active     = true;

    // Los otros dos dueños posibles del canal 3 se apartan.
    channel3_seq.active      = false;
    channel3_tone_has_expiry = false;

    configure_channel(&channel3, freq_start, volume, waveform);
    channel3_sweep.next_change = timeout_ms(step_ms);
}

static void channel3_sweep_update(void)
{
    if (!channel3_sweep.active) {
        return;
    }
    if (!time_reached_ms(channel3_sweep.next_change)) {
        return;
    }

    channel3_sweep.index++;

    if (channel3_sweep.index >= channel3_sweep.steps) {
        channel3_sweep.active = false;
        disable_channel(&channel3);
        return;
    }

    // Interpolación lineal entre freq_start y freq_end. En int32 para
    // que el producto intermedio no desborde con frecuencias altas.
    int32_t f0 = (int32_t)channel3_sweep.freq_start;
    int32_t f1 = (int32_t)channel3_sweep.freq_end;
    int32_t f  = f0 + ((f1 - f0) * (int32_t)channel3_sweep.index)
                      / (int32_t)(channel3_sweep.steps - 1);

    channel_set_frequency(&channel3, (uint16_t)f);
    channel3_sweep.next_change = timeout_ms(channel3_sweep.step_ms);
}

/* Efecto corto de una sola nota, con apagado automático -- usada por
 * todos los sound_effect_* de una nota (shoot/explosion/select/move/
 * game_over/success). Es channel3_seq_start() con una única nota. */
static void channel3_play_short(
    uint16_t freq,
    uint16_t volume,
    uint8_t waveform,
    uint16_t duration_ms
)
{
    uint16_t freqs[1]     = { freq };
    uint16_t durations[1] = { duration_ms };
    channel3_seq_start(freqs, durations, 1, volume, waveform);
}


/* ============================================================
 * UTILIDADES
 * ============================================================ */


/*
 * Convierte una frecuencia en incremento de fase.
 *
 * DDS:
 *
 *     phase += increment
 *
 * La parte alta de phase representa la posición
 * dentro de la onda.
 */
static uint32_t frequency_to_phase_increment(
    uint16_t frequency
)
{
    if (frequency == 0) {
        return 0;
    }

    return (
        (uint32_t)(
            (
                (uint64_t)frequency
                << 32
            ) /
            AUDIO_SAMPLE_RATE
        )
    );
}


/*
 * Configura un canal.
 *
 * CORREGIDO: protegida con SOUND_ENTER_CRITICAL(), como ya
 * hacían sound_start_menu_music()/sound_update(). Antes, si el timer
 * de audio (que lee estos mismos campos ~22050 veces/segundo)
 * interrumpía justo a mitad de esta función, podía leer una muestra
 * con una mezcla de valores viejos/nuevos (p.ej. la forma de onda ya
 * actualizada pero el phase_increment todavía el antiguo) -- un
 * "clic" audible puntual al reconfigurar un canal que ya sonaba
 * (típicamente al retriggerar un efecto). Con esto, cada llamada a
 * configure_channel() (incluyendo todos los sound_effect_*, que
 * antes no tenían ninguna protección) queda atómica frente al timer.
 * Nota: portENTER_CRITICAL()/portEXIT_CRITICAL() (mismo spinlock) se
 * pueden anidar sin problema, así que esto no rompe nada en
 * sound_start_menu_music()/sound_update(), que ya envuelven sus
 * propias llamadas a esta función en su propia protección.
 */
static void configure_channel(
    volatile audio_channel_t *channel,
    uint16_t frequency,
    uint16_t volume,
    uint8_t waveform
)
{
    SOUND_ENTER_CRITICAL();

    if (frequency == 0) {

        channel->active = false;
        channel->frequency = 0;
        channel->phase_increment = 0;

        SOUND_EXIT_CRITICAL();
        return;
    }

    channel->frequency = frequency;

    channel->phase = 0;

    channel->phase_increment =
        frequency_to_phase_increment(
            frequency
        );

    channel->volume = volume;

    channel->waveform = waveform;

    channel->noise_state =
        0x12345678u ^
        ((uint32_t)frequency * 2654435761u);

    if (channel->noise_state == 0) {
        channel->noise_state = 1;
    }

    channel->active = true;

    SOUND_EXIT_CRITICAL();
}


/*
 * Desactiva un canal.
 */
static void disable_channel(
    volatile audio_channel_t *channel
)
{
    channel->active = false;
}


/*
 * Cambia SOLO la frecuencia de un canal ya activo, sin resetear la
 * fase ni tocar volumen/forma de onda -- a diferencia de
 * configure_channel(), que reinicia la fase en cada llamada (pensado
 * para notas discretas: un pequeño "clic" de retrigger no se nota en
 * un beep de 50ms). Para un tono que cambia de frecuencia de forma
 * CONTINUA y gradual -- el motor, actualizado una vez por frame según
 * la velocidad -- resetear la fase en cada actualización metería un
 * chasquido constante: sonaría como un zumbido sucio en vez de un
 * barrido de tono suave. Si el canal no está activo, no hace nada
 * (llama primero a configure_channel() para arrancarlo).
 */
static void channel_set_frequency(
    volatile audio_channel_t *channel,
    uint16_t frequency
)
{
    SOUND_ENTER_CRITICAL();

    if (channel->active) {
        channel->frequency = frequency;
        channel->phase_increment = frequency_to_phase_increment(frequency);
    }

    SOUND_EXIT_CRITICAL();
}


/* ============================================================
 * GENERADORES DE ONDA
 * ============================================================ */


/*
 * Generador cuadrado.
 *
 * Retorna aproximadamente:
 *
 *     -127 ... +127
 */
static int16_t wave_square(
    uint32_t phase
)
{
    if (phase & 0x80000000u) {
        return 127;
    }

    return -127;
}


/*
 * Generador triangular.
 *
 * CORREGIDO: el factor de escala original (*4) hacía que el valor
 * saliera del rango [-127,127] casi de inmediato (en los primeros
 * ~16 valores de p de 32768 posibles), así que el clamp final
 * saturaba la señal enseguida -- sonaba prácticamente como una
 * cuadrada, no como una rampa suave. El factor correcto para una
 * rampa lineal de verdad sobre todo el semiperiodo es 254/32768,
 * no 4.
 */
static int16_t wave_triangle(
    uint32_t phase
)
{
    uint16_t p =
        (uint16_t)(phase >> 16);

    int32_t value;

    if (p < 32768) {
        value =
            -127 +
            (((int32_t)p * 254) / 32768);
    } else {
        value =
            127 -
            ((((int32_t)p - 32768) * 254) / 32768);
    }

    if (value > 127) {
        value = 127;
    }

    if (value < -127) {
        value = -127;
    }

    return (int16_t)value;
}


/*
 * Diente de sierra.
 */
static int16_t wave_saw(
    uint32_t phase
)
{
    return (
        (int16_t)(phase >> 24)
    ) - 128;
}


/*
 * Ruido pseudoaleatorio.
 *
 * LFSR sencillo.
 */
static uint8_t noise_next(
    volatile audio_channel_t *channel
)
{
    uint32_t x =
        channel->noise_state;

    uint32_t bit =
        (
            (x >> 0) ^
            (x >> 2) ^
            (x >> 3) ^
            (x >> 5)
        ) & 1;

    x =
        (x >> 1) |
        (bit << 31);

    channel->noise_state = x;

    return (uint8_t)(x >> 24);
}


/*
 * Obtiene una muestra de un canal.
 */
static int16_t channel_sample(
    volatile audio_channel_t *channel
)
{
    if (!channel->active) {
        return 0;
    }

    int16_t sample = 0;

    switch (channel->waveform) {

        case WAVE_SQUARE:
            sample =
                wave_square(
                    channel->phase
                );
            break;


        case WAVE_TRIANGLE:
            sample =
                wave_triangle(
                    channel->phase
                );
            break;


        case WAVE_SAW:
            sample =
                wave_saw(
                    channel->phase
                );
            break;


        case WAVE_NOISE:
            sample =
                (int16_t)
                    noise_next(channel)
                - 128;
            break;


        default:
            sample = 0;
            break;
    }

    channel->phase +=
        channel->phase_increment;

    return (
        sample *
        (int16_t)channel->volume
    ) / 100;
}


/* ============================================================
 * TAREA DE AUDIO
 *
 * Sustituye a la interrupción de timer de la Pico (que generaba una
 * muestra cada ~45 us). Aquí una tarea de FreeRTOS genera bloques de
 * AUDIO_BLOCK_SAMPLES muestras y se los pasa al DAC con DMA;
 * dac_continuous_write() bloquea hasta que hay hueco, así que la
 * tarea va sola al ritmo de AUDIO_SAMPLE_RATE.
 *
 * Mezcla exactamente igual que la versión de la Pico: se suman los
 * tres canales, se limita a -255..+255 y se pasa a 0..255 alrededor
 * del punto medio 128.
 * ============================================================ */

#define AUDIO_BLOCK_SAMPLES 256

static uint8_t generate_sample_u8(void)
{
    if (!sound_enabled) {
        return 128;
    }

    int32_t sample1 = channel_sample(&channel1);
    int32_t sample2 = channel_sample(&channel2);
    int32_t sample3 = channel_sample(&channel3);

    /* Mezcla de los tres canales. */
    int32_t mixed = sample1 + sample2 + sample3;

    /* Limitador: evita el desbordamiento de la suma. */
    if (mixed > 255) {
        mixed = 255;
    }

    if (mixed < -255) {
        mixed = -255;
    }

    /* -255..+255  ->  0..255 */
    int32_t value = 128 + mixed / 2;

    if (value < 0) {
        value = 0;
    }

    if (value > 255) {
        value = 255;
    }

    return (uint8_t)value;
}

static void audio_task(void *arg)
{
    (void)arg;

    uint8_t block[AUDIO_BLOCK_SAMPLES];

    while (true) {
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
            block[i] = generate_sample_u8();
        }

        dac_continuous_write(s_dac, block, sizeof(block), NULL, portMAX_DELAY);
    }
}


/* ============================================================
 * PAC-MAN -- datos de efectos y jingle de inicio
 *
 * Portados desde el motor de ArcadePi (sound.h/.c de ese proyecto),
 * que los guardaba como arrays {frecuencia,duración_ms} indexados por
 * un enum SoundEffect. Aquí cada efecto es una función propia (ver
 * estilo de sound_effect_lose_point()/sound_effect_victory() más
 * abajo), así que las notas se declaran locales a cada función.
 *
 * NOTA: el secuenciador de canal 3 de este motor (channel3_seq_start)
 * admite como mucho CHANNEL3_SEQ_MAX_NOTES=4 notas por efecto. Los
 * datos originales de ArcadePi traían una nota NOTE_REST final de
 * silencio en cada efecto (p.ej. power pellet: DO-MI-SOL-DO + silencio
 * = 5 notas) -- ese silencio final no hace falta aquí: en cuanto la
 * secuencia termina, channel3_seq_update() ya apaga el canal por su
 * cuenta (ver más abajo), así que se omite y las 4 notas útiles caben
 * justas.
 * ============================================================ */

/*
 * Jingle de inicio de Pac-Man (~4300ms, una vez).
 *
 * Fuente: Pacman_Theme.mid — 220 BPM, ticks_per_beat=384 (mismo
 * origen que usaba ArcadePi). Canal 1 = melodía (triangular, igual
 * que la música de menú); canal 2 = bajo (cuadrada, para que se
 * distinga timbre de la melodía). Los dos canales NO están
 * sincronizados nota a nota (tienen distinto número de eventos:
 * 61 en melodía, 52 en bajo) aunque ambos suman exactamente 4300ms
 * en total -- por eso se avanzan con dos índices/temporizadores
 * independientes en sound_update(), en vez de un único índice
 * compartido como hace la música de menú.
 */
#define PACMAN_INTRO_CH1_LEN 61
#define PACMAN_INTRO_CH2_LEN 52

static const uint16_t pacman_intro_ch1_freq[PACMAN_INTRO_CH1_LEN] = {
    262, 0, 523, 0, 392, 0, 330, 0,
    523, 0, 392, 0, 330, 330, 0, 277,
    0, 554, 0, 415, 0, 349, 0, 554,
    0, 415, 0, 349, 349, 0, 262, 0,
    523, 0, 392, 0, 330, 0, 523, 392,
    0, 330, 330, 0, 330, 349, 370, 0,
    370, 0, 392, 415, 0, 415, 440, 0,
    466, 0, 523, 523, 0,
};
static const uint16_t pacman_intro_ch1_dur[PACMAN_INTRO_CH1_LEN] = {
    68, 68, 68, 69, 68, 68, 68, 68,
    68, 1, 68, 136, 68, 68, 137, 68,
    68, 68, 69, 68, 68, 68, 68, 68,
    1, 68, 136, 68, 68, 137, 68, 68,
    68, 69, 68, 68, 68, 68, 68, 68,
    137, 68, 68, 137, 68, 68, 68, 68,
    68, 1, 68, 68, 68, 68, 68, 1,
    68, 68, 68, 68, 73,
};
static const uint16_t pacman_intro_ch2_freq[PACMAN_INTRO_CH2_LEN] = {
    131, 131, 131, 0, 196, 0, 131, 0,
    131, 131, 0, 196, 0, 139, 139, 139,
    0, 208, 0, 139, 0, 139, 139, 0,
    208, 0, 131, 131, 131, 0, 196, 0,
    131, 131, 0, 131, 0, 196, 0, 196,
    196, 0, 220, 0, 220, 0, 247, 247,
    0, 262, 262, 0,
};
static const uint16_t pacman_intro_ch2_dur[PACMAN_INTRO_CH2_LEN] = {
    68, 68, 68, 205, 68, 68, 68, 1,
    68, 68, 205, 68, 68, 68, 68, 68,
    205, 68, 68, 68, 1, 68, 68, 204,
    68, 69, 68, 68, 68, 205, 68, 68,
    68, 68, 1, 68, 204, 68, 69, 68,
    68, 136, 68, 1, 68, 136, 68, 68,
    137, 68, 68, 73,
};

static volatile bool pacman_intro_active   = false;
static volatile bool pacman_intro_ch1_done = false;
static volatile bool pacman_intro_ch2_done = false;
static uint8_t pacman_intro_ch1_index;
static uint8_t pacman_intro_ch2_index;
static uint32_t pacman_intro_ch1_next;
static uint32_t pacman_intro_ch2_next;


/* ============================================================
 * FANFARRIA DE VICTORIA DE SCRAMBLE (JEFE DESTRUIDO)
 *
 * Melodía original compuesta para este proyecto (no es un port de
 * ningún arcade), pensada para sonar mientras el OVNI jefe estalla
 * al final de cada nivel: arpegio ascendente de Do mayor, subida
 * cromática y resolución en el Do agudo.
 *
 * Misma estructura que el jingle de Pac-Man de arriba: una sola
 * pasada (no bucle), canal 1 = melodía y canal 2 = bajo, con dos
 * índices/temporizadores independientes porque los canales no van
 * sincronizados nota a nota (18 eventos en melodía, 13 en bajo)
 * aunque ambos suman exactamente 3200ms.
 *
 * Canal 3 (efectos) queda libre a propósito: así las explosiones
 * de las partículas pueden seguir sonando por encima de la música.
 * ============================================================ */
#define SCRAMBLE_WIN_CH1_LEN 18
#define SCRAMBLE_WIN_CH2_LEN 13

/* Volúmenes propios: por encima de los de Tetris (es un remate
 * puntual, no música de fondo) pero sin llegar a los del menú. */
#define SCRAMBLE_WIN_CH1_VOLUME 120
#define SCRAMBLE_WIN_CH2_VOLUME  90

static const uint16_t scramble_win_ch1_freq[SCRAMBLE_WIN_CH1_LEN] = {
    /* Arpegio Do mayor ascendente: SOL4-DO5-MI5-SOL5 */
     392,    0,  523,    0,  659,    0,  784,    0,
    /* MI5-SOL5 sostenido */
     659,  784,    0,
    /* Subida LA5-SI5 y resolución en DO6 */
     880,  988, 1047,    0,
    /* Remate: SOL5 corto + DO6 largo */
     784, 1047,    0,
};
static const uint16_t scramble_win_ch1_dur[SCRAMBLE_WIN_CH1_LEN] = {
     100,   20,  100,   20,  100,   20,  300,   40,
     120,  480,   60,
     140,  140,  600,   80,
     120,  700,   60,
};

static const uint16_t scramble_win_ch2_freq[SCRAMBLE_WIN_CH2_LEN] = {
    /* Pedal de DO3 bajo el arpegio, luego SOL3 */
     131,    0,  131,    0,  196,    0,
    /* MI3-FA3-SOL3 acompañando la subida */
     165,  175,  196,    0,
    /* Cadencia final SOL3 -> DO4 */
     196,  262,    0,
};
static const uint16_t scramble_win_ch2_dur[SCRAMBLE_WIN_CH2_LEN] = {
     330,   30,  260,   40,  480,   60,
     300,  140,  600,   80,
     120,  700,   60,
};

static volatile bool scramble_win_active   = false;
static volatile bool scramble_win_ch1_done = false;
static volatile bool scramble_win_ch2_done = false;
static uint8_t scramble_win_ch1_index;
static uint8_t scramble_win_ch2_index;
static uint32_t scramble_win_ch1_next;
static uint32_t scramble_win_ch2_next;


/* ============================================================
 * MÚSICA DE TETRIS (IN-GAME)
 * Korobeiniki ("tema A" de Tetris) — melodía popular rusa de
 * dominio público, en el arreglo chiptune de 2 voces habitual en
 * los clones de Tetris.
 *
 * Igual que con el jingle de Pac-Man, canal 1 = melodía (triangular)
 * y canal 2 = bajo/acompañamiento (cuadrada) -- el canal 3 queda
 * libre para los efectos de la partida (mover, girar, línea, game
 * over...), que son los que ya usa game_tetris_run().
 *
 * A diferencia del jingle de Pac-Man, esto es música de fondo en
 * bucle: los dos canales NO están sincronizados nota a nota (98
 * eventos en melodía, 128 en bajo) pero ambos suman exactamente
 * 37800ms, así que encajan y vuelven a empezar juntos en cada
 * vuelta -- se avanzan con dos índices/temporizadores
 * independientes en sound_update(), igual que el jingle de Pac-Man,
 * solo que al llegar al final se reinicia el índice a 0 en vez de
 * marcarse como terminado.
 */
#define TETRIS_MUSIC_CH1_LEN 98
#define TETRIS_MUSIC_CH2_LEN 128

static const uint16_t tetris_music_ch1_freq[TETRIS_MUSIC_CH1_LEN] = {
    659, 494, 523, 587, 523, 494, 440, 440,
    523, 659, 587, 523, 494, 0, 523, 587,
    659, 523, 440, 440, 0, 587, 698, 880,
    784, 698, 659, 523, 659, 587, 523, 494,
    494, 523, 587, 659, 523, 440, 440, 0,
    659, 494, 523, 587, 523, 494, 440, 440,
    523, 659, 587, 523, 494, 0, 523, 587,
    659, 523, 440, 440, 0, 587, 698, 880,
    784, 698, 659, 523, 659, 587, 523, 494,
    494, 523, 587, 659, 523, 440, 440, 0,
    330, 262, 294, 247, 262, 220, 208, 247,
    0, 330, 262, 294, 247, 262, 330, 440,
    415, 0,
};
static const uint16_t tetris_music_ch1_dur[TETRIS_MUSIC_CH1_LEN] = {
    400, 200, 200, 400, 200, 200, 400, 200,
    200, 400, 200, 200, 400, 200, 200, 400,
    400, 400, 400, 400, 600, 400, 200, 400,
    200, 200, 600, 200, 400, 200, 200, 400,
    200, 200, 400, 400, 400, 400, 400, 400,
    400, 200, 200, 400, 200, 200, 400, 200,
    200, 400, 200, 200, 400, 200, 200, 400,
    400, 400, 400, 400, 600, 400, 200, 400,
    200, 200, 600, 200, 400, 200, 200, 400,
    200, 200, 400, 400, 400, 400, 400, 400,
    800, 800, 800, 800, 800, 800, 800, 400,
    400, 800, 800, 800, 800, 400, 400, 800,
    800, 200,
};
static const uint16_t tetris_music_ch2_freq[TETRIS_MUSIC_CH2_LEN] = {
    494, 415, 440, 494, 659, 587, 440, 415,
    330, 0, 440, 523, 494, 440, 415, 415,
    330, 0, 415, 440, 494, 523, 440, 330,
    330, 0, 147, 349, 440, 523, 523, 523,
    494, 440, 392, 330, 392, 440, 392, 349,
    330, 415, 330, 415, 440, 494, 415, 523,
    415, 440, 523, 330, 330, 330, 0, 494,
    415, 440, 494, 659, 587, 440, 415, 330,
    0, 440, 523, 494, 440, 415, 415, 330,
    0, 415, 440, 494, 523, 440, 330, 330,
    0, 147, 349, 440, 523, 523, 523, 494,
    440, 392, 330, 392, 440, 392, 349, 330,
    415, 330, 415, 440, 494, 415, 523, 415,
    440, 523, 330, 330, 330, 0, 262, 220,
    247, 208, 220, 165, 165, 208, 0, 262,
    220, 247, 208, 220, 262, 330, 294, 0,
};
static const uint16_t tetris_music_ch2_dur[TETRIS_MUSIC_CH2_LEN] = {
    400, 200, 200, 200, 100, 100, 200, 200,
    200, 400, 200, 400, 200, 200, 100, 100,
    100, 100, 200, 200, 400, 400, 400, 400,
    400, 400, 200, 400, 200, 200, 100, 100,
    200, 200, 600, 200, 200, 100, 100, 200,
    200, 200, 200, 200, 200, 200, 200, 200,
    200, 100, 100, 200, 400, 400, 400, 400,
    200, 200, 200, 100, 100, 200, 200, 200,
    400, 200, 400, 200, 200, 100, 100, 100,
    100, 200, 200, 400, 400, 400, 400, 400,
    400, 200, 400, 200, 200, 100, 100, 200,
    200, 600, 200, 200, 100, 100, 200, 200,
    200, 200, 200, 200, 200, 200, 200, 200,
    100, 100, 200, 400, 400, 400, 800, 800,
    800, 800, 800, 800, 800, 400, 400, 800,
    800, 800, 800, 400, 400, 800, 800, 200,
};

static volatile bool tetris_music_playing = false;
static uint8_t tetris_music_ch1_index;
static uint8_t tetris_music_ch2_index;
static uint32_t tetris_music_ch1_next;
static uint32_t tetris_music_ch2_next;


/* ============================================================
 * INICIALIZACIÓN
 * ============================================================ */

void sound_init(void)
{
    if (sound_initialized) {
        return;
    }

    /*
     * DAC continuo con DMA en GPIO25 (DAC_CHANNEL_MASK_CH0).
     */
    dac_continuous_config_t dac_cfg = {
        .chan_mask = DAC_CHANNEL_MASK_CH0,
        .desc_num  = 4,
        .buf_size  = AUDIO_BLOCK_SAMPLES,
        .freq_hz   = AUDIO_SAMPLE_RATE,
        .offset    = 0,
        .clk_src   = DAC_DIGI_CLK_SRC_DEFAULT,
        .chan_mode = DAC_CHANNEL_MODE_SIMUL,
    };

    dac_continuous_new_channels(&dac_cfg, &s_dac);
    dac_continuous_enable(s_dac);


    /*
     * Estado inicial de los canales.
     */
    channel1.active = false;
    channel2.active = false;
    channel3.active = false;


    /*
     * Tarea de audio en el core 1, con prioridad alta para que el
     * DMA no se quede sin datos aunque el juego esté ocupado.
     */
    xTaskCreatePinnedToCore(
        audio_task,
        "sound_task",
        2048,
        NULL,
        configMAX_PRIORITIES - 2,
        NULL,
        1
    );


    sound_initialized = true;
}


/* ============================================================
 * TONO GENÉRICO
 * ============================================================ */

void sound_play_tone(
    uint16_t frequency_hz,
    uint16_t duration_ms
)
{
    if (!sound_initialized) {
        sound_init();
    }

    /*
     * CORREGIDO: antes duration_ms se ignoraba por completo (el
     * tono sonaba indefinidamente) y había una llamada duplicada a
     * configure_channel() que no hacía nada distinto de la primera.
     * Ahora se guarda cuándo debe apagarse y sound_update() lo
     * comprueba en cada vuelta.
     *
     * También cancela cualquier secuencia de canal3_seq que
     * estuviera sonando -- comparten el canal 3, así que si no se
     * cancela, el siguiente sound_update() podría pisar este tono
     * con la nota que tocara de la secuencia.
     */
    channel3_seq.active = false;

    configure_channel(
        &channel3,
        frequency_hz,
        CHANNEL3_VOLUME,
        WAVE_SQUARE
    );

    if (duration_ms > 0) {
        channel3_tone_has_expiry = true;
        channel3_tone_expiry =
            timeout_ms(duration_ms);
    } else {
        channel3_tone_has_expiry = false;
    }
}


/* ============================================================
 * MÚSICA
 * ============================================================ */

void sound_start_menu_music(void)
{
    if (!sound_initialized) {
        sound_init();
    }


    SOUND_ENTER_CRITICAL();


    music_index = 0;

    menu_music_playing = true;

    // La sirena del platillo y el motor también usan el canal 2 -- si
    // por lo que sea estuvieran sonando, la música de menú manda.
    // Lo mismo con la fanfarria de victoria de Scramble (canales 1+2):
    // al volver al menú tras un nivel, la música de menú se impone.
    channel2_siren_active  = false;
    channel2_engine_active = false;
    scramble_win_active    = false;
    toccata_playing        = false;


    configure_channel(
        &channel1,
        menu_tracks[menu_track].melody[0],
        MENU_MUSIC_CH1_VOLUME,
        WAVE_TRIANGLE
    );


    configure_channel(
        &channel2,
        menu_tracks[menu_track].bass[0],
        MENU_MUSIC_CH2_VOLUME,
        WAVE_TRIANGLE
    );


    music_next_change =
        timeout_ms(
            menu_tracks[menu_track].durations[0]
        );


    SOUND_EXIT_CRITICAL();
}


void sound_stop_menu_music(void)
{
    SOUND_ENTER_CRITICAL();


    menu_music_playing = false;

    disable_channel(&channel1);
    disable_channel(&channel2);


    SOUND_EXIT_CRITICAL();
}


/*
 * Esta función es intencionadamente muy ligera.
 *
 * El audio se genera en la interrupción.
 * Aquí solamente avanzamos la secuencia musical.
 */
void sound_update(void)
{
    /*
     * Auto-apagado del tono genérico de sound_play_tone() (ver
     * comentario junto a channel3_tone_has_expiry más arriba).
     */
    if (
        channel3_tone_has_expiry &&
        time_reached_ms(channel3_tone_expiry)
    ) {
        channel3_tone_has_expiry = false;
        disable_channel(&channel3);
    }

    // Avanza la melodía corta de canal 3 (efectos de una nota con
    // apagado automático, o las secuencias de 2-4 notas).
    channel3_seq_update();

    // Avanza el barrido continuo de frecuencia (láser, power-ups...).
    channel3_sweep_update();

    // Avanza el jingle de inicio de Pac-Man (canal 1 + canal 2, dos
    // índices independientes -- ver comentario junto a los arrays
    // pacman_intro_ch1/ch2 más arriba).
    if (pacman_intro_active) {
        SOUND_ENTER_CRITICAL();

        if (!pacman_intro_ch1_done && time_reached_ms(pacman_intro_ch1_next)) {
            pacman_intro_ch1_index++;
            if (pacman_intro_ch1_index >= PACMAN_INTRO_CH1_LEN) {
                pacman_intro_ch1_done = true;
                disable_channel(&channel1);
            } else {
                configure_channel(
                    &channel1,
                    pacman_intro_ch1_freq[pacman_intro_ch1_index],
                    CHANNEL1_VOLUME,
                    WAVE_SQUARE
                );
                pacman_intro_ch1_next =
                    timeout_ms(pacman_intro_ch1_dur[pacman_intro_ch1_index]);
            }
        }

        if (!pacman_intro_ch2_done && time_reached_ms(pacman_intro_ch2_next)) {
            pacman_intro_ch2_index++;
            if (pacman_intro_ch2_index >= PACMAN_INTRO_CH2_LEN) {
                pacman_intro_ch2_done = true;
                disable_channel(&channel2);
            } else {
                configure_channel(
                    &channel2,
                    pacman_intro_ch2_freq[pacman_intro_ch2_index],
                    CHANNEL2_VOLUME,
                    WAVE_SQUARE
                );
                pacman_intro_ch2_next =
                    timeout_ms(pacman_intro_ch2_dur[pacman_intro_ch2_index]);
            }
        }

        if (pacman_intro_ch1_done && pacman_intro_ch2_done) {
            pacman_intro_active = false;
        }

        SOUND_EXIT_CRITICAL();
    }

    // Avanza la fanfarria de victoria de Scramble (canal 1 + canal 2).
    // Igual que el jingle de Pac-Man: una sola pasada, dos índices
    // independientes, y al terminar los dos canales se desactiva sola.
    if (scramble_win_active) {
        SOUND_ENTER_CRITICAL();

        if (!scramble_win_ch1_done && time_reached_ms(scramble_win_ch1_next)) {
            scramble_win_ch1_index++;
            if (scramble_win_ch1_index >= SCRAMBLE_WIN_CH1_LEN) {
                scramble_win_ch1_done = true;
                disable_channel(&channel1);
            } else {
                configure_channel(
                    &channel1,
                    scramble_win_ch1_freq[scramble_win_ch1_index],
                    SCRAMBLE_WIN_CH1_VOLUME,
                    WAVE_TRIANGLE
                );
                scramble_win_ch1_next =
                    timeout_ms(scramble_win_ch1_dur[scramble_win_ch1_index]);
            }
        }

        if (!scramble_win_ch2_done && time_reached_ms(scramble_win_ch2_next)) {
            scramble_win_ch2_index++;
            if (scramble_win_ch2_index >= SCRAMBLE_WIN_CH2_LEN) {
                scramble_win_ch2_done = true;
                disable_channel(&channel2);
            } else {
                configure_channel(
                    &channel2,
                    scramble_win_ch2_freq[scramble_win_ch2_index],
                    SCRAMBLE_WIN_CH2_VOLUME,
                    WAVE_SQUARE
                );
                scramble_win_ch2_next =
                    timeout_ms(scramble_win_ch2_dur[scramble_win_ch2_index]);
            }
        }

        if (scramble_win_ch1_done && scramble_win_ch2_done) {
            scramble_win_active = false;
        }

        SOUND_EXIT_CRITICAL();
    }

    // Avanza la Toccata BWV 565 de Paratrooper (canal 1 + canal 2).
    // Una sola pasada (~8.6s) al empezar la partida, NO en bucle --
    // igual que el jingle de inicio de Pac-Man: al llegar al final se
    // desactiva sola y a partir de ahí solo se oyen los efectos.
    //
    // OJO canal 2: en Paratrooper el aviso del avión usa
    // sound_siren_start()/stop() sobre el CANAL 2, que es el mismo
    // que la voz de bajo de la Toccata -- por si el avión llegara a
    // aparecer durante los ~8.6s que dura. En vez de parar la música
    // entera (cortaría también la melodía del canal 1), aquí se cede
    // el canal 2 a quien lo esté usando (sirena o motor) y solo se
    // reconfigura el canal 1: la melodía sigue sonando sin cortes y
    // el bajo retoma solo, en el índice que toque, en cuanto el avión
    // se calla (ver sound_siren_stop()).
    if (toccata_playing) {
        SOUND_ENTER_CRITICAL();

        if (time_reached_ms(toccata_next)) {
            toccata_index++;

            if (toccata_index >= TOCCATA_LEN) {
                toccata_playing = false;
                disable_channel(&channel1);
                if (!channel2_siren_active && !channel2_engine_active) {
                    disable_channel(&channel2);
                }
            } else {
                configure_channel(
                    &channel1,
                    toccata_ch1_freq[toccata_index],
                    TOCCATA_CH1_VOLUME,
                    WAVE_TRIANGLE
                );
                if (!channel2_siren_active && !channel2_engine_active) {
                    configure_channel(
                        &channel2,
                        toccata_ch2_freq[toccata_index],
                        TOCCATA_CH2_VOLUME,
                        WAVE_SQUARE
                    );
                }

                toccata_next = timeout_ms(toccata_dur[toccata_index]);
            }
        }

        SOUND_EXIT_CRITICAL();
    }

    // Avanza la música in-game de Tetris (canal 1 + canal 2, dos
    // índices independientes -- ver comentario junto a los arrays
    // tetris_music_ch1/ch2 más arriba). A diferencia del jingle de
    // Pac-Man, aquí al llegar al final de cada canal se vuelve al
    // índice 0 en vez de pararse: es música de fondo en bucle.
    if (tetris_music_playing) {
        SOUND_ENTER_CRITICAL();

        if (time_reached_ms(tetris_music_ch1_next)) {
            tetris_music_ch1_index++;
            if (tetris_music_ch1_index >= TETRIS_MUSIC_CH1_LEN) {
                tetris_music_ch1_index = 0;
            }
            configure_channel(
                &channel1,
                tetris_music_ch1_freq[tetris_music_ch1_index],
                TETRIS_MUSIC_CH1_VOLUME,
                WAVE_TRIANGLE
            );
            tetris_music_ch1_next =
                timeout_ms(tetris_music_ch1_dur[tetris_music_ch1_index]);
        }

        if (time_reached_ms(tetris_music_ch2_next)) {
            tetris_music_ch2_index++;
            if (tetris_music_ch2_index >= TETRIS_MUSIC_CH2_LEN) {
                tetris_music_ch2_index = 0;
            }
            configure_channel(
                &channel2,
                tetris_music_ch2_freq[tetris_music_ch2_index],
                TETRIS_MUSIC_CH2_VOLUME,
                WAVE_TRIANGLE
            );
            tetris_music_ch2_next =
                timeout_ms(tetris_music_ch2_dur[tetris_music_ch2_index]);
        }

        SOUND_EXIT_CRITICAL();
    }

    // Avanza el silbido de la sirena del platillo (canal 2).
    if (
        channel2_siren_active &&
        time_reached_ms(channel2_siren_next_change)
    ) {
        channel2_siren_phase ^= 1;
        configure_channel(
            &channel2,
            channel2_siren_phase ? SIREN_FREQ_HIGH : SIREN_FREQ_LOW,
            SIREN_VOLUME,
            WAVE_TRIANGLE
        );
        channel2_siren_next_change = timeout_ms(SIREN_STEP_MS);
    }

    // Avanza el temblor de frecuencia del chirrido de derrape (canal 1)
    // -- xorshift32 minúsculo, no hace falta que sea buen azar, solo
    // que salte de forma poco predecible. channel_set_frequency() en
    // vez de configure_channel() para no resetear la fase en cada
    // salto (18ms es muy seguido -- con reset de fase sonaría a un
    // "tac-tac-tac" en vez de a un chirrido continuo).
    if (
        channel1_skid_active &&
        time_reached_ms(channel1_skid_next_change)
    ) {
        channel1_skid_rng ^= channel1_skid_rng << 13;
        channel1_skid_rng ^= channel1_skid_rng >> 17;
        channel1_skid_rng ^= channel1_skid_rng << 5;

        uint16_t jitter = (uint16_t)(channel1_skid_rng % (SKID_FREQ_JITTER * 2));
        uint16_t freq   = (SKID_FREQ_BASE - SKID_FREQ_JITTER) + jitter;

        channel_set_frequency(&channel1, freq);
        channel1_skid_next_change = timeout_ms(SKID_STEP_MS);
    }

    if (!menu_music_playing) {
        return;
    }


    if (
        !time_reached_ms(
            music_next_change
        )
    ) {
        return;
    }


    SOUND_ENTER_CRITICAL();


     music_index++;

     if (
         music_index >=
         menu_tracks[menu_track].count)      
         {
         music_index = 0;
         menu_music_playing = false;

       SOUND_EXIT_CRITICAL();
       return;
       }


    uint16_t melody =
        menu_tracks[menu_track].melody[music_index];

    uint16_t bass =
        menu_tracks[menu_track].bass[music_index];


    configure_channel(
        &channel1,
        melody,
        MENU_MUSIC_CH1_VOLUME,
        WAVE_TRIANGLE
    );


    configure_channel(
        &channel2,
        bass,
        MENU_MUSIC_CH2_VOLUME,
        WAVE_TRIANGLE
    );


    music_next_change =
        timeout_ms(
            menu_tracks[menu_track].durations[music_index]
        );


    SOUND_EXIT_CRITICAL();
}


bool sound_menu_music_is_playing(void)
{
    return menu_music_playing;
}




/* ============================================================
 * EFECTOS
 * ============================================================ */


/*
 * Disparo: beep corto y agudo.
 *
 * CORREGIDO: antes se dejaba sonando indefinidamente (ningún
 * sound_effect_* tenía apagado automático -- ver el comentario largo
 * junto a channel3_seq arriba). Ahora dura CHANNEL3_SFX_MS y se para
 * sola.
 */
#define CHANNEL3_SFX_MS 55   // duración de los efectos cortos de una nota

void sound_effect_shoot(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    channel3_play_short(1100, CHANNEL3_VOLUME, WAVE_SQUARE, CHANNEL3_SFX_MS);
}


/*
 * Explosión: ráfaga corta de ruido blanco.
 *
 * CORREGIDO: mismo apagado automático.
 */
void sound_effect_explosion(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    channel3_play_short(100, CHANNEL3_VOLUME, WAVE_NOISE, 150);
}


/*
 * Selección del menú.
 *
 * CORREGIDO: mismo apagado automático.
 */
void sound_effect_select(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    channel3_play_short(880, CHANNEL3_VOLUME, WAVE_SQUARE, CHANNEL3_SFX_MS);
}


/*
 * Movimiento del selector / rebote en pared.
 *
 * CORREGIDO: mismo apagado automático. Tono más grave que
 * sound_effect_shoot(), para que se distingan sin mirar la pantalla
 * (p.ej. rebote en pared vs. rebote en pala en Pong).
 */
void sound_effect_move(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    channel3_play_short(660, CHANNEL3_VOLUME, WAVE_SQUARE, CHANNEL3_SFX_MS);
}


/*
 * Game over: un solo tono grave y algo más largo -- distinto de
 * sound_effect_lose_point() (que es la secuencia descendente de
 * "has perdido la bola/punto", más corta y repetible muchas veces
 * por partida).
 *
 * Game over: melodía grave y dramática de 4 notas.
 */
void sound_effect_game_over(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    // Frecuencias: Re4 -> Do#4 -> Do4 -> Si3 (grave)
    static const uint16_t freqs[4]     = { 294, 277, 262, 247 };
    static const uint16_t durations[4] = { 120, 120, 120, 600 };

    channel3_seq_start(freqs, durations, 4, CHANNEL3_VOLUME, WAVE_SAW);
}

/*
 * Éxito puntual (p.ej. subida de nivel) -- una sola nota alegre,
 * corta. Para una victoria de partida completa usa
 * sound_effect_victory() en su lugar (secuencia de 3 notas).
 *
 * CORREGIDO: mismo apagado automático.
 */
/*
 * Éxito puntual (p.ej. subida de nivel / objetivo completado):
 * Arpegio ascendente brillante y alegre de 3 notas (Do5 -> Mi5 -> Sol5).
 */
void sound_effect_success(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    // Tríada mayor ascendente (C5 - E5 - G5)
    static const uint16_t freqs[3]     = { 523, 659, 784 };
    static const uint16_t durations[3] = {  70,  70, 120 };

    channel3_seq_start(freqs, durations, 3, CHANNEL3_VOLUME, WAVE_SQUARE);
}

/*
 * Pierdes la bola / el punto: 4 notas descendentes, cortas.
 * Nuevo efecto -- no existía en el sistema original.
 */
void sound_effect_lose_point(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    static const uint16_t freqs[4]     = { 392, 330, 262, 196 }; // sol-mi-do-sol descendente
    static const uint16_t durations[4] = {  70,  70,  70, 140 };

    channel3_seq_start(freqs, durations, 4, CHANNEL3_VOLUME, WAVE_SQUARE);
}


/*
 * Victoria de partida: fanfarria corta de 3 notas ascendentes.
 * Nuevo efecto -- no existía en el sistema original. Distinto de
 * sound_effect_success() (una sola nota, para eventos menores como
 * subir de nivel).
 */
void sound_effect_victory(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    static const uint16_t freqs[3]     = { 523, 659, 784 }; // do-mi-sol ascendente
    static const uint16_t durations[3] = { 110, 110, 260 };

    channel3_seq_start(freqs, durations, 3, CHANNEL3_VOLUME, WAVE_SQUARE);
}


/*
 * Apaga solamente el canal de efectos.
 *
 * La música continúa.
 */
/* ============================================================
 * EFECTOS AÑADIDOS
 *
 * Los que usan channel3_sweep_start() deslizan la frecuencia de
 * forma continua (ver el motor de barrido más arriba); el resto son
 * secuencias cortas de hasta 4 notas, como los efectos originales.
 * Todos NO BLOQUEANTES y todos sobre el canal 3, así que no pisan
 * la música de fondo de los canales 1+2.
 * ============================================================ */

/*
 * Láser: el "pew" clásico -- caída rápida de agudo a grave en diente
 * de sierra, la forma más áspera que tiene el motor. Corto (~110ms)
 * porque se dispara muchas veces seguidas.
 */
void sound_effect_laser(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    channel3_sweep_start(1900, 320, 16, 7, CHANNEL3_VOLUME, WAVE_SAW);
}


/*
 * Láser grande / cañón cargado: mismo gesto que el láser normal pero
 * más grave, más largo y en cuadrada, para el disparo potenciado o
 * el arma del jefe.
 */
void sound_effect_laser_big(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    channel3_sweep_start(900, 90, 24, 11, CHANNEL3_VOLUME, WAVE_SQUARE);
}


/*
 * Power-up recogido: barrido ascendente amplio en triangular, suena
 * a "mejora conseguida".
 */
void sound_effect_powerup(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    channel3_sweep_start(280, 1700, 22, 13, CHANNEL3_VOLUME, WAVE_TRIANGLE);
}


/*
 * Power-down / escudo perdido: el gesto contrario al anterior,
 * descendente y algo más lento.
 */
void sound_effect_powerdown(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    channel3_sweep_start(1400, 180, 26, 15, CHANNEL3_VOLUME, WAVE_TRIANGLE);
}


/*
 * Moneda / bonus recogido: dos notas rápidas con salto de quinta,
 * el gesto universal de "has cogido algo bueno".
 */
void sound_effect_coin(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    static const uint16_t freqs[2]     = { 988, 1319 };  // si5 -> mi6
    static const uint16_t durations[2] = {  70,  200 };

    channel3_seq_start(freqs, durations, 2, CHANNEL3_VOLUME, WAVE_SQUARE);
}


/*
 * Salto: barrido ascendente corto y seco, en cuadrada.
 */
void sound_effect_jump(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    channel3_sweep_start(380, 950, 10, 8, CHANNEL3_VOLUME, WAVE_SQUARE);
}


/*
 * Impacto recibido: golpe de ruido grave, más corto y seco que
 * sound_effect_explosion() -- "me han dado" en vez de "algo ha
 * estallado".
 */
void sound_effect_hit(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    channel3_play_short(70, CHANNEL3_VOLUME, WAVE_NOISE, 90);
}


/*
 * Alarma / aviso: dos parejas de notas alternando agudo-grave, al
 * estilo de los avisos de combustible bajo o enemigo entrante.
 */
void sound_effect_alarm(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    static const uint16_t freqs[4]     = { 1047, 740, 1047, 740 };
    static const uint16_t durations[4] = {  110, 110,  110, 160 };

    channel3_seq_start(freqs, durations, 4, CHANNEL3_VOLUME, WAVE_SQUARE);
}


/*
 * Teletransporte / aparición: barrido ascendente muy amplio y largo
 * en diente de sierra -- el "whoop" de materialización.
 */
void sound_effect_teleport(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    channel3_sweep_start(150, 2100, 32, 12, CHANNEL3_VOLUME, WAVE_SAW);
}


/*
 * Rebote: dos notas muy cortas subiendo, para pelotas, muelles o
 * proyectiles que rebotan en una pared.
 */
void sound_effect_bounce(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    static const uint16_t freqs[2]     = { 520, 780 };
    static const uint16_t durations[2] = {  45,  55 };

    channel3_seq_start(freqs, durations, 2, CHANNEL3_VOLUME, WAVE_SQUARE);
}


/*
 * Propulsor: soplido corto de ruido grave, para el empuje de una
 * nave o el paracaídas abriéndose. Pensado para repetirse seguido
 * mientras se mantiene el empuje.
 */
void sound_effect_thrust(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    channel3_play_short(45, CHANNEL3_VOLUME / 2, WAVE_NOISE, 70);
}


/*
 * Vida extra: arpegio ascendente de 4 notas (do-mi-sol-do), la
 * fanfarria corta de "1UP". Más larga y resolutiva que
 * sound_effect_victory() (3 notas).
 */
void sound_effect_extra_life(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    static const uint16_t freqs[4]     = { 523, 659, 784, 1047 };
    static const uint16_t durations[4] = {  90,  90,  90,  300 };

    channel3_seq_start(freqs, durations, 4, CHANNEL3_VOLUME, WAVE_TRIANGLE);
}

void sound_effect_stop(void)
{
    channel3_seq.active = false;
    channel3_tone_has_expiry = false;
    channel3_sweep.active = false;

    disable_channel(
        &channel3
    );
}


/*
 * Sirena del platillo volante: silbido continuo de dos tonos
 * alternando (canal 2), hasta llamar a sound_siren_stop(). Pensada
 * para arrancar cuando el platillo aparece en pantalla y pararla
 * cuando se destruye o sale de pantalla.
 */
void sound_siren_start(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    channel2_engine_active = false;   // el canal 2 es de quien lo pida el último
    channel2_siren_active = true;
    channel2_siren_phase  = 0;

    configure_channel(
        &channel2,
        SIREN_FREQ_LOW,
        SIREN_VOLUME,
        WAVE_TRIANGLE
    );

    channel2_siren_next_change = timeout_ms(SIREN_STEP_MS);
}

void sound_siren_stop(void)
{
    channel2_siren_active = false;

    // Si la Toccata de Paratrooper está sonando, el canal 2 es su voz
    // de bajo (ver el "cede el canal" en sound_update()) -- en vez de
    // apagarlo sin más, se retoma la nota que tocaría ahora mismo. En
    // cualquier otro juego toccata_playing es siempre false, así que
    // esto no cambia nada del comportamiento de antes.
    if (toccata_playing) {
        configure_channel(
            &channel2,
            toccata_ch2_freq[toccata_index],
            TOCCATA_CH2_VOLUME,
            WAVE_SQUARE
        );
        return;
    }

    disable_channel(
        &channel2
    );
}


/*
 * Motor -- zumbido continuo (canal 2) cuyo tono sube con la
 * velocidad. Pensada para llamarse una vez por frame con la
 * velocidad actual del juego: "speed_pct" va de 0 (ralentí) a 255
 * (a fondo). No hace falta limitar tú la frecuencia de llamada --
 * internamente usa channel_set_frequency() en vez de reconfigurar el
 * canal entero, así que un valor que cambia cada frame no mete
 * chasquidos (ver el comentario junto a channel_set_frequency()).
 */
void sound_engine_set_speed(uint8_t speed_pct)
{
    if (!sound_initialized) {
        sound_init();
    }

    uint16_t freq =
        ENGINE_FREQ_MIN +
        (uint16_t)(
            ((uint32_t)(ENGINE_FREQ_MAX - ENGINE_FREQ_MIN) * speed_pct) / 255
        );

    if (!channel2_engine_active) {
        // Primer arranque: el motor se queda con el canal 2, igual que
        // ya hacía sound_start_menu_music() con la sirena.
        channel2_siren_active = false;
        menu_music_playing    = false;

        channel2_engine_active  = true;
        channel2_engine_last_freq = freq;

        configure_channel(
            &channel2,
            freq,
            ENGINE_VOLUME,
            WAVE_SAW
        );
        return;
    }

    if (freq != channel2_engine_last_freq) {
        channel2_engine_last_freq = freq;
        channel_set_frequency(&channel2, freq);
    }
}

void sound_engine_stop(void)
{
    channel2_engine_active = false;

    disable_channel(
        &channel2
    );
}


/*
 * Derrape -- chirrido agudo (canal 1) mientras se cumplan las
 * condiciones de derrape (típicamente velocidad alta + volante muy
 * girado). Llamar sound_skid_start() en cada frame mientras se derrape
 * y sound_skid_stop() en cuanto se deje de cumplir -- llamar start()
 * repetidamente mientras ya está activo no hace nada (el temblor de
 * frecuencia lo lleva sound_update(), no hace falta reconfigurar el
 * canal en cada llamada).
 */
void sound_skid_start(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    if (channel1_skid_active) {
        return;
    }

    channel1_skid_active = true;
    channel1_skid_rng    = 0x9E3779B9u;

    configure_channel(
        &channel1,
        SKID_FREQ_BASE,
        SKID_VOLUME,
        WAVE_SAW
    );

    channel1_skid_next_change = timeout_ms(SKID_STEP_MS);
}

void sound_skid_stop(void)
{
    if (!channel1_skid_active) {
        return;
    }

    channel1_skid_active = false;

    disable_channel(
        &channel1
    );
}


/*
 * Silencia todo el motor.
 */
void sound_mute(void)
{
    /* generate_sample_u8() devuelve 128 (silencio) mientras
     * sound_enabled sea false. */
    sound_enabled = false;
}


/*
 * Reactiva el motor.
 */
void sound_unmute(void)
{
    sound_enabled = true;
}


/* ============================================================
 * PAC-MAN -- efectos y jingle de inicio
 * ============================================================ */

/*
 * Come-punto alterno: dos notas cortas y agudas (igual que
 * SFX_TICTAC_LO/HI de ArcadePi), alternando en cada punto comido para
 * dar el efecto "waa-waa" clásico sin machacar siempre la misma nota.
 */
void sound_effect_pacman_chomp(bool alt)
{
    if (!sound_initialized) {
        sound_init();
    }

    channel3_play_short(
        alt ? 147 : 110,
        CHANNEL3_VOLUME,
        WAVE_SQUARE,
        22
    );
}


/*
 * Power pellet: arpegio ascendente de 4 notas (DO-MI-SOL-DO), igual
 * que SFX_LEVEL_UP de ArcadePi (que este juego reutilizaba también
 * para la power pellet).
 */
void sound_effect_pacman_power(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    static const uint16_t freqs[4]     = { NOTE_C4, NOTE_E4, NOTE_G4, NOTE_C5 };
    static const uint16_t durations[4] = {      60,      60,      60,     130 };

    channel3_seq_start(freqs, durations, 4, CHANNEL3_VOLUME, WAVE_SQUARE);
}


/*
 * Nivel superado: mismo sonido que sound_effect_pacman_power() (así
 * era también en ArcadePi -- SFX_LEVEL_UP se reutilizaba para ambas
 * cosas). Alias con nombre propio para que quede claro en pacman.c
 * cuál es cuál en cada punto de la llamada.
 */
void sound_effect_pacman_levelup(void)
{
    sound_effect_pacman_power();
}


/*
 * Fruta/bonus recogida: mismo arpegio que la power pellet pero más
 * largo y con la última nota sostenida más tiempo (igual que
 * SFX_SCORE de ArcadePi), para distinguirlo al oído.
 */
void sound_effect_pacman_fruit(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    static const uint16_t freqs[4]     = { NOTE_C4, NOTE_E4, NOTE_G4, NOTE_C5 };
    static const uint16_t durations[4] = {      90,      90,      90,     200 };

    channel3_seq_start(freqs, durations, 4, CHANNEL3_VOLUME, WAVE_SQUARE);
}


/*
 * Fantasma comido en modo frightened: silbido descendente corto
 * (igual que SFX_AST_SMALL de ArcadePi, que este juego reutilizaba).
 */
void sound_effect_pacman_eat_ghost(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    static const uint16_t freqs[4]     = { 320, 190, 110, 65 };
    static const uint16_t durations[4] = {   8,  15,  20, 22 };

    channel3_seq_start(freqs, durations, 4, CHANNEL3_VOLUME, WAVE_SAW);
}


/*
 * Pac-Man muere: 4 notas descendentes largas (igual que
 * SFX_GAME_OVER_LOSE de ArcadePi).
 */
void sound_effect_pacman_death(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    static const uint16_t freqs[4]     = { NOTE_G4, NOTE_F4, NOTE_E4, NOTE_C4 };
    static const uint16_t durations[4] = {     120,     120,     120,     350 };

    channel3_seq_start(freqs, durations, 4, CHANNEL3_VOLUME, WAVE_SQUARE);
}


/*
 * Arranca el jingle de inicio (canal 1 + canal 2). Ver notas junto a
 * los arrays pacman_intro_ch1/ch2 más arriba.
 */
void sound_start_pacman_intro(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    SOUND_ENTER_CRITICAL();

    // Comparte canal 1/2 con la música de menú y canal 2 con la
    // sirena del platillo -- si sonaba algo de eso, el jingle manda.
    menu_music_playing    = false;
    channel2_siren_active = false;
    scramble_win_active   = false;
    toccata_playing       = false;

    pacman_intro_active   = true;
    pacman_intro_ch1_done = false;
    pacman_intro_ch2_done = false;
    pacman_intro_ch1_index = 0;
    pacman_intro_ch2_index = 0;

    configure_channel(&channel1, pacman_intro_ch1_freq[0], CHANNEL1_VOLUME, WAVE_SQUARE);
    configure_channel(&channel2, pacman_intro_ch2_freq[0], CHANNEL2_VOLUME, WAVE_SQUARE);

    pacman_intro_ch1_next = timeout_ms(pacman_intro_ch1_dur[0]);
    pacman_intro_ch2_next = timeout_ms(pacman_intro_ch2_dur[0]);

    SOUND_EXIT_CRITICAL();
}


/*
 * Corta el jingle antes de tiempo (p.ej. si se sale del juego a
 * mitad). Si ya había terminado solo, no hace nada raro: simplemente
 * vuelve a desactivar unos canales que ya estaban inactivos.
 */
void sound_stop_pacman_intro(void)
{
    SOUND_ENTER_CRITICAL();

    pacman_intro_active = false;

    disable_channel(&channel1);
    disable_channel(&channel2);

    SOUND_EXIT_CRITICAL();
}


/*
 * Arranca la fanfarria de victoria de Scramble (canal 1 + canal 2),
 * una sola pasada de ~3200ms. Ver notas junto a los arrays
 * scramble_win_ch1/ch2 más arriba.
 *
 * Deja libre el canal 3 a propósito: los efectos de explosión que
 * el juego dispare mientras suena se siguen oyendo por encima.
 */
void sound_start_scramble_victory(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    SOUND_ENTER_CRITICAL();

    // Canales 1/2: esto manda sobre cualquier otra cosa que los
    // estuviera usando, igual que hacen el jingle de Pac-Man y la
    // música de Tetris.
    menu_music_playing     = false;
    tetris_music_playing   = false;
    pacman_intro_active    = false;
    channel2_siren_active  = false;
    channel2_engine_active = false;
    channel1_skid_active   = false;
    toccata_playing        = false;

    scramble_win_active   = true;
    scramble_win_ch1_done = false;
    scramble_win_ch2_done = false;
    scramble_win_ch1_index = 0;
    scramble_win_ch2_index = 0;

    configure_channel(&channel1, scramble_win_ch1_freq[0], SCRAMBLE_WIN_CH1_VOLUME, WAVE_TRIANGLE);
    configure_channel(&channel2, scramble_win_ch2_freq[0], SCRAMBLE_WIN_CH2_VOLUME, WAVE_SQUARE);

    scramble_win_ch1_next = timeout_ms(scramble_win_ch1_dur[0]);
    scramble_win_ch2_next = timeout_ms(scramble_win_ch2_dur[0]);

    SOUND_EXIT_CRITICAL();
}


/*
 * Corta la fanfarria antes de tiempo (p.ej. al salir del juego a
 * mitad). Si ya había terminado sola, no hace nada raro.
 */
void sound_stop_scramble_victory(void)
{
    SOUND_ENTER_CRITICAL();

    scramble_win_active = false;

    disable_channel(&channel1);
    disable_channel(&channel2);

    SOUND_EXIT_CRITICAL();
}


bool sound_scramble_victory_is_playing(void)
{
    return scramble_win_active;
}


/*
 * Arranca la Toccata y fuga en re menor BWV 565 de Bach como jingle
 * de inicio de Paratrooper (canal 1 + canal 2): UNA SOLA PASADA de
 * ~8.6s, no en bucle -- igual que sound_start_pacman_intro(). Termina
 * sola y a partir de ahí solo se oyen los efectos del canal 3; llamar
 * a sound_stop_paratrooper_music() la corta antes de tiempo si hace
 * falta (p.ej. si se sale del juego a mitad). Ver notas junto a los
 * arrays toccata_ch1/ch2 más arriba.
 *
 * Canal 3 libre, como siempre: los disparos y explosiones del juego
 * se siguen oyendo por encima mientras suena.
 */
void sound_start_paratrooper_music(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    SOUND_ENTER_CRITICAL();

    // Canales 1/2: esto manda sobre cualquier otra cosa que los
    // estuviera usando.
    menu_music_playing     = false;
    tetris_music_playing   = false;
    pacman_intro_active    = false;
    scramble_win_active    = false;
    channel2_siren_active  = false;
    channel2_engine_active = false;
    channel1_skid_active   = false;

    toccata_playing = true;
    toccata_index   = 0;

    configure_channel(&channel1, toccata_ch1_freq[0], TOCCATA_CH1_VOLUME, WAVE_TRIANGLE);
    configure_channel(&channel2, toccata_ch2_freq[0], TOCCATA_CH2_VOLUME, WAVE_SQUARE);

    toccata_next = timeout_ms(toccata_dur[0]);

    SOUND_EXIT_CRITICAL();
}


/*
 * Para la música de Paratrooper (p.ej. al terminar la partida o al
 * salir del juego).
 */
void sound_stop_paratrooper_music(void)
{
    SOUND_ENTER_CRITICAL();

    toccata_playing = false;

    disable_channel(&channel1);
    disable_channel(&channel2);

    SOUND_EXIT_CRITICAL();
}


/* Refleja si la pasada sigue sonando -- se pone sola a false en
 * cuanto termina (ver el bloque de avance en sound_update()). */
bool sound_paratrooper_music_is_playing(void)
{
    return toccata_playing;
}


/* ============================================================
 * SELECCIÓN DE PISTA DEL MENÚ
 * ============================================================ */

/*
 * Elige la pista del menú (0 = Greensleeves, 1 = Neon Circuit,
 * 2 = Star Patrol). Si la música ya está sonando, el cambio se nota
 * de inmediato: se reinicia el índice y se reconfiguran los canales
 * con la nota 0 de la pista nueva. Un valor fuera de rango se
 * ignora, así que no hace falta validarlo en el sitio de la llamada.
 */
void sound_set_menu_track(uint8_t track)
{
    if (track >= MENU_TRACK_COUNT) {
        return;
    }

    SOUND_ENTER_CRITICAL();

    menu_track  = track;
    music_index = 0;

    if (menu_music_playing) {
        configure_channel(
            &channel1,
            menu_tracks[menu_track].melody[0],
            MENU_MUSIC_CH1_VOLUME,
            WAVE_TRIANGLE
        );
        configure_channel(
            &channel2,
            menu_tracks[menu_track].bass[0],
            MENU_MUSIC_CH2_VOLUME,
            WAVE_TRIANGLE
        );
        music_next_change =
            timeout_ms(menu_tracks[menu_track].durations[0]);
    }

    SOUND_EXIT_CRITICAL();
}


/* Pasa a la siguiente pista, volviendo a la 0 tras la última. */
void sound_next_menu_track(void)
{
    sound_set_menu_track((uint8_t)((menu_track + 1) % MENU_TRACK_COUNT));
}


uint8_t sound_get_menu_track(void)
{
    return menu_track;
}


/*
 * Duración en ms de UNA vuelta completa a la pista de menú "track"
 * (la suma de todas sus duraciones -- Greensleeves ~29.9s, Neon
 * Circuit ~12.8s, Star Patrol ~9.6s). Pensada para que quien reproduce
 * la música sepa cuándo ha terminado un ciclo y toca pararla, sin
 * tener que llevar esa cuenta por su cuenta -- p.ej. para encadenar
 * varias pistas con un silencio entre medias, como hace el menú
 * principal (menu_run() en menu.c).
 *
 * Un valor de "track" fuera de rango devuelve 0.
 */
uint32_t sound_menu_track_duration_ms(uint8_t track)
{
    if (track >= MENU_TRACK_COUNT) {
        return 0;
    }

    uint32_t total = 0;
    for (uint16_t i = 0; i < menu_tracks[track].count; i++) {
        total += menu_tracks[track].durations[i];
    }
    return total;
}


/*
 * Arranca la música in-game de Tetris (canal 1 + canal 2), en bucle
 * hasta llamar a sound_stop_tetris_music(). Ver notas junto a los
 * arrays tetris_music_ch1/ch2 más arriba.
 */
void sound_start_tetris_music(void)
{
    if (!sound_initialized) {
        sound_init();
    }

    SOUND_ENTER_CRITICAL();

    // Canales 1/2 son "lo único que suena de fondo a la vez" --
    // igual que hace sound_start_pacman_intro(), esto manda sobre
    // cualquier otra cosa que estuviera usándolos.
    menu_music_playing     = false;
    channel2_siren_active  = false;
    channel2_engine_active = false;
    channel1_skid_active   = false;
    pacman_intro_active    = false;
    scramble_win_active    = false;
    toccata_playing        = false;

    tetris_music_playing  = true;
    tetris_music_ch1_index = 0;
    tetris_music_ch2_index = 0;

    configure_channel(&channel1, tetris_music_ch1_freq[0], TETRIS_MUSIC_CH1_VOLUME, WAVE_TRIANGLE);
    configure_channel(&channel2, tetris_music_ch2_freq[0], TETRIS_MUSIC_CH2_VOLUME, WAVE_TRIANGLE);

    tetris_music_ch1_next = timeout_ms(tetris_music_ch1_dur[0]);
    tetris_music_ch2_next = timeout_ms(tetris_music_ch2_dur[0]);

    SOUND_EXIT_CRITICAL();
}


/*
 * Para la música in-game de Tetris (p.ej. al terminar la partida).
 */
void sound_stop_tetris_music(void)
{
    SOUND_ENTER_CRITICAL();

    tetris_music_playing = false;

    disable_channel(&channel1);
    disable_channel(&channel2);

    SOUND_EXIT_CRITICAL();
}


bool sound_tetris_music_is_playing(void)
{
    return tetris_music_playing;
}
