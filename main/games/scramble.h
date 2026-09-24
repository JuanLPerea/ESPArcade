#ifndef SCRAMBLE_H
#define SCRAMBLE_H

#include "game_common.h"

#define SCR_GAME_ID 4   // debe coincidir con el índice de "SCRAMBLE" en games_list.c
                         // -- este juego sustituye al que antes se llamaba "DEFENDER";
                         // usa el MISMO número de índice que tenía DEFENDER en tu
                         // games_list.c/menu.c (ajusta este valor si no es 4).

void game_scramble_run(game_mode_t mode);

#endif
