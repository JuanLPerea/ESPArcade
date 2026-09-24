#include "menu.h"
#include "renderer.h"
#include "controls.h"
#include "games/games_list.h"

// Unico cambio respecto al menu.c original: sleep_ms() del Pico SDK
// sustituido por vTaskDelay() de FreeRTOS. Es la unica linea de todo
// el fichero que tocaba un SDK especifico de plataforma; el resto de
// la logica (scroll, seleccion, redibujado parcial) es identica.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define VISIBLE_ITEMS 8
#define ITEM_HEIGHT 28
#define ITEM_SCALE 2
#define LIST_TOP 40

// Dibuja (o redibuja) una única fila de la lista, en su estado
// normal o seleccionado. display_row es la posición en pantalla
// (0 = primera fila visible), no el índice del juego.
static void draw_item(int game_idx, int display_row, bool is_selected) {
    uint16_t y = LIST_TOP + display_row * ITEM_HEIGHT;
    uint16_t bg = is_selected ? COLOR_YELLOW : COLOR_BLACK;
    uint16_t color = is_selected ? COLOR_BLACK : COLOR_WHITE;

    renderer_fill_rect(5, y - 3, TFT_WIDTH - 10, ITEM_HEIGHT - 4, bg);
    renderer_draw_text(15, y, games_list[game_idx].name, color, bg, ITEM_SCALE);
}

// Redibuja todo: título, separador y las filas visibles.
// Solo hace falta en el primer dibujado y cuando cambia el scroll.
static void draw_full_menu(int selected, int scroll_offset) {
    renderer_clear(COLOR_BLACK);
    renderer_draw_text(15, 10, "SELECCIONA JUEGO", COLOR_YELLOW, COLOR_BLACK, 2);
    renderer_fill_rect(10, 32, TFT_WIDTH - 20, 2, COLOR_CYAN);

    int visible = VISIBLE_ITEMS;
    if (visible > NUM_GAMES) visible = NUM_GAMES;

    for (int i = 0; i < visible; i++) {
        int idx = scroll_offset + i;
        if (idx >= NUM_GAMES) break;
        draw_item(idx, i, idx == selected);
    }
}

void menu_run(void) {
    int selected = 0;
    int scroll_offset = 0;
    int prev_selected = -1;
    int prev_scroll = -1;

    while (true) {
        controls_update();

        if (controls_menu_down()) {
            if (selected < NUM_GAMES - 1) selected++;
        }
        if (controls_menu_up()) {
            if (selected > 0) selected--;
        }

        // Ajusta el scroll para que el elemento seleccionado quede siempre visible
        if (selected < scroll_offset) {
            scroll_offset = selected;
        }
        if (selected >= scroll_offset + VISIBLE_ITEMS) {
            scroll_offset = selected - VISIBLE_ITEMS + 1;
        }

        if (scroll_offset != prev_scroll) {
            // Cambió qué tramo de la lista se ve: hay que redibujar todo
            draw_full_menu(selected, scroll_offset);
        } else if (selected != prev_selected) {
            // Solo cambió la selección dentro del mismo tramo visible:
            // redibuja únicamente las dos filas afectadas -> sin parpadeo
            if (prev_selected >= 0) {
                int old_row = prev_selected - scroll_offset;
                if (old_row >= 0 && old_row < VISIBLE_ITEMS) {
                    draw_item(prev_selected, old_row, false);
                }
            }
            int new_row = selected - scroll_offset;
            draw_item(selected, new_row, true);
        }

        prev_selected = selected;
        prev_scroll = scroll_offset;

        renderer_flush();

        if (controls_menu_select()) {
            // TODO: cuando exista el submenú 1P/2P/Demo, sustituir este
            // GAME_MODE_1P fijo por el modo que elija el jugador.
            games_list[selected].run(GAME_MODE_1P);

            // Al volver del juego, fuerza un redibujado completo
            prev_selected = -1;
            prev_scroll = -1;
        }

        vTaskDelay(pdMS_TO_TICKS(15));
    }
}