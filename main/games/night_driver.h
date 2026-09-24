#ifndef NIGHT_DRIVER_H
#define NIGHT_DRIVER_H

#include "game_common.h"

// Índice en games_list[] (ver games_list.c) -- DEBE coincidir con la
// posición de "NIGHT DRIVER" en esa tabla. En el games_list.c que me
// pasaste es la entrada nº8 (índice 7, 0-based):
//   PONG=0, SPACE INVADERS=1, ASTEROIDS=2, BREAKOUT=3, SCRAMBLE=4,
//   GRAN TRAK=5, LUNAR LANDER=6, NIGHT DRIVER=7, PAC-MAN=8, ...
// Si reordenas games_list[], actualiza este número (y machaca el
// highscore guardado bajo el índice viejo si lo haces).
#define ND_GAME_ID 7

void game_night_driver_run(game_mode_t mode);

#endif // NIGHT_DRIVER_H
