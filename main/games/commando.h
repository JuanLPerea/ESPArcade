#ifndef COMMANDO_H
#define COMMANDO_H

#include "game_common.h"   /* mismo include que use paratrooper.h */

/* Id del juego para highscores_*(): es su posición (0-based) en games_list[],
 * igual que PT_GAME_ID en paratrooper.h. Commando va el 13º => 12.
 * Ajústalo si lo colocas en otro sitio de la lista. */
#ifndef CMD_GAME_ID
#define CMD_GAME_ID 12
#endif

/* MISSING IN ARCADE (Commando 1985), vertical 240x320. Firma game_run_fn:
 * 1 jugador; GAME_MODE_2P = 1P; GAME_MODE_DEMO solo muestra el título 5 s (fase 1). */
void game_commando_run(game_mode_t mode);

#endif