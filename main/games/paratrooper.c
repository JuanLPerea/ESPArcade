#include "paratrooper.h"
#include "../renderer.h"
#include "../controls.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// PLACEHOLDER: pendiente de portar. Sustituir por el .c real
// (adaptando controles/sonido igual que se hizo con pong.c) cuando
// este disponible.
void game_paratrooper_run(game_mode_t mode) {
    renderer_clear(COLOR_BLACK);
    renderer_draw_text(20, 100, "PARATROOPER", COLOR_CYAN, COLOR_BLACK, 2);
    renderer_draw_text(20, 130, "PROXIMAMENTE", COLOR_WHITE, COLOR_BLACK, 2);
    if (mode != GAME_MODE_DEMO) {
        renderer_draw_text(20, 165, "Pulsa SELECT", COLOR_WHITE, COLOR_BLACK, 1);
        renderer_draw_text(20, 180, "para volver", COLOR_WHITE, COLOR_BLACK, 1);
    }
    renderer_flush();

    // En modo demo (attract mode del menu, sin nadie tocando
    // botones) hay que salir solos tras un rato, o el menu se
    // quedaria colgado aqui para siempre esperando un SELECT que
    // nunca llega.
    int elapsed_ms = 0;
    const int DEMO_TIMEOUT_MS = 3000;

    while (true) {
        controls_update();
        if (controls_menu_select()) break;
        if (mode == GAME_MODE_DEMO && elapsed_ms >= DEMO_TIMEOUT_MS) break;
        vTaskDelay(pdMS_TO_TICKS(15));
        elapsed_ms += 15;
    }
}
