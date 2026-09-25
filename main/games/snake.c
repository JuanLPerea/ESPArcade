#include "snake.h"
#include "../renderer.h"
#include "../controls.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// PLACEHOLDER: pendiente de portar. Sustituir por el .c real
// (adaptando controles/sonido igual que se hizo con pong.c) cuando
// este disponible.
void game_snake_run(game_mode_t mode) {
    (void)mode;

    renderer_clear(COLOR_BLACK);
    renderer_draw_text(20, 100, "SNAKE", COLOR_CYAN, COLOR_BLACK, 2);
    renderer_draw_text(20, 130, "PROXIMAMENTE", COLOR_WHITE, COLOR_BLACK, 2);
    renderer_draw_text(20, 165, "Pulsa SELECT", COLOR_WHITE, COLOR_BLACK, 1);
    renderer_draw_text(20, 180, "para volver", COLOR_WHITE, COLOR_BLACK, 1);
    renderer_flush();

    while (true) {
        controls_update();
        if (controls_menu_select()) break;
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}
