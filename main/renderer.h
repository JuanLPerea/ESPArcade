#ifndef RENDERER_H
#define RENDERER_H

#include "st7789.h"

// Capa fina sobre el driver st7789: los juegos y el menú dibujan a
// través de esta interfaz en vez de llamar directo al driver, para
// poder cambiar de driver/pantalla en el futuro sin tocar el resto
// del proyecto.
//
// IMPORTANTE (doble buffer): renderer_clear/draw_text/fill_rect/
// draw_pixel ya NO tocan la pantalla física -- escriben en un
// framebuffer en RAM. Hace falta llamar a renderer_flush() para
// que lo dibujado se transmita y se vea. Puedes acumular varios
// draws entre dos flush sin coste de SPI; solo se transmite la
// zona que realmente cambió.

void renderer_init(void);
void renderer_clear(uint16_t color);
void renderer_draw_text(uint16_t x, uint16_t y, const char *text, uint16_t color, uint16_t bg, uint8_t scale);
void renderer_draw_char(uint16_t x, uint16_t y, char c, uint16_t color, uint16_t bg, uint8_t scale);
void renderer_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void renderer_draw_pixel(uint16_t x, uint16_t y, uint16_t color);

// Transmite al panel físico solo la zona que ha cambiado desde el
// último flush. Sin esto, lo dibujado no se ve.
void renderer_flush(void);

// Como renderer_draw_pixel/fill_rect, pero para un bloque de
// píxeles ya preparado (p.ej. un sprite): lo copia al framebuffer
// en vez de mandarlo directo por SPI, para que respete el mismo
// orden que el resto del frame y aparezca en el siguiente flush.
void renderer_blit_to_buffer(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *buf);

#endif