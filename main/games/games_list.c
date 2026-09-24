#include "games_list.h"
#include "../renderer.h"
#include "../controls.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Placeholder: en vez de un juego real, muestra un mensaje y
// espera a que el jugador pulse seleccionar para volver al menu.
// Sirve para probar de extremo a extremo el flujo completo
// menu -> "juego" -> menu con controls.c y renderer.c reales.
static void placeholder_game(game_mode_t mode) {
    (void)mode;

    renderer_clear(COLOR_BLACK);
    renderer_draw_text(20, 100, "PROXIMAMENTE", COLOR_CYAN, COLOR_BLACK, 2);
    renderer_draw_text(20, 140, "PULSA SELECT", COLOR_WHITE, COLOR_BLACK, 2);
    renderer_draw_text(20, 165, "PARA VOLVER", COLOR_WHITE, COLOR_BLACK, 2);
    renderer_flush();

    while (true) {
        controls_update();
        if (controls_menu_select()) break;
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

const game_entry_t games_list[] = {
    { "PONG",           placeholder_game },
    { "BREAKOUT",       placeholder_game },
    { "SPACE INVADERS", placeholder_game },
};

const int NUM_GAMES = sizeof(games_list) / sizeof(games_list[0]);