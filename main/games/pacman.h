#ifndef PACMAN_H
#define PACMAN_H

#include "game_common.h"

// AJUSTAR: debe coincidir con el índice de "PAC-MAN" en games_list.c
// (no tengo ese archivo a la vista; verifica el valor real antes de compilar).
#define PM_GAME_ID 8

void game_pacman_run(game_mode_t mode);

#endif
