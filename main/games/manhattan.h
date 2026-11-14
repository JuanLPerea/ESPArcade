#ifndef MANHATTAN_H
#define MANHATTAN_H

#include "game_common.h"

/* Id del juego para highscores_*(): posicion (0-based) en games_list[].
 * Manhattan va el 14o => 13 (HS_MAX_GAMES es 15, cabe). */
#ifndef MH_GAME_ID
#define MH_GAME_ID 13
#endif

/* MANHATTAN ARCADE: LABERINTO NEUROTICO. Apaisado 320x240, 1 jugador
 * (GAME_MODE_2P = 1P). GAME_MODE_DEMO solo muestra el titulo 5 s. */
void game_manhattan_run(game_mode_t mode);

#endif
