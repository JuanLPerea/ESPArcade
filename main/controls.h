#ifndef CONTROLS_H
#define CONTROLS_H

#include <stdint.h>
#include <stdbool.h>

// Misma API que la version Pico. Los pines fisicos (2 botones +
// 1 joystick analogico por jugador, en vez de encoder + botones)
// se definen dentro de controls.c, no aqui -- nada fuera de ese
// fichero necesita conocerlos.

void controls_init(void);

// Sondea botones y ejes. Llamar una vez por iteracion del bucle
// (menu o juego) antes de leer cualquiera de las funciones de
// abajo.
void controls_update(void);

bool controls_menu_up(void);
bool controls_menu_down(void);
bool controls_menu_select(void);

#endif