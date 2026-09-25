#include "games_list.h"

#include "pong.h"
#include "space_invaders.h"
#include "asteroids.h"
#include "breakout.h"
#include "scramble.h"
#include "snake.h"
#include "lunar_lander.h"
#include "night_driver.h"
#include "pacman.h"
#include "paratrooper.h"
#include "frogger.h"
#include "tetris.h"

const game_entry_t games_list[NUM_GAMES] = {
    { "PONG",           game_pong_run },
    { "SPACE INVADERS", game_space_invaders_run },
    { "ASTEROIDS",      game_asteroids_run },
    { "BREAKOUT",       game_breakout_run },
    { "SCRAMBLE",       game_scramble_run },
    { "SNAKE",          game_snake_run },
    { "LUNAR LANDER",   game_lunar_lander_run },
    { "NIGHT DRIVER",   game_night_driver_run },
    { "PAC-MAN",        game_pacman_run },
    { "PARATROOPER",    game_paratrooper_run },
    { "FROGGER",        game_frogger_run },
    { "TETRIS",         game_tetris_run },
};
