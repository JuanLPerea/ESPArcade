#include "sound.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "driver/dac_continuous.h"

/* ============================================================
 * PORT A ESP32 - lo que cambia respecto al original:
 *
 *  - Salida: PWM+GP4 (Pico) -> DAC+DMA en GPIO25 (ESP32). En vez
 *    de una interrupcion de timer que escribe una muestra cada
 *    vez, una tarea de FreeRTOS genera un BLOQUE de muestras y lo
 *    entrega a dac_continuous_write(), que se encarga del DMA.
 *  - Seccion critica: save_and_disable_interrupts/restore_interrupts
 *    (Pico) -> portENTER_CRITICAL/portEXIT_CRITICAL con un spinlock
 *    (equivalente en FreeRTOS/ESP-IDF).
 *  - Temporizacion de la musica: absolute_time_t/time_reached
 *    (Pico) -> milisegundos con esp_timer_get_time().
 *
 * TODO LO DEMAS (generadores de onda, mezcla, tablas de melodia,
 * DDS por acumulador de fase) es EXACTAMENTE la misma logica que
 * el sound.c original: es matematica pura, no toca hardware.
 * ============================================================ */

#define SOUND_DAC_GPIO 25  // DAC1

#define AUDIO_SAMPLE_RATE 22050
#define CHANNEL1_VOLUME 75
#define CHANNEL2_VOLUME 55
#define CHANNEL3_VOLUME 100

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
    // Apagado automatico por duracion (solo se usa en el canal de
    // efectos; los canales de musica se paran/cambian desde
    // sound_update() y dejan esto a 0 = "sin limite propio").
    volatile uint32_t samples_left;
} audio_channel_t;

static dac_continuous_handle_t s_dac;
static volatile bool sound_initialized = false;
static volatile bool sound_enabled = true;

static volatile audio_channel_t channel1;
static volatile audio_channel_t channel2;
static volatile audio_channel_t channel3;

// Spinlock ESP-IDF: equivalente a save_and_disable_interrupts/
// restore_interrupts del Pico SDK, para proteger el acceso a los
// canales entre la tarea de audio y las llamadas desde menu/juegos.
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
#define SOUND_ENTER_CRITICAL() portENTER_CRITICAL(&s_lock)
#define SOUND_EXIT_CRITICAL()  portEXIT_CRITICAL(&s_lock)

/* ============================================================
 * MÚSICA (tablas identicas al original)
 * ============================================================ */
#define MUSIC_NOTE_COUNT 64
#define NOTE_C3 131
#define NOTE_D3 147
#define NOTE_E3 165
#define NOTE_F3 175
#define NOTE_G3 196
#define NOTE_A3 220
#define NOTE_B3 247
#define NOTE_C4 262
#define NOTE_D4 294
#define NOTE_E4 330
#define NOTE_F4 349
#define NOTE_G4 392
#define NOTE_A4 440
#define NOTE_B4 494
#define NOTE_C5 523
#define NOTE_D5 587
#define NOTE_E5 659
#define NOTE_F5 698
#define NOTE_G5 784
#define NOTE_A5 880
#define NOTE_B5 988

static const uint16_t melody_notes[MUSIC_NOTE_COUNT] = {
    NOTE_C5, NOTE_E5, NOTE_G5, NOTE_A5,
    NOTE_G5, NOTE_E5, NOTE_D5, 0,
    NOTE_C5, NOTE_D5, NOTE_E5, NOTE_G5,
    NOTE_E5, NOTE_D5, NOTE_C5, 0,
    NOTE_E5, NOTE_G5, NOTE_B5, NOTE_A5,
    NOTE_G5, NOTE_E5, NOTE_D5, 0,
    NOTE_C5, NOTE_E5, NOTE_G5, NOTE_E5,
    NOTE_D5, NOTE_C5, NOTE_D5, 0,
    NOTE_G4, NOTE_C5, NOTE_E5, NOTE_G5,
    NOTE_A5, NOTE_G5, NOTE_E5, 0,
    NOTE_E5, NOTE_D5, NOTE_C5, NOTE_D5,
    NOTE_E5, NOTE_G5, NOTE_A5, 0,
    NOTE_A5, NOTE_G5, NOTE_E5, NOTE_D5,
    NOTE_C5, NOTE_D5, NOTE_E5, NOTE_G5,
    NOTE_A5, NOTE_G5, NOTE_E5, NOTE_D5,
    NOTE_C5, NOTE_E5, NOTE_D5, NOTE_C5
};

static const uint16_t melody_duration[MUSIC_NOTE_COUNT] = {
    220, 220, 220, 360,
    220, 220, 420, 140,
    200, 200, 240, 360,
    240, 220, 440, 140,
    220, 220, 260, 360,
    240, 220, 440, 140,
    220, 220, 300, 360,
    240, 220, 440, 160,
    220, 220, 220, 280,
    360, 220, 440, 140,
    220, 220, 300, 220,
    240, 240, 480, 160,
    300, 220, 240, 220,
    360, 220, 220, 300,
    300, 240, 260, 240,
    400, 300, 300, 800
};

static const uint16_t bass_notes[MUSIC_NOTE_COUNT] = {
    NOTE_C3, 0, NOTE_G3, 0,
    NOTE_C3, 0, NOTE_G3, 0,
    NOTE_C3, 0, NOTE_G3, 0,
    NOTE_C3, 0, NOTE_G3, 0,
    NOTE_A3, 0, NOTE_E3, 0,
    NOTE_A3, 0, NOTE_E3, 0,
    NOTE_F3, 0, NOTE_C3, 0,
    NOTE_G3, 0, NOTE_G3, 0,
    NOTE_C3, 0, NOTE_G3, 0,
    NOTE_C3, 0, NOTE_G3, 0,
    NOTE_A3, 0, NOTE_E3, 0,
    NOTE_A3, 0, NOTE_E3, 0,
    NOTE_F3, 0, NOTE_C3, 0,
    NOTE_G3, 0, NOTE_C3, 0,
    NOTE_F3, 0, NOTE_G3, 0,
    NOTE_C3, 0, NOTE_G3, 0
};

static volatile bool menu_music_playing = false;
static volatile uint32_t music_index = 0;
static uint32_t music_next_change_ms = 0; // ms desde el arranque (esp_timer_get_time()/1000)

static inline uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ============================================================
 * DDS / generadores de onda - IDENTICO al original, es
 * matematica pura, no toca hardware.
 * ============================================================ */
static uint32_t frequency_to_phase_increment(uint16_t frequency) {
    if (frequency == 0) return 0;
    return (uint32_t)(((uint64_t)frequency << 32) / AUDIO_SAMPLE_RATE);
}

static void configure_channel(volatile audio_channel_t *channel, uint16_t frequency, uint16_t volume, uint8_t waveform) {
    if (frequency == 0) {
        channel->active = false;
        channel->frequency = 0;
        channel->phase_increment = 0;
        return;
    }
    channel->frequency = frequency;
    channel->phase = 0;
    channel->phase_increment = frequency_to_phase_increment(frequency);
    channel->volume = volume;
    channel->waveform = waveform;
    channel->noise_state = 0x12345678u ^ ((uint32_t)frequency * 2654435761u);
    if (channel->noise_state == 0) channel->noise_state = 1;
    channel->samples_left = 0; // 0 = sin limite (lo gestiona sound_update)
    channel->active = true;
}

// Igual que configure_channel, pero programa un apagado automatico
// tras duration_ms. Pensado para el canal de efectos: se dispara y
// se olvida, sin que nadie tenga que llamar a sound_effect_stop().
static void configure_channel_timed(volatile audio_channel_t *channel, uint16_t frequency, uint16_t volume, uint8_t waveform, uint16_t duration_ms) {
    configure_channel(channel, frequency, volume, waveform);
    if (channel->active) {
        channel->samples_left = ((uint32_t)duration_ms * AUDIO_SAMPLE_RATE) / 1000;
    }
}

static void disable_channel(volatile audio_channel_t *channel) {
    channel->active = false;
}

static int16_t wave_square(uint32_t phase) {
    return (phase & 0x80000000u) ? 127 : -127;
}

static int16_t wave_triangle(uint32_t phase) {
    uint16_t p = (uint16_t)(phase >> 16);
    int32_t value;
    if (p < 32768) {
        value = ((int32_t)p * 4) - 65536;
    } else {
        value = 196608 - ((int32_t)p * 4);
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

    // Apagado automatico: si este canal tiene un limite de duracion
    // (samples_left != 0), cuenta hacia atras muestra a muestra y se
    // desactiva solo al llegar a 0. Los canales sin limite (musica,
    // samples_left == 0) no se ven afectados.
    if (channel->samples_left > 0) {
        channel->samples_left--;
        if (channel->samples_left == 0) {
            channel->active = false;
        }
    }

    return (sample * (int16_t)channel->volume) / 100;
}

// Genera UNA muestra de 8 bits sin signo (128 = silencio), mezclando
// los 3 canales -- misma logica de mezcla/limitador que el original.
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
 * Genera bloques de muestras y los entrega al DAC via DMA.
 * ============================================================ */
#define AUDIO_BLOCK_SAMPLES 256

static void audio_task(void *arg) {
    (void)arg;
    uint8_t block[AUDIO_BLOCK_SAMPLES];
    while (true) {
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
            block[i] = generate_sample_u8();
        }
        // Bloqueante hasta que el DMA tiene hueco: esto es lo que
        // marca el ritmo real de generacion (~22050 muestras/s),
        // igual que antes lo marcaba el timer de 45us del Pico.
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

    // Tarea de audio en el core 1, prioridad alta: no queremos que
    // el juego/menu (core 0) le robe tiempo y se note en el pitch.
    xTaskCreatePinnedToCore(audio_task, "sound_task", 2048, NULL, configMAX_PRIORITIES - 2, NULL, 1);

    sound_initialized = true;
}

/* ============================================================
 * TONO GENÉRICO
 * ============================================================ */
void sound_play_tone(uint16_t frequency_hz, uint16_t duration_ms) {
    if (!sound_initialized) sound_init();
    configure_channel_timed(&channel3, frequency_hz, CHANNEL3_VOLUME, WAVE_SQUARE, duration_ms);
}

/* ============================================================
 * MÚSICA
 * ============================================================ */
void sound_start_menu_music(void) {
    if (!sound_initialized) sound_init();

    SOUND_ENTER_CRITICAL();
    music_index = 0;
    menu_music_playing = true;
    configure_channel(&channel1, melody_notes[0], CHANNEL1_VOLUME, WAVE_TRIANGLE);
    configure_channel(&channel2, bass_notes[0], CHANNEL2_VOLUME, WAVE_TRIANGLE);
    music_next_change_ms = now_ms() + melody_duration[0];
    SOUND_EXIT_CRITICAL();
}

void sound_stop_menu_music(void) {
    SOUND_ENTER_CRITICAL();
    menu_music_playing = false;
    disable_channel(&channel1);
    disable_channel(&channel2);
    SOUND_EXIT_CRITICAL();
}

// Ligera a proposito: el audio se genera en audio_task(); aqui solo
// avanzamos la secuencia musical, igual que en el original.
void sound_update(void) {
    if (!menu_music_playing) return;
    if (now_ms() < music_next_change_ms) return;

    SOUND_ENTER_CRITICAL();
    music_index++;
    if (music_index >= MUSIC_NOTE_COUNT) music_index = 0;

    uint16_t melody = melody_notes[music_index];
    uint16_t bass = bass_notes[music_index];
    configure_channel(&channel1, melody, CHANNEL1_VOLUME, WAVE_TRIANGLE);
    configure_channel(&channel2, bass, CHANNEL2_VOLUME, WAVE_TRIANGLE);
    music_next_change_ms = now_ms() + melody_duration[music_index];
    SOUND_EXIT_CRITICAL();
}

bool sound_menu_music_is_playing(void) {
    return menu_music_playing;
}

/* ============================================================
 * EFECTOS -- cada uno con una duracion razonable, para que se
 * apaguen solos (antes se quedaban sonando indefinidamente).
 * ============================================================ */
void sound_effect_shoot(void) {
    if (!sound_initialized) sound_init();
    configure_channel_timed(&channel3, 1100, CHANNEL3_VOLUME, WAVE_SQUARE, 80);
}

void sound_effect_explosion(void) {
    if (!sound_initialized) sound_init();
    configure_channel_timed(&channel3, 100, CHANNEL3_VOLUME, WAVE_NOISE, 350);
}

void sound_effect_select(void) {
    if (!sound_initialized) sound_init();
    configure_channel_timed(&channel3, 880, CHANNEL3_VOLUME, WAVE_SQUARE, 100);
}

void sound_effect_move(void) {
    if (!sound_initialized) sound_init();
    configure_channel_timed(&channel3, 660, CHANNEL3_VOLUME, WAVE_SQUARE, 50);
}

void sound_effect_game_over(void) {
    if (!sound_initialized) sound_init();
    configure_channel_timed(&channel3, 180, CHANNEL3_VOLUME, WAVE_SAW, 600);
}

void sound_effect_success(void) {
    if (!sound_initialized) sound_init();
    configure_channel_timed(&channel3, 1047, CHANNEL3_VOLUME, WAVE_TRIANGLE, 300);
}

void sound_effect_stop(void) {
    disable_channel(&channel3);
}

/* ============================================================
 * CONTROL GENERAL
 * ============================================================ */
void sound_mute(void) {
    sound_enabled = false;
}

void sound_unmute(void) {
    sound_enabled = true;
}