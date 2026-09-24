#ifndef GAMES_LIST_H
#define GAMES_LIST_H

/* ===========================================================
 * STUB TEMPORAL: esto no es el games_list.h real del proyecto
 * (aun no lo hemos portado). Sirve solo para poder compilar y
 * probar menu.c con datos de verdad -- 3 entradas de relleno
 * en vez de los 12 juegos -- hasta que toque portar games
 * =========================================================== */

typedef enum {
    GAME_MODE_1P = 0,
    GAME_MODE_2P,
    GAME_MODE_DEMO,
} game_mode_t;

typedef void (*game_run_fn)(game_mode_t mode);

typedef struct {
    const char *name;
    game_run_fn run;
} game_entry_t;

extern const game_entry_t games_list[];
extern const int NUM_GAMES;

#endif