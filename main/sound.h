#ifndef SOUND_H
#define SOUND_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Motor de audio de 3 canales.
 *
 * CANAL 1 -> melodía
 * CANAL 2 -> acompañamiento / bajo
 * CANAL 3 -> efectos de sonido
 *
 * Los tres canales se mezclan digitalmente. En la version ESP32
 * salen por GPIO25 (DAC1) via DMA en vez de PWM+GP4 como en la Pico.
 *
 * Todas las funciones públicas son NO BLOQUEANTES.
 */

void sound_init(void);

/*
 * Compatible con la API anterior.
 * NO bloquea. Genera un tono utilizando el canal de efectos.
 */
void sound_play_tone(uint16_t frequency_hz, uint16_t duration_ms);

/*
 * Música del menú.
 */
void sound_start_menu_music(void);
void sound_stop_menu_music(void);
void sound_update(void);
bool sound_menu_music_is_playing(void);

/*
 * Efectos de sonido. Todos son NO BLOQUEANTES.
 */
void sound_effect_shoot(void);
void sound_effect_explosion(void);
void sound_effect_select(void);
void sound_effect_move(void);
void sound_effect_game_over(void);
void sound_effect_success(void);

/*
 * Control del canal de efectos.
 */
void sound_effect_stop(void);

/*
 * Control general.
 */
void sound_mute(void);
void sound_unmute(void);

#endif