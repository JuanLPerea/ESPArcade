#include "renderer.h"
#include "controls.h"
#include "sound.h"
#include "highscores.h"
#include "menu.h"

void app_main(void) {
    renderer_init();
    controls_init();
    sound_init();
    highscores_init();
    menu_run(); // no retorna nunca en uso normal
}