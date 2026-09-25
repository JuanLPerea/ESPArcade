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
 * Los tres canales se mezclan digitalmente y salen por:
 *
 *     GP4 -> PWM -> altavoz / amplificador
 *
 * Todas las funciones públicas son NO BLOQUEANTES.
 */

void sound_init(void);

/*
 * Compatible con la API anterior.
 *
 * Ahora NO bloquea.
 * Genera un tono utilizando el canal de efectos.
 */
void sound_play_tone(
    uint16_t frequency_hz,
    uint16_t duration_ms
);


/*
 * Música del menú.
 */
void sound_start_menu_music(void);
void sound_stop_menu_music(void);
void sound_update(void);

bool sound_menu_music_is_playing(void);


/*
 * Música in-game de Tetris (Korobeiniki), canales 1+2 en bucle --
 * el canal 3 (efectos) sigue libre para mover/girar/línea/game over.
 * Llamar sound_start_tetris_music() al empezar la partida y
 * sound_stop_tetris_music() al terminarla (game over, o al salir).
 */
void sound_start_tetris_music(void);
void sound_stop_tetris_music(void);
bool sound_tetris_music_is_playing(void);


/*
 * Efectos de sonido.
 *
 * Todos son NO BLOQUEANTES.
 */
void sound_effect_shoot(void);
void sound_effect_explosion(void);
void sound_effect_select(void);
void sound_effect_move(void);
void sound_effect_game_over(void);
void sound_effect_success(void);
void sound_effect_lose_point(void);
void sound_effect_victory(void);
void sound_siren_start(void);
void sound_siren_stop(void);

/*
 * Motor -- zumbido continuo cuyo tono sube con la velocidad (canal 2,
 * compartido con sound_siren_start() y la música de menú -- quien se
 * active el último se queda el canal). Llamar sound_engine_set_speed()
 * una vez por frame con la velocidad actual (0 = ralentí, 255 = a
 * fondo); no hace falta limitar tú la frecuencia de llamada. Llamar
 * sound_engine_stop() al salir del juego de conducción.
 */
void sound_engine_set_speed(uint8_t speed_pct);
void sound_engine_stop(void);

/*
 * Derrape -- ruido continuo (canal 1, libre durante la partida) para
 * cuando el coche patina -- p.ej. velocidad alta + volante girado a
 * fondo. Llamar sound_skid_start() en cada frame mientras se cumplan
 * las condiciones de derrape y sound_skid_stop() en cuanto se dejen
 * de cumplir; llamar start() repetidamente mientras ya suena no hace
 * nada, así que no hace falta que el juego lleve su propio estado de
 * "¿ya está sonando?".
 */
void sound_skid_start(void);
void sound_skid_stop(void);

/*
 * Control del canal de efectos.
 */
void sound_effect_stop(void);


/*
 * Control general.
 */
void sound_mute(void);
void sound_unmute(void);


/*
 * Efectos y música de Pac-Man.
 *
 * Portados desde el motor de sonido de ArcadePi (SFX_TICTAC_LO/HI,
 * SFX_LEVEL_UP, SFX_SCORE, SFX_AST_SMALL, SFX_GAME_OVER_LOSE y
 * MUSIC_PACMAN), que usaba un enum de efectos con datos de nota
 * embebidos. Este motor no tiene ese enum -- cada efecto es una
 * función propia, igual que sound_effect_shoot()/sound_effect_move()
 * etc. de arriba.
 *
 * Todas NO BLOQUEANTES, igual que el resto del motor.
 */

/* Come-punto alterno (tic/tac): pasa true/false para alternar el tono
 * en cada punto comido, igual que hacía el jingle waa-waa original. */
void sound_effect_pacman_chomp(bool alt);

/* Power pellet comida (los fantasmas se vuelven frightened). */
void sound_effect_pacman_power(void);

/* Fruta/bonus recogida. */
void sound_effect_pacman_fruit(void);

/* Fantasma comido en modo frightened. */
void sound_effect_pacman_eat_ghost(void);

/* Pac-Man muere. */
void sound_effect_pacman_death(void);

/* Nivel superado (mismo sonido que sound_effect_pacman_power(),
 * nombre propio para el sitio de la llamada). */
void sound_effect_pacman_levelup(void);

/* Jingle de inicio de partida (~4300ms, una sola vez, no bloqueante).
 * Usa canal 1 (melodía) + canal 2 (bajo) -- si sonaba la música de
 * menú, la para. Termina sola; sound_stop_pacman_intro() la corta
 * antes de tiempo si hace falta (p.ej. si se sale del juego). */
void sound_start_pacman_intro(void);
void sound_stop_pacman_intro(void);

#endif