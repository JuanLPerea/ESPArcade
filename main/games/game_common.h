#ifndef GAME_COMMON_H
#define GAME_COMMON_H

// Modo de ejecución con el que el menú lanza cada juego
typedef enum {
    GAME_MODE_1P,
    GAME_MODE_2P,
    GAME_MODE_DEMO
} game_mode_t;

// Firma común que debe tener la función de entrada de cada juego
typedef void (*game_run_fn)(game_mode_t mode);

// Entrada de la lista de juegos del menú: nombre a mostrar + función de arranque
typedef struct {
    const char *name;
    game_run_fn run;
} game_entry_t;

#define NUM_GAMES 12

#endif
