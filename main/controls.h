#ifndef CONTROLS_H
#define CONTROLS_H

#include <stdint.h>
#include <stdbool.h>

// Misma API que la version Pico, mas las funciones nuevas que
// necesitan los juegos reales (Pong, etc.) y la pantalla de
// pruebas del menu. Los pines fisicos (2 botones + 1 joystick
// analogico por jugador, en vez de encoder + botones) se definen
// dentro de controls.c, no aqui.

void controls_init(void);

// Sondea botones y ejes. Llamar una vez por iteracion del bucle
// (menu o juego) antes de leer cualquiera de las funciones de
// abajo.
void controls_update(void);

bool controls_menu_up(void);
bool controls_menu_down(void);
bool controls_menu_select(void);

// Movimiento continuo (palas, etc.): idx 0 = eje Y de J1, idx 1 =
// eje Y de J2. Devuelve un valor pequeño con signo, pensado para
// sustituir a "cuentas de encoder este frame" -- con un joystick
// analogico es proporcional a lo inclinado que este el stick (mas
// inclinado = numero mas alto), no un pulso discreto, pero se
// integra con el mismo sistema de aceleracion/inercia que ya
// tenian los juegos (ver enc_momentum() en pong.c).
int controls_get_raw_delta(int idx);

// Boton generico por indice (0=J1_A, 1=J1_B, 2=J2_A, 3=J2_B,
// 4=J1_SW, 5=J2_SW). Nivel actual (no flanco).
bool controls_button_down(int idx);

// Para la pantalla de "PRUEBA CONTROLES" del menu: valor crudo del
// ADC (0-4095) y normalizado (-1000..1000, centro calibrado) de un
// eje. axis_id: 0=J1 X, 1=J1 Y, 2=J2 X, 3=J2 Y.
int controls_debug_axis_raw(int axis_id);
int controls_debug_axis_normalized(int axis_id);

#endif
