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
 * Fanfarria de victoria de Scramble (~3200ms, una sola vez, no
 * bloqueante). Melodía original de este proyecto: arpegio ascendente
 * de Do mayor y resolución en el Do agudo, canal 1 (melodía,
 * triangular) + canal 2 (bajo, cuadrada) -- si sonaba música de menú
 * o de Tetris, la para.
 *
 * El canal 3 (efectos) queda libre a propósito, así las explosiones
 * del jefe se siguen oyendo por encima de la música. Termina sola;
 * sound_stop_scramble_victory() la corta antes de tiempo si hace
 * falta (p.ej. al salir del juego a mitad).
 */
void sound_start_scramble_victory(void);
void sound_stop_scramble_victory(void);
bool sound_scramble_victory_is_playing(void);


/*
 * Pistas de la música del menú.
 *
 *   0 -> GREENSLEEVES   (la original, Mi menor, 6/4)
 *   1 -> NEON CIRCUIT   (La menor, arcade de arpegios, 12.8s)
 *   2 -> STAR PATROL    (Do mayor, marcha espacial más rápida, 9.6s)
 *
 * sound_set_menu_track() se puede llamar con la música ya sonando:
 * el cambio se nota al momento. Un valor fuera de rango se ignora.
 */
#define SOUND_MENU_TRACK_COUNT 3

void    sound_set_menu_track(uint8_t track);
void    sound_next_menu_track(void);
uint8_t sound_get_menu_track(void);

/* Duración en ms de una vuelta completa a la pista "track" (suma de
 * todas sus notas). Para encadenar pistas con un hueco de silencio
 * entre medias sin tener que llevar esa cuenta aparte -- ver el menú
 * principal. Un "track" fuera de rango devuelve 0. */
uint32_t sound_menu_track_duration_ms(uint8_t track);


/*
 * Jingle de inicio de Paratrooper: Toccata y fuga en re menor BWV 565
 * (J. S. Bach, dominio público), adaptación del compás de apertura a
 * 2 voces en octavas, canales 1+2. UNA SOLA PASADA (~8.6s, no en
 * bucle) -- igual que sound_start_pacman_intro(): suena una vez al
 * empezar la partida y se apaga sola, dejando sonar solo los efectos
 * del canal 3 (disparos, explosiones...) el resto de la partida.
 * sound_stop_paratrooper_music() la corta antes de tiempo si hace
 * falta (p.ej. al salir del juego a mitad).
 */
void sound_start_paratrooper_music(void);
void sound_stop_paratrooper_music(void);
bool sound_paratrooper_music_is_playing(void);


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
 * Efectos añadidos. Todos sobre el canal 3 (no pisan la música de
 * fondo de los canales 1+2) y todos NO BLOQUEANTES.
 *
 * Los marcados como "barrido" deslizan la frecuencia de forma
 * continua en vez de saltar de nota en nota, que es lo que les da el
 * carácter de "pew" / "whoop" en vez de sonar a escalera.
 */
void sound_effect_laser(void);        /* barrido: disparo agudo y seco   */
void sound_effect_laser_big(void);    /* barrido: disparo grave y potente*/
void sound_effect_powerup(void);      /* barrido ascendente: mejora      */
void sound_effect_powerdown(void);    /* barrido descendente: pérdida    */
void sound_effect_coin(void);         /* 2 notas: bonus recogido         */
void sound_effect_jump(void);         /* barrido corto ascendente: salto */
void sound_effect_hit(void);          /* ruido seco: impacto recibido    */
void sound_effect_alarm(void);        /* 4 notas alternas: aviso         */
void sound_effect_teleport(void);     /* barrido largo: materialización  */
void sound_effect_bounce(void);       /* 2 notas cortas: rebote          */
void sound_effect_thrust(void);       /* ruido grave: propulsor          */
void sound_effect_extra_life(void);   /* arpegio de 4 notas: 1UP         */

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