#ifndef GAME_COMMON_H
#define GAME_COMMON_H

// Tipos compartidos por todos los juegos. No tocan hardware, son
// los mismos en Pico y ESP32.

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

#define NUM_GAMES 12

#endif
