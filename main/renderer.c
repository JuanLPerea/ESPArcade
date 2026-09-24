#include "renderer.h"

void renderer_init(void) {
    st7789_init();
}

void renderer_clear(uint16_t color) {
    st7789_fill_screen(color);
}

void renderer_draw_text(uint16_t x, uint16_t y, const char *text, uint16_t color, uint16_t bg, uint8_t scale) {
    st7789_draw_text(x, y, text, color, bg, scale);
}

void renderer_draw_char(uint16_t x, uint16_t y, char c, uint16_t color, uint16_t bg, uint8_t scale) {
    st7789_draw_char(x, y, c, color, bg, scale);
}

void renderer_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color) {
    st7789_fill_rect(x, y, w, h, color);
}

void renderer_draw_pixel(uint16_t x, uint16_t y, uint16_t color) {
    st7789_draw_pixel(x, y, color);
}

void renderer_flush(void) {
    st7789_flush();
}

void renderer_blit_to_buffer(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *buf) {
    st7789_blit_to_buffer(x, y, w, h, buf);
}