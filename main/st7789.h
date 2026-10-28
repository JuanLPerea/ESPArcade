#ifndef ST7789_H
#define ST7789_H

#include <stdint.h>
#include <stdbool.h>

/* ===========================================================
 * st7789.h - API identica a la version Pico. Renderer, menu y
 * los 12 juegos incluyen este fichero y no necesitan saber que
 * por debajo ahora es esp_lcd_panel_st7789 en vez de spi_master
 * a pelo.
 *
 * Pines fisicos (definidos en st7789.c, no hace falta tocarlos
 * desde fuera): SCK=18 MOSI=23 CS=5 DC=21 RST=22
 * =========================================================== */

// Resolucion efectiva actual (se ajusta con st7789_set_rotation).
extern uint16_t st7789_screen_w;
extern uint16_t st7789_screen_h;
#define TFT_WIDTH  st7789_screen_w
#define TFT_HEIGHT st7789_screen_h

// Colores basicos en formato RGB565 (identico a la version Pico)
#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_BLUE    0x001F
#define COLOR_YELLOW  0xFFE0
#define COLOR_CYAN    0x07FF
#define COLOR_MAGENTA 0xF81F

void st7789_init(void);

// Se mantiene por compatibilidad de firma, pero ya no hace falta
// llamarla desde fuera: esp_lcd_panel_draw_bitmap fija la ventana
// de direccionamiento internamente en cada llamada. No-op real.
void st7789_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

/* -----------------------------------------------------------
 * DOBLE BUFFER (framebuffer en RAM) - mismo contrato que en Pico:
 * nada se ve en pantalla hasta st7789_flush(), y flush() solo
 * transmite el rectangulo que ha cambiado desde el flush anterior.
 * ----------------------------------------------------------- */
void st7789_flush(void);

// Como st7789_flush(), pero transmite como mucho ~max_bytes en esta
// llamada. Lo que no cabe queda pendiente y se manda en los siguientes
// flush. Pensado para el flush de cada frame de juego: con un SPI
// lento, en escenas cargadas se ralentiza el refresco de pantalla en
// vez de todo el bucle del juego. Las pantallas de titulo/menus deben
// seguir usando st7789_flush() (envia todo).
void st7789_flush_limited(uint32_t max_bytes);

// Presupuesto recomendado por frame de juego: ~10 ms de SPI a 20 MHz.
// Ajustalo entre 18000 (si el juego va a trompicones) y 32000 (si ves
// la pantalla "a tirones" en las explosiones grandes).
#define FLUSH_BUDGET_BYTES 24000
void st7789_fill_screen(uint16_t color);
void st7789_draw_pixel(uint16_t x, uint16_t y, uint16_t color);
void st7789_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

/* -----------------------------------------------------------
 * Texto (fuente bitmap 5x7) - identica a la version Pico.
 * ----------------------------------------------------------- */
#define FONT_WIDTH  5
#define FONT_HEIGHT 7

void st7789_draw_char(uint16_t x, uint16_t y, char c, uint16_t color, uint16_t bg, uint8_t scale);
void st7789_draw_text(uint16_t x, uint16_t y, const char *str, uint16_t color, uint16_t bg, uint8_t scale);
uint16_t st7789_text_width(const char *str, uint8_t scale);

/* -----------------------------------------------------------
 * st7789_blit: vuelca un buffer EXTERNO directo al panel, en el
 * acto, sin pasar por el framebuffer interno ni por el
 * rectangulo sucio. Mismo aviso que en la version Pico: si lo
 * mezclas con las funciones de dibujo de arriba, el orden en
 * pantalla puede no coincidir porque estas se acumulan hasta el
 * siguiente flush() y blit() escribe inmediatamente.
 * ----------------------------------------------------------- */
void st7789_blit(uint16_t x0, uint16_t y0, uint16_t w, uint16_t h, const uint16_t *buf);

// Igual que st7789_blit, pero copia AL framebuffer interno
// (marcando la zona sucia) en vez de escribir directo. Se ve en
// el siguiente st7789_flush(), igual que el resto de dibujo.
void st7789_blit_to_buffer(uint16_t x0, uint16_t y0, uint16_t w, uint16_t h, const uint16_t *buf);

/* -----------------------------------------------------------
 * Rotacion de pantalla: 0=0 grados, 1=90, 2=180, 3=270.
 * Llamar despues de st7789_init(). Si la imagen no sale como se
 * espera, prueba los otros 3 valores.
 * ----------------------------------------------------------- */
void st7789_set_rotation(uint8_t rotation);

#endif // ST7789_H