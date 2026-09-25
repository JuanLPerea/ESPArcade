#include "sound.h"

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "driver/dac_continuous.h"

/* ============================================================
 * PORT A ESP32 - lo que cambia respecto al original (que usaba
 * PWM+GP4+repeating_timer de la Pico):
 *
 *  - Salida: PWM+GP4 -> DAC+DMA en GPIO25, via una tarea de
 *    FreeRTOS en el core 1 que genera bloques de muestras (en vez
 *    de una interrupcion de timer que genera una muestra cada
 *    vez -- el resultado sonoro es el mismo, cambia el mecanismo).
 *  - Seccion critica: save_and_disable_interrupts/restore_interrupts
 *    (Pico) -> portENTER_CRITICAL/portEXIT_CRITICAL con un spinlock.
 *  - Temporizacion: absolute_time_t/make_timeout_time_ms/time_reached
 *    (Pico) -> milisegundos con esp_timer_get_time().
 *
 * TODO LO DEMAS (DDS, formas de onda, mezcla, las 4 melodias
 * completas, el secuenciador de canal 3, todos los efectos) es
 * la MISMA logica que el sound.c original: matematica y datos
 * puros, no tocan hardware.
 * ============================================================ */

#define SOUND_DAC_GPIO 25  // DAC1

#define AUDIO_SAMPLE_RATE 22050

/*
 * Volumen máximo de cada canal (ver notas del original sobre por
 * qué estos valores concretos: wave_triangle() corregida tiene
 * ~42% menos RMS que una cuadrada del mismo pico).
 */
#define CHANNEL1_VOLUME 110
#define CHANNEL2_VOLUME 80
#define CHANNEL3_VOLUME 100

#define MENU_MUSIC_CH1_VOLUME   130
#define MENU_MUSIC_CH2_VOLUME   100
#define TETRIS_MUSIC_CH1_VOLUME  90
#define TETRIS_MUSIC_CH2_VOLUME  55

#define WAVE_SQUARE   0
#define WAVE_TRIANGLE 1
#define WAVE_SAW      2
#define WAVE_NOISE    3

typedef struct {
    volatile bool active;
    uint32_t phase;
    uint32_t phase_increment;
    uint16_t frequency;
    uint16_t volume;
    uint8_t waveform;
    uint32_t noise_state;
} audio_channel_t;

static dac_continuous_handle_t s_dac;
static volatile bool sound_initialized = false;
static volatile bool sound_enabled = true;

static volatile audio_channel_t channel1;
static volatile audio_channel_t channel2;
static volatile audio_channel_t channel3;

// Spinlock ESP-IDF: equivalente a save_and_disable_interrupts/
// restore_interrupts del Pico SDK.
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
#define SOUND_ENTER_CRITICAL() portENTER_CRITICAL(&s_lock)
#define SOUND_EXIT_CRITICAL()  portEXIT_CRITICAL(&s_lock)

static inline uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ============================================================
 * MÚSICA DEL MENÚ -- GREENSLEEVES
 * Mi menor, 6/4, 16 compases. Identico al original.
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

static const uint16_t melody_notes[MUSIC_NOTE_COUNT] = {
    NOTE_E4, NOTE_G4, NOTE_A4, NOTE_B4, NOTE_C5, NOTE_B4,
    NOTE_A4, NOTE_F4, NOTE_D4, NOTE_E4, NOTE_F4,
    NOTE_G4, NOTE_F4, NOTE_E4, NOTE_DS4, NOTE_E4,
    NOTE_F4, NOTE_DS4, NOTE_B3, NOTE_E4,
    NOTE_E4, NOTE_G4, NOTE_A4, NOTE_B4, NOTE_C5, NOTE_B4,
    NOTE_A4, NOTE_F4, NOTE_D4, NOTE_E4, NOTE_F4,
    NOTE_G4, NOTE_F4, NOTE_E4, NOTE_DS4, NOTE_E4,
    NOTE_F4, NOTE_E4, NOTE_D4, 0,
    NOTE_D5, NOTE_D5, NOTE_C5, NOTE_B4,
    NOTE_A4, NOTE_F4, NOTE_D4, NOTE_E4, NOTE_F4,
    NOTE_G4, NOTE_E4, NOTE_E4, NOTE_DS4, NOTE_E4,
    NOTE_F4, NOTE_DS4, NOTE_B3, 0,
    NOTE_D5, NOTE_D5, NOTE_C5, NOTE_B4,
    NOTE_A4, NOTE_F4, NOTE_D4, NOTE_E4, NOTE_F4,
    NOTE_G4, NOTE_F4, NOTE_E4, NOTE_DS4, NOTE_E4,
    NOTE_E4, NOTE_E4
};

static const uint16_t melody_duration[MUSIC_NOTE_COUNT] = {
    600, 300, 300, 450, 150, 300,
    600, 300, 450, 150, 300,
    600, 300, 450, 150, 300,
    600, 300, 600, 300,
    600, 300, 300, 450, 150, 300,
    600, 300, 450, 150, 300,
    450, 150, 300, 450, 150, 300,
    900, 600, 300,
    900, 450, 150, 300,
    600, 300, 450, 150, 300,
    600, 300, 450, 150, 300,
    600, 300, 600, 300,
    900, 450, 150, 300,
    600, 300, 450, 150, 300,
    450, 150, 300, 450, 150, 300,
    900, 1400
};

static const uint16_t bass_notes[MUSIC_NOTE_COUNT] = {
    NOTE_E3, 0, NOTE_B3, 0, NOTE_E3, 0,
    NOTE_A3, 0, NOTE_D3, 0, NOTE_A3,
    NOTE_E3, 0, NOTE_B3, 0, NOTE_E3,
    NOTE_B3, 0, NOTE_B3, NOTE_E3,
    NOTE_E3, 0, NOTE_B3, 0, NOTE_E3, 0,
    NOTE_A3, 0, NOTE_D3, 0, NOTE_A3,
    NOTE_E3, 0, NOTE_B3, 0, NOTE_B3, 0,
    NOTE_E3, 0, 0,
    NOTE_G3, 0, NOTE_D4, NOTE_G3,
    NOTE_D3, 0, NOTE_A3, 0, NOTE_D3,
    NOTE_E3, 0, NOTE_B3, 0, NOTE_E3,
    NOTE_B3, 0, NOTE_B3, 0,
    NOTE_G3, 0, NOTE_D4, NOTE_G3,
    NOTE_D3, 0, NOTE_A3, 0, NOTE_D3,
    NOTE_B3, 0, NOTE_B3, 0, NOTE_E3, 0,
    NOTE_E3, 0
};

static volatile bool menu_music_playing = false;
static volatile uint32_t music_index = 0;
static uint32_t music_next_change_ms = 0;

/* ============================================================
 * MÚSICA IN-GAME DE TETRIS -- KOROBEINIKI
 * Canal 1 (melodia, triangular) 98 eventos, canal 2 (bajo,
 * cuadrada) 128 eventos, cada uno con su propio indice/tiempo
 * pero ambos suman 37800ms exactos -- coinciden al reiniciar.
 * ============================================================ */
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
static uint32_t tetris_music_ch1_next_ms;
static uint32_t tetris_music_ch2_next_ms;

/* ============================================================
 * JINGLE DE INICIO DE PAC-MAN (~4300ms, una vez)
 * ============================================================ */
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
static uint32_t pacman_intro_ch1_next_ms;
static uint32_t pacman_intro_ch2_next_ms;

/* ============================================================
 * SIRENA / MOTOR / DERRAPE (canales compartidos, "quien llega
 * ultimo se queda el canal" -- ver notas del original)
 * ============================================================ */
static volatile bool channel2_siren_active = false;
static uint8_t channel2_siren_phase = 0;
static uint32_t channel2_siren_next_ms;
#define SIREN_FREQ_LOW   600
#define SIREN_FREQ_HIGH  900
#define SIREN_STEP_MS     90
#define SIREN_VOLUME      40

static volatile bool channel2_engine_active = false;
static volatile uint16_t channel2_engine_last_freq = 0;
#define ENGINE_FREQ_MIN   55
#define ENGINE_FREQ_MAX  260
#define ENGINE_VOLUME     50

static volatile bool channel1_skid_active = false;
static uint32_t channel1_skid_rng = 1;
static uint32_t channel1_skid_next_ms;
#define SKID_FREQ_BASE     2000
#define SKID_FREQ_JITTER    700
#define SKID_STEP_MS         18
#define SKID_VOLUME           60

static volatile bool channel3_tone_has_expiry = false;
static uint32_t channel3_tone_expiry_ms;

/* ============================================================
 * DECLARACIONES ADELANTADAS
 * ============================================================ */
static void configure_channel(volatile audio_channel_t *channel, uint16_t frequency, uint16_t volume, uint8_t waveform);
static void disable_channel(volatile audio_channel_t *channel);
static void channel_set_frequency(volatile audio_channel_t *channel, uint16_t frequency);

/* ============================================================
 * SECUENCIADOR DE CANAL 3 -- melodias cortas no bloqueantes
 * (2-4 notas) y apagado automatico de efectos de 1 nota.
 * ============================================================ */
#define CHANNEL3_SEQ_MAX_NOTES 4

typedef struct {
    uint16_t freqs[CHANNEL3_SEQ_MAX_NOTES];
    uint16_t durations_ms[CHANNEL3_SEQ_MAX_NOTES];
    uint8_t  count;
    uint8_t  index;
    bool     active;
    uint16_t volume;
    uint8_t  waveform;
    uint32_t next_change_ms;
} channel3_sequence_t;

static volatile channel3_sequence_t channel3_seq = { .active = false };

static void channel3_seq_start(const uint16_t *freqs, const uint16_t *durations_ms,
                                uint8_t count, uint16_t volume, uint8_t waveform) {
    if (count > CHANNEL3_SEQ_MAX_NOTES) count = CHANNEL3_SEQ_MAX_NOTES;

    for (uint8_t i = 0; i < count; i++) {
        channel3_seq.freqs[i] = freqs[i];
        channel3_seq.durations_ms[i] = durations_ms[i];
    }

    channel3_seq.count = count;
    channel3_seq.index = 0;
    channel3_seq.volume = volume;
    channel3_seq.waveform = waveform;
    channel3_seq.active = true;

    channel3_tone_has_expiry = false; // se excluyen mutuamente

    configure_channel(&channel3, channel3_seq.freqs[0], volume, waveform);
    channel3_seq.next_change_ms = now_ms() + channel3_seq.durations_ms[0];
}

static void channel3_seq_update(void) {
    if (!channel3_seq.active) return;
    if (now_ms() < channel3_seq.next_change_ms) return;

    channel3_seq.index++;
    if (channel3_seq.index >= channel3_seq.count) {
        channel3_seq.active = false;
        disable_channel(&channel3);
        return;
    }

    configure_channel(&channel3, channel3_seq.freqs[channel3_seq.index],
                       channel3_seq.volume, channel3_seq.waveform);
    channel3_seq.next_change_ms = now_ms() + channel3_seq.durations_ms[channel3_seq.index];
}

static void channel3_play_short(uint16_t freq, uint16_t volume, uint8_t waveform, uint16_t duration_ms) {
    uint16_t freqs[1] = { freq };
    uint16_t durations[1] = { duration_ms };
    channel3_seq_start(freqs, durations, 1, volume, waveform);
}

/* ============================================================
 * DDS / generadores de onda
 * ============================================================ */
static uint32_t frequency_to_phase_increment(uint16_t frequency) {
    if (frequency == 0) return 0;
    return (uint32_t)(((uint64_t)frequency << 32) / AUDIO_SAMPLE_RATE);
}

static void configure_channel(volatile audio_channel_t *channel, uint16_t frequency, uint16_t volume, uint8_t waveform) {
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
    channel->phase_increment = frequency_to_phase_increment(frequency);
    channel->volume = volume;
    channel->waveform = waveform;
    channel->noise_state = 0x12345678u ^ ((uint32_t)frequency * 2654435761u);
    if (channel->noise_state == 0) channel->noise_state = 1;
    channel->active = true;

    SOUND_EXIT_CRITICAL();
}

static void disable_channel(volatile audio_channel_t *channel) {
    channel->active = false;
}

// Cambia SOLO la frecuencia de un canal ya activo, sin resetear fase
// (para barridos continuos: motor, derrape). Ver notas del original.
static void channel_set_frequency(volatile audio_channel_t *channel, uint16_t frequency) {
    SOUND_ENTER_CRITICAL();
    if (channel->active) {
        channel->frequency = frequency;
        channel->phase_increment = frequency_to_phase_increment(frequency);
    }
    SOUND_EXIT_CRITICAL();
}

static int16_t wave_square(uint32_t phase) {
    return (phase & 0x80000000u) ? 127 : -127;
}

// Rampa lineal correcta sobre todo el semiperiodo: 254/32768.
static int16_t wave_triangle(uint32_t phase) {
    uint16_t p = (uint16_t)(phase >> 16);
    int32_t value;
    if (p < 32768) {
        value = -127 + (((int32_t)p * 254) / 32768);
    } else {
        value = 127 - ((((int32_t)p - 32768) * 254) / 32768);
    }
    if (value > 127) value = 127;
    if (value < -127) value = -127;
    return (int16_t)value;
}

static int16_t wave_saw(uint32_t phase) {
    return (int16_t)(phase >> 24) - 128;
}

static uint8_t noise_next(volatile audio_channel_t *channel) {
    uint32_t x = channel->noise_state;
    uint32_t bit = ((x >> 0) ^ (x >> 2) ^ (x >> 3) ^ (x >> 5)) & 1;
    x = (x >> 1) | (bit << 31);
    channel->noise_state = x;
    return (uint8_t)(x >> 24);
}

static int16_t channel_sample(volatile audio_channel_t *channel) {
    if (!channel->active) return 0;
    int16_t sample = 0;
    switch (channel->waveform) {
        case WAVE_SQUARE:   sample = wave_square(channel->phase); break;
        case WAVE_TRIANGLE: sample = wave_triangle(channel->phase); break;
        case WAVE_SAW:      sample = wave_saw(channel->phase); break;
        case WAVE_NOISE:    sample = (int16_t)noise_next(channel) - 128; break;
        default: sample = 0; break;
    }
    channel->phase += channel->phase_increment;
    return (sample * (int16_t)channel->volume) / 100;
}

static uint8_t generate_sample_u8(void) {
    if (!sound_enabled) return 128;

    int32_t s1 = channel_sample(&channel1);
    int32_t s2 = channel_sample(&channel2);
    int32_t s3 = channel_sample(&channel3);

    int32_t mixed = s1 + s2 + s3;
    if (mixed > 255) mixed = 255;
    if (mixed < -255) mixed = -255;

    int32_t v = 128 + mixed / 2;
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return (uint8_t)v;
}

/* ============================================================
 * Tarea de audio: sustituye a la interrupcion de timer del Pico.
 * ============================================================ */
#define AUDIO_BLOCK_SAMPLES 256

static void audio_task(void *arg) {
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
 * INICIALIZACIÓN
 * ============================================================ */
void sound_init(void) {
    if (sound_initialized) return;

    dac_continuous_config_t dac_cfg = {
        .chan_mask = DAC_CHANNEL_MASK_CH0, // GPIO25
        .desc_num = 4,
        .buf_size = AUDIO_BLOCK_SAMPLES,
        .freq_hz = AUDIO_SAMPLE_RATE,
        .offset = 0,
        .clk_src = DAC_DIGI_CLK_SRC_DEFAULT,
        .chan_mode = DAC_CHANNEL_MODE_SIMUL,
    };
    dac_continuous_new_channels(&dac_cfg, &s_dac);
    dac_continuous_enable(s_dac);

    channel1.active = false;
    channel2.active = false;
    channel3.active = false;

    xTaskCreatePinnedToCore(audio_task, "sound_task", 2048, NULL, configMAX_PRIORITIES - 2, NULL, 1);

    sound_initialized = true;
}

/* ============================================================
 * TONO GENÉRICO
 * ============================================================ */
void sound_play_tone(uint16_t frequency_hz, uint16_t duration_ms) {
    if (!sound_initialized) sound_init();

    channel3_seq.active = false; // comparten canal 3, se excluyen mutuamente

    configure_channel(&channel3, frequency_hz, CHANNEL3_VOLUME, WAVE_SQUARE);

    if (duration_ms > 0) {
        channel3_tone_has_expiry = true;
        channel3_tone_expiry_ms = now_ms() + duration_ms;
    } else {
        channel3_tone_has_expiry = false;
    }
}

/* ============================================================
 * MÚSICA DEL MENÚ
 * ============================================================ */
void sound_start_menu_music(void) {
    if (!sound_initialized) sound_init();

    SOUND_ENTER_CRITICAL();
    music_index = 0;
    menu_music_playing = true;
    channel2_siren_active = false;
    channel2_engine_active = false;
    SOUND_EXIT_CRITICAL();

    configure_channel(&channel1, melody_notes[0], MENU_MUSIC_CH1_VOLUME, WAVE_TRIANGLE);
    configure_channel(&channel2, bass_notes[0], MENU_MUSIC_CH2_VOLUME, WAVE_TRIANGLE);
    music_next_change_ms = now_ms() + melody_duration[0];
}

void sound_stop_menu_music(void) {
    SOUND_ENTER_CRITICAL();
    menu_music_playing = false;
    SOUND_EXIT_CRITICAL();
    disable_channel(&channel1);
    disable_channel(&channel2);
}

void sound_update(void) {
    uint32_t t = now_ms();

    // Auto-apagado del tono generico de sound_play_tone()
    if (channel3_tone_has_expiry && t >= channel3_tone_expiry_ms) {
        channel3_tone_has_expiry = false;
        disable_channel(&channel3);
    }

    channel3_seq_update();

    // Jingle de inicio de Pac-Man
    if (pacman_intro_active) {
        if (!pacman_intro_ch1_done && t >= pacman_intro_ch1_next_ms) {
            pacman_intro_ch1_index++;
            if (pacman_intro_ch1_index >= PACMAN_INTRO_CH1_LEN) {
                pacman_intro_ch1_done = true;
                disable_channel(&channel1);
            } else {
                configure_channel(&channel1, pacman_intro_ch1_freq[pacman_intro_ch1_index], CHANNEL1_VOLUME, WAVE_SQUARE);
                pacman_intro_ch1_next_ms = t + pacman_intro_ch1_dur[pacman_intro_ch1_index];
            }
        }
        if (!pacman_intro_ch2_done && t >= pacman_intro_ch2_next_ms) {
            pacman_intro_ch2_index++;
            if (pacman_intro_ch2_index >= PACMAN_INTRO_CH2_LEN) {
                pacman_intro_ch2_done = true;
                disable_channel(&channel2);
            } else {
                configure_channel(&channel2, pacman_intro_ch2_freq[pacman_intro_ch2_index], CHANNEL2_VOLUME, WAVE_SQUARE);
                pacman_intro_ch2_next_ms = t + pacman_intro_ch2_dur[pacman_intro_ch2_index];
            }
        }
        if (pacman_intro_ch1_done && pacman_intro_ch2_done) {
            pacman_intro_active = false;
        }
    }

    // Musica in-game de Tetris (bucle: al llegar al final, indice a 0)
    if (tetris_music_playing) {
        if (t >= tetris_music_ch1_next_ms) {
            tetris_music_ch1_index++;
            if (tetris_music_ch1_index >= TETRIS_MUSIC_CH1_LEN) tetris_music_ch1_index = 0;
            configure_channel(&channel1, tetris_music_ch1_freq[tetris_music_ch1_index], TETRIS_MUSIC_CH1_VOLUME, WAVE_TRIANGLE);
            tetris_music_ch1_next_ms = t + tetris_music_ch1_dur[tetris_music_ch1_index];
        }
        if (t >= tetris_music_ch2_next_ms) {
            tetris_music_ch2_index++;
            if (tetris_music_ch2_index >= TETRIS_MUSIC_CH2_LEN) tetris_music_ch2_index = 0;
            configure_channel(&channel2, tetris_music_ch2_freq[tetris_music_ch2_index], TETRIS_MUSIC_CH2_VOLUME, WAVE_SQUARE);
            tetris_music_ch2_next_ms = t + tetris_music_ch2_dur[tetris_music_ch2_index];
        }
    }

    // Sirena del platillo
    if (channel2_siren_active && t >= channel2_siren_next_ms) {
        channel2_siren_phase ^= 1;
        configure_channel(&channel2, channel2_siren_phase ? SIREN_FREQ_HIGH : SIREN_FREQ_LOW, SIREN_VOLUME, WAVE_TRIANGLE);
        channel2_siren_next_ms = t + SIREN_STEP_MS;
    }

    // Temblor de frecuencia del derrape (xorshift32)
    if (channel1_skid_active && t >= channel1_skid_next_ms) {
        channel1_skid_rng ^= channel1_skid_rng << 13;
        channel1_skid_rng ^= channel1_skid_rng >> 17;
        channel1_skid_rng ^= channel1_skid_rng << 5;

        uint16_t jitter = (uint16_t)(channel1_skid_rng % (SKID_FREQ_JITTER * 2));
        uint16_t freq = (SKID_FREQ_BASE - SKID_FREQ_JITTER) + jitter;

        channel_set_frequency(&channel1, freq);
        channel1_skid_next_ms = t + SKID_STEP_MS;
    }

    if (!menu_music_playing) return;
    if (t < music_next_change_ms) return;

    music_index++;
    if (music_index >= MUSIC_NOTE_COUNT) music_index = 0;

    uint16_t melody = melody_notes[music_index];
    uint16_t bass = bass_notes[music_index];

    configure_channel(&channel1, melody, MENU_MUSIC_CH1_VOLUME, WAVE_TRIANGLE);
    configure_channel(&channel2, bass, MENU_MUSIC_CH2_VOLUME, WAVE_TRIANGLE);

    music_next_change_ms = t + melody_duration[music_index];
}

bool sound_menu_music_is_playing(void) {
    return menu_music_playing;
}

/* ============================================================
 * MÚSICA IN-GAME DE TETRIS
 * ============================================================ */
void sound_start_tetris_music(void) {
    if (!sound_initialized) sound_init();

    SOUND_ENTER_CRITICAL();
    menu_music_playing = false;
    channel2_siren_active = false;
    channel2_engine_active = false;
    channel1_skid_active = false;
    pacman_intro_active = false;

    tetris_music_playing = true;
    tetris_music_ch1_index = 0;
    tetris_music_ch2_index = 0;
    SOUND_EXIT_CRITICAL();

    configure_channel(&channel1, tetris_music_ch1_freq[0], TETRIS_MUSIC_CH1_VOLUME, WAVE_TRIANGLE);
    configure_channel(&channel2, tetris_music_ch2_freq[0], TETRIS_MUSIC_CH2_VOLUME, WAVE_SQUARE);

    uint32_t t = now_ms();
    tetris_music_ch1_next_ms = t + tetris_music_ch1_dur[0];
    tetris_music_ch2_next_ms = t + tetris_music_ch2_dur[0];
}

void sound_stop_tetris_music(void) {
    SOUND_ENTER_CRITICAL();
    tetris_music_playing = false;
    SOUND_EXIT_CRITICAL();
    disable_channel(&channel1);
    disable_channel(&channel2);
}

bool sound_tetris_music_is_playing(void) {
    return tetris_music_playing;
}

/* ============================================================
 * EFECTOS
 * ============================================================ */
#define CHANNEL3_SFX_MS 55

void sound_effect_shoot(void) {
    if (!sound_initialized) sound_init();
    channel3_play_short(1100, CHANNEL3_VOLUME, WAVE_SQUARE, CHANNEL3_SFX_MS);
}

void sound_effect_explosion(void) {
    if (!sound_initialized) sound_init();
    channel3_play_short(100, CHANNEL3_VOLUME, WAVE_NOISE, 150);
}

void sound_effect_select(void) {
    if (!sound_initialized) sound_init();
    channel3_play_short(880, CHANNEL3_VOLUME, WAVE_SQUARE, CHANNEL3_SFX_MS);
}

void sound_effect_move(void) {
    if (!sound_initialized) sound_init();
    channel3_play_short(660, CHANNEL3_VOLUME, WAVE_SQUARE, CHANNEL3_SFX_MS);
}

void sound_effect_game_over(void) {
    if (!sound_initialized) sound_init();
    channel3_play_short(180, CHANNEL3_VOLUME, WAVE_SAW, 400);
}

void sound_effect_success(void) {
    if (!sound_initialized) sound_init();
    channel3_play_short(1047, CHANNEL3_VOLUME, WAVE_TRIANGLE, 150);
}

void sound_effect_lose_point(void) {
    if (!sound_initialized) sound_init();
    static const uint16_t freqs[4]     = { 392, 330, 262, 196 };
    static const uint16_t durations[4] = {  70,  70,  70, 140 };
    channel3_seq_start(freqs, durations, 4, CHANNEL3_VOLUME, WAVE_SQUARE);
}

void sound_effect_victory(void) {
    if (!sound_initialized) sound_init();
    static const uint16_t freqs[3]     = { 523, 659, 784 };
    static const uint16_t durations[3] = { 110, 110, 260 };
    channel3_seq_start(freqs, durations, 3, CHANNEL3_VOLUME, WAVE_SQUARE);
}

void sound_effect_stop(void) {
    channel3_seq.active = false;
    channel3_tone_has_expiry = false;
    disable_channel(&channel3);
}

/* ============================================================
 * SIRENA / MOTOR / DERRAPE
 * ============================================================ */
void sound_siren_start(void) {
    if (!sound_initialized) sound_init();

    channel2_engine_active = false;
    channel2_siren_active = true;
    channel2_siren_phase = 0;

    configure_channel(&channel2, SIREN_FREQ_LOW, SIREN_VOLUME, WAVE_TRIANGLE);
    channel2_siren_next_ms = now_ms() + SIREN_STEP_MS;
}

void sound_siren_stop(void) {
    channel2_siren_active = false;
    disable_channel(&channel2);
}

void sound_engine_set_speed(uint8_t speed_pct) {
    if (!sound_initialized) sound_init();

    uint16_t freq = ENGINE_FREQ_MIN +
        (uint16_t)(((uint32_t)(ENGINE_FREQ_MAX - ENGINE_FREQ_MIN) * speed_pct) / 255);

    if (!channel2_engine_active) {
        channel2_siren_active = false;
        SOUND_ENTER_CRITICAL();
        menu_music_playing = false;
        SOUND_EXIT_CRITICAL();

        channel2_engine_active = true;
        channel2_engine_last_freq = freq;

        configure_channel(&channel2, freq, ENGINE_VOLUME, WAVE_SAW);
        return;
    }

    if (freq != channel2_engine_last_freq) {
        channel2_engine_last_freq = freq;
        channel_set_frequency(&channel2, freq);
    }
}

void sound_engine_stop(void) {
    channel2_engine_active = false;
    disable_channel(&channel2);
}

void sound_skid_start(void) {
    if (!sound_initialized) sound_init();
    if (channel1_skid_active) return;

    channel1_skid_active = true;
    channel1_skid_rng = 0x9E3779B9u;

    configure_channel(&channel1, SKID_FREQ_BASE, SKID_VOLUME, WAVE_SAW);
    channel1_skid_next_ms = now_ms() + SKID_STEP_MS;
}

void sound_skid_stop(void) {
    if (!channel1_skid_active) return;
    channel1_skid_active = false;
    disable_channel(&channel1);
}

/* ============================================================
 * CONTROL GENERAL
 * ============================================================ */
void sound_mute(void) {
    sound_enabled = false; // generate_sample_u8() ya devuelve 128 (silencio) con esto
}

void sound_unmute(void) {
    sound_enabled = true;
}

/* ============================================================
 * PAC-MAN -- efectos y jingle de inicio
 * ============================================================ */
void sound_effect_pacman_chomp(bool alt) {
    if (!sound_initialized) sound_init();
    channel3_play_short(alt ? 147 : 110, CHANNEL3_VOLUME, WAVE_SQUARE, 22);
}

void sound_effect_pacman_power(void) {
    if (!sound_initialized) sound_init();
    static const uint16_t freqs[4]     = { NOTE_C4, NOTE_E4, NOTE_G4, NOTE_C5 };
    static const uint16_t durations[4] = {      60,      60,      60,     130 };
    channel3_seq_start(freqs, durations, 4, CHANNEL3_VOLUME, WAVE_SQUARE);
}

void sound_effect_pacman_levelup(void) {
    sound_effect_pacman_power();
}

void sound_effect_pacman_fruit(void) {
    if (!sound_initialized) sound_init();
    static const uint16_t freqs[4]     = { NOTE_C4, NOTE_E4, NOTE_G4, NOTE_C5 };
    static const uint16_t durations[4] = {      90,      90,      90,     200 };
    channel3_seq_start(freqs, durations, 4, CHANNEL3_VOLUME, WAVE_SQUARE);
}

void sound_effect_pacman_eat_ghost(void) {
    if (!sound_initialized) sound_init();
    static const uint16_t freqs[4]     = { 320, 190, 110, 65 };
    static const uint16_t durations[4] = {   8,  15,  20, 22 };
    channel3_seq_start(freqs, durations, 4, CHANNEL3_VOLUME, WAVE_SAW);
}

void sound_effect_pacman_death(void) {
    if (!sound_initialized) sound_init();
    static const uint16_t freqs[4]     = { NOTE_G4, NOTE_F4, NOTE_E4, NOTE_C4 };
    static const uint16_t durations[4] = {     120,     120,     120,     350 };
    channel3_seq_start(freqs, durations, 4, CHANNEL3_VOLUME, WAVE_SQUARE);
}

void sound_start_pacman_intro(void) {
    if (!sound_initialized) sound_init();

    SOUND_ENTER_CRITICAL();
    menu_music_playing = false;
    channel2_siren_active = false;

    pacman_intro_active = true;
    pacman_intro_ch1_done = false;
    pacman_intro_ch2_done = false;
    pacman_intro_ch1_index = 0;
    pacman_intro_ch2_index = 0;
    SOUND_EXIT_CRITICAL();

    configure_channel(&channel1, pacman_intro_ch1_freq[0], CHANNEL1_VOLUME, WAVE_SQUARE);
    configure_channel(&channel2, pacman_intro_ch2_freq[0], CHANNEL2_VOLUME, WAVE_SQUARE);

    uint32_t t = now_ms();
    pacman_intro_ch1_next_ms = t + pacman_intro_ch1_dur[0];
    pacman_intro_ch2_next_ms = t + pacman_intro_ch2_dur[0];
}

void sound_stop_pacman_intro(void) {
    SOUND_ENTER_CRITICAL();
    pacman_intro_active = false;
    SOUND_EXIT_CRITICAL();
    disable_channel(&channel1);
    disable_channel(&channel2);
}
