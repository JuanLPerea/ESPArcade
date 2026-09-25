#include "st7789.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"

/* ===========================================================
 * st7789.c - Version ESP32 (esp_lcd_panel_st7789 + DMA)
 *
 * CORREGIDO tras localizar el bug real (gracias a un benchmark
 * de referencia que SI funcionaba bien en el mismo hardware):
 *
 *  1. esp_lcd_panel_draw_bitmap() es ASINCRONO -- vuelve antes
 *     de que la DMA termine de transmitir. La UNICA forma fiable
 *     de saber cuando es seguro reutilizar el buffer de origen es
 *     el callback on_color_trans_done + un semaforo. Bajar
 *     trans_queue_depth (lo que se probo antes) NO es suficiente:
 *     solo limita cuantas transacciones se pueden encolar, no
 *     garantiza que la anterior haya terminado de verdad.
 *  2. Modo retrato nativo (240x320) con esp_lcd_panel_mirror(),
 *     SIN swap_xy, evita el lio de gap+rotacion que dio tantos
 *     problemas en landscape.
 *  3. .data_endian se deja SIN especificar (iba a LITTLE antes;
 *     el benchmark de referencia no lo toca en absoluto y con eso
 *     el color sale bien).
 *
 * El framebuffer en RAM, el rectangulo "sucio" y la fuente 5x7
 * siguen siendo la misma logica de siempre (C puro, no toca
 * hardware).
 *
 * Pines: SCK=18 MOSI=23 CS=5 DC=21 RST=22
 * =========================================================== */

#define PIN_SCK   18
#define PIN_MOSI  23
#define PIN_CS    5
#define PIN_DC    21
#define PIN_RST   22

#define TFT_SPI_HOST SPI2_HOST
#define TFT_SPI_HZ   (40 * 1000 * 1000)

// Resolucion: retrato nativo 240x320, confirmado funcionando en
// el benchmark de referencia. TFT_WIDTH/TFT_HEIGHT (en st7789.h)
// apuntan a estas variables, asi que el resto del codigo (menu.c,
// games/*.c) se adapta solo, sin cambios.
uint16_t st7789_screen_w = 240;
uint16_t st7789_screen_h = 320;

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;

// Semaforo que se libera cuando el callback on_color_trans_done
// confirma que una transferencia ha terminado FISICAMENTE. Sin
// esto no hay forma segura de saber cuando se puede reutilizar el
// buffer de origen (ver cabecera del fichero).
static SemaphoreHandle_t s_color_done_sem;

static bool IRAM_ATTR on_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                           esp_lcd_panel_io_event_data_t *edata,
                                           void *user_ctx) {
    BaseType_t high_task_wakeup = pdFALSE;
    xSemaphoreGiveFromISR(s_color_done_sem, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
}

/* ---------------------------------------------------------
 * Framebuffer (doble buffer), en uint16_t nativo (RGB565 normal,
 * sin swap manual de bytes).
 * --------------------------------------------------------- */
#define FB_MAX_PIXELS ((uint32_t)320 * 240)
static uint16_t framebuffer[FB_MAX_PIXELS];

static bool fb_dirty = false;
static uint16_t fb_dirty_x0, fb_dirty_y0, fb_dirty_x1, fb_dirty_y1;

static inline void mark_dirty(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    if (x0 >= TFT_WIDTH || y0 >= TFT_HEIGHT) return;
    if (x1 >= TFT_WIDTH) x1 = TFT_WIDTH - 1;
    if (y1 >= TFT_HEIGHT) y1 = TFT_HEIGHT - 1;
    if (!fb_dirty) {
        fb_dirty_x0 = x0; fb_dirty_y0 = y0;
        fb_dirty_x1 = x1; fb_dirty_y1 = y1;
        fb_dirty = true;
    } else {
        if (x0 < fb_dirty_x0) fb_dirty_x0 = x0;
        if (y0 < fb_dirty_y0) fb_dirty_y0 = y0;
        if (x1 > fb_dirty_x1) fb_dirty_x1 = x1;
        if (y1 > fb_dirty_y1) fb_dirty_y1 = y1;
    }
}

static inline uint32_t fb_index(uint16_t x, uint16_t y) {
    return (uint32_t)y * TFT_WIDTH + x;
}

/* ---------------------------------------------------------
 * Banda de volcado: igual que antes (agrupar filas para no hacer
 * una llamada a draw_bitmap por fila), pero AHORA con espera real
 * al semaforo tras cada banda, antes de reutilizar s_band_buf
 * para la siguiente. Eso es lo que faltaba.
 * --------------------------------------------------------- */
#define FLUSH_BAND_ROWS 40
static uint16_t *s_band_buf; // heap_caps_malloc, DMA-capable

void st7789_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    (void)x0; (void)y0; (void)x1; (void)y1;
}

void st7789_flush(void) {
    if (!fb_dirty) return;

    uint16_t x0 = fb_dirty_x0, y0 = fb_dirty_y0;
    uint16_t x1 = fb_dirty_x1, y1 = fb_dirty_y1;
    uint16_t w = x1 - x0 + 1;

    for (uint16_t band_y0 = y0; band_y0 <= y1; band_y0 += FLUSH_BAND_ROWS) {
        uint16_t rows = (band_y0 + FLUSH_BAND_ROWS - 1 <= y1)
                             ? FLUSH_BAND_ROWS
                             : (uint16_t)(y1 - band_y0 + 1);
        for (uint16_t r = 0; r < rows; r++) {
            uint32_t off = fb_index(x0, band_y0 + r);
            memcpy(&s_band_buf[(size_t)r * w], &framebuffer[off], (size_t)w * sizeof(uint16_t));
        }

        // Limpia cualquier senal residual antes de encolar, igual
        // que el patron del benchmark de referencia.
        xSemaphoreTake(s_color_done_sem, 0);
        esp_lcd_panel_draw_bitmap(s_panel, x0, band_y0, x1 + 1, band_y0 + rows, s_band_buf);
        // Espera BLOQUEANTE a que esta banda termine de verdad
        // antes de tocar s_band_buf otra vez en la siguiente
        // vuelta del bucle. Esto es lo que evita la corrupcion.
        xSemaphoreTake(s_color_done_sem, portMAX_DELAY);
    }
    fb_dirty = false;
}

void st7789_fill_screen(uint16_t color) {
    if (color == COLOR_BLACK || color == COLOR_WHITE) {
        uint8_t b = (color == COLOR_BLACK) ? 0x00 : 0xFF;
        memset(framebuffer, b, (size_t)TFT_WIDTH * TFT_HEIGHT * sizeof(uint16_t));
        mark_dirty(0, 0, TFT_WIDTH - 1, TFT_HEIGHT - 1);
        return;
    }
    st7789_fill_rect(0, 0, TFT_WIDTH, TFT_HEIGHT, color);
}

void st7789_draw_pixel(uint16_t x, uint16_t y, uint16_t color) {
    if (x >= TFT_WIDTH || y >= TFT_HEIGHT) return;
    framebuffer[fb_index(x, y)] = color;
    mark_dirty(x, y, x, y);
}

void st7789_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color) {
    if (x >= TFT_WIDTH || y >= TFT_HEIGHT) return;
    if (w == 0 || h == 0) return;
    if (x + w > TFT_WIDTH) w = TFT_WIDTH - x;
    if (y + h > TFT_HEIGHT) h = TFT_HEIGHT - y;

    for (uint16_t row = 0; row < h; row++) {
        uint32_t offset = fb_index(x, y + row);
        for (uint16_t col = 0; col < w; col++) {
            framebuffer[offset + col] = color;
        }
    }
    mark_dirty(x, y, x + w - 1, y + h - 1);
}
// ---------------------------------------------------------
// Fuente bitmap 5x7. Cubre ' ' (0x20) hasta 'z' (0x7A): espacio,
// símbolos básicos, dígitos 0-9, mayúsculas A-Z y minúsculas a-z.
// Cada carácter son 7 bytes (uno por fila), usando los 5 bits
// bajos de cada byte para las 5 columnas (bit4 = columna
// izquierda, bit0 = columna derecha).
// ---------------------------------------------------------
static const uint8_t font5x7[91][7] = {
    /* ' ' */ {0b00000,0b00000,0b00000,0b00000,0b00000,0b00000,0b00000},
    /* '!' */ {0b00100,0b00100,0b00100,0b00100,0b00100,0b00000,0b00100},
    /* '"' */ {0b01010,0b01010,0b00000,0b00000,0b00000,0b00000,0b00000},
    /* '#' */ {0b01010,0b01010,0b11111,0b01010,0b11111,0b01010,0b01010},
    /* '$' */ {0b00100,0b01111,0b10100,0b01110,0b00101,0b11110,0b00100},
    /* '%' */ {0b11001,0b11010,0b00010,0b00100,0b01000,0b01011,0b10011},
    /* '&' */ {0b01100,0b10010,0b10100,0b01000,0b10101,0b10010,0b01101},
    /* '\''*/ {0b00100,0b00100,0b01000,0b00000,0b00000,0b00000,0b00000},
    /* '(' */ {0b00010,0b00100,0b01000,0b01000,0b01000,0b00100,0b00010},
    /* ')' */ {0b01000,0b00100,0b00010,0b00010,0b00010,0b00100,0b01000},
    /* '*' */ {0b00000,0b10101,0b01110,0b11111,0b01110,0b10101,0b00000},
    /* '+' */ {0b00000,0b00100,0b00100,0b11111,0b00100,0b00100,0b00000},
    /* ',' */ {0b00000,0b00000,0b00000,0b00000,0b00000,0b00100,0b01000},
    /* '-' */ {0b00000,0b00000,0b00000,0b11111,0b00000,0b00000,0b00000},
    /* '.' */ {0b00000,0b00000,0b00000,0b00000,0b00000,0b00000,0b00100},
    /* '/' */ {0b00001,0b00010,0b00010,0b00100,0b01000,0b01000,0b10000},
    /* '0' */ {0b01110,0b10001,0b10011,0b10101,0b11001,0b10001,0b01110},
    /* '1' */ {0b00100,0b01100,0b00100,0b00100,0b00100,0b00100,0b01110},
    /* '2' */ {0b01110,0b10001,0b00001,0b00010,0b00100,0b01000,0b11111},
    /* '3' */ {0b11111,0b00010,0b00100,0b00010,0b00001,0b10001,0b01110},
    /* '4' */ {0b00010,0b00110,0b01010,0b10010,0b11111,0b00010,0b00010},
    /* '5' */ {0b11111,0b10000,0b11110,0b00001,0b00001,0b10001,0b01110},
    /* '6' */ {0b00110,0b01000,0b10000,0b11110,0b10001,0b10001,0b01110},
    /* '7' */ {0b11111,0b00001,0b00010,0b00100,0b01000,0b01000,0b01000},
    /* '8' */ {0b01110,0b10001,0b10001,0b01110,0b10001,0b10001,0b01110},
    /* '9' */ {0b01110,0b10001,0b10001,0b01111,0b00001,0b00010,0b01100},
    /* ':' */ {0b00000,0b00100,0b00000,0b00000,0b00000,0b00100,0b00000},
    /* ';' */ {0b00000,0b00100,0b00000,0b00000,0b00000,0b00100,0b01000},
    /* '<' */ {0b00001,0b00010,0b00100,0b01000,0b00100,0b00010,0b00001},
    /* '=' */ {0b00000,0b00000,0b11111,0b00000,0b11111,0b00000,0b00000},
    /* '>' */ {0b10000,0b01000,0b00100,0b00010,0b00100,0b01000,0b10000},
    /* '?' */ {0b01110,0b10001,0b00001,0b00010,0b00100,0b00000,0b00100},
    /* '@' */ {0b01110,0b10001,0b10111,0b10101,0b10111,0b10000,0b01111},
    /* 'A' */ {0b01110,0b10001,0b10001,0b11111,0b10001,0b10001,0b10001},
    /* 'B' */ {0b11110,0b10001,0b10001,0b11110,0b10001,0b10001,0b11110},
    /* 'C' */ {0b01111,0b10000,0b10000,0b10000,0b10000,0b10000,0b01111},
    /* 'D' */ {0b11110,0b10001,0b10001,0b10001,0b10001,0b10001,0b11110},
    /* 'E' */ {0b11111,0b10000,0b10000,0b11110,0b10000,0b10000,0b11111},
    /* 'F' */ {0b11111,0b10000,0b10000,0b11110,0b10000,0b10000,0b10000},
    /* 'G' */ {0b01111,0b10000,0b10000,0b10111,0b10001,0b10001,0b01111},
    /* 'H' */ {0b10001,0b10001,0b10001,0b11111,0b10001,0b10001,0b10001},
    /* 'I' */ {0b01110,0b00100,0b00100,0b00100,0b00100,0b00100,0b01110},
    /* 'J' */ {0b00001,0b00001,0b00001,0b00001,0b00001,0b10001,0b01110},
    /* 'K' */ {0b10001,0b10010,0b10100,0b11000,0b10100,0b10010,0b10001},
    /* 'L' */ {0b10000,0b10000,0b10000,0b10000,0b10000,0b10000,0b11111},
    /* 'M' */ {0b10001,0b11011,0b10101,0b10101,0b10001,0b10001,0b10001},
    /* 'N' */ {0b10001,0b11001,0b10101,0b10101,0b10011,0b10001,0b10001},
    /* 'O' */ {0b01110,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110},
    /* 'P' */ {0b11110,0b10001,0b10001,0b11110,0b10000,0b10000,0b10000},
    /* 'Q' */ {0b01110,0b10001,0b10001,0b10001,0b10101,0b10010,0b01101},
    /* 'R' */ {0b11110,0b10001,0b10001,0b11110,0b10100,0b10010,0b10001},
    /* 'S' */ {0b01111,0b10000,0b10000,0b01110,0b00001,0b00001,0b11110},
    /* 'T' */ {0b11111,0b00100,0b00100,0b00100,0b00100,0b00100,0b00100},
    /* 'U' */ {0b10001,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110},
    /* 'V' */ {0b10001,0b10001,0b10001,0b10001,0b10001,0b01010,0b00100},
    /* 'W' */ {0b10001,0b10001,0b10001,0b10101,0b10101,0b10101,0b01010},
    /* 'X' */ {0b10001,0b10001,0b01010,0b00100,0b01010,0b10001,0b10001},
    /* 'Y' */ {0b10001,0b10001,0b01010,0b00100,0b00100,0b00100,0b00100},
    /* 'Z' */ {0b11111,0b00001,0b00010,0b00100,0b01000,0b10000,0b11111},
    /* '[' */ {0b01110,0b01000,0b01000,0b01000,0b01000,0b01000,0b01110},
    /* '\\'*/ {0b10000,0b01000,0b01000,0b00100,0b00010,0b00010,0b00001},
    /* ']' */ {0b01110,0b00010,0b00010,0b00010,0b00010,0b00010,0b01110},
    /* '^' */ {0b00100,0b01010,0b10001,0b00000,0b00000,0b00000,0b00000},
    /* '_' */ {0b00000,0b00000,0b00000,0b00000,0b00000,0b00000,0b11111},
    /* '`' */ {0b01000,0b00100,0b00000,0b00000,0b00000,0b00000,0b00000},
    /* 'a' */ {0b00000,0b00000,0b01110,0b00001,0b01111,0b10001,0b01111},
    /* 'b' */ {0b10000,0b10000,0b10000,0b11110,0b10001,0b10001,0b11110},
    /* 'c' */ {0b00000,0b00000,0b01111,0b10000,0b10000,0b10000,0b01111},
    /* 'd' */ {0b00001,0b00001,0b00001,0b01111,0b10001,0b10001,0b01111},
    /* 'e' */ {0b00000,0b00000,0b01110,0b10001,0b11111,0b10000,0b01111},
    /* 'f' */ {0b00110,0b01001,0b01000,0b11100,0b01000,0b01000,0b01000},
    /* 'g' */ {0b00000,0b00000,0b01111,0b10001,0b01111,0b00001,0b01110},
    /* 'h' */ {0b10000,0b10000,0b10000,0b11110,0b10001,0b10001,0b10001},
    /* 'i' */ {0b00100,0b00000,0b01100,0b00100,0b00100,0b00100,0b01110},
    /* 'j' */ {0b00010,0b00000,0b00110,0b00010,0b00010,0b10010,0b01100},
    /* 'k' */ {0b10000,0b10000,0b10010,0b10100,0b11000,0b10100,0b10010},
    /* 'l' */ {0b01100,0b00100,0b00100,0b00100,0b00100,0b00100,0b01110},
    /* 'm' */ {0b00000,0b00000,0b11010,0b10101,0b10101,0b10101,0b10101},
    /* 'n' */ {0b00000,0b00000,0b11110,0b10001,0b10001,0b10001,0b10001},
    /* 'o' */ {0b00000,0b00000,0b01110,0b10001,0b10001,0b10001,0b01110},
    /* 'p' */ {0b00000,0b00000,0b11110,0b10001,0b11110,0b10000,0b10000},
    /* 'q' */ {0b00000,0b00000,0b01111,0b10001,0b01111,0b00001,0b00001},
    /* 'r' */ {0b00000,0b00000,0b10110,0b11001,0b10000,0b10000,0b10000},
    /* 's' */ {0b00000,0b00000,0b01111,0b10000,0b01110,0b00001,0b11110},
    /* 't' */ {0b00100,0b01110,0b00100,0b00100,0b00100,0b00101,0b00010},
    /* 'u' */ {0b00000,0b00000,0b10001,0b10001,0b10001,0b10011,0b01101},
    /* 'v' */ {0b00000,0b00000,0b10001,0b10001,0b10001,0b01010,0b00100},
    /* 'w' */ {0b00000,0b00000,0b10101,0b10101,0b10101,0b10101,0b01010},
    /* 'x' */ {0b00000,0b00000,0b10001,0b01010,0b00100,0b01010,0b10001},
    /* 'y' */ {0b00000,0b00000,0b10001,0b10001,0b01111,0b00001,0b01110},
    /* 'z' */ {0b00000,0b00000,0b11111,0b00010,0b00100,0b01000,0b11111},
};

// ---------------------------------------------------------
// Iconos -- códigos 0x01-0x05 (ver ICON_* en st7789.h), fuera del
// rango imprimible normal, así que van en una tabla aparte en vez
// de intentar encajarlos como índices negativos de font5x7[].
// ---------------------------------------------------------
static const uint8_t font5x7_icons[5][7] = {
    /* 0x01 ICON_SMILEY  */ {0b01110,0b10001,0b01010,0b10001,0b01010,0b10001,0b01110},
    /* 0x02 ICON_SPADE   */ {0b00100,0b01110,0b11111,0b11111,0b00100,0b01110,0b00100},
    /* 0x03 ICON_HEART   */ {0b01010,0b11111,0b11111,0b11111,0b01110,0b00100,0b00000},
    /* 0x04 ICON_DIAMOND */ {0b00100,0b01110,0b11111,0b11111,0b01110,0b00100,0b00000},
    /* 0x05 ICON_CLUB    */ {0b00100,0b01110,0b00100,0b11111,0b11111,0b00100,0b01110},
};

void st7789_draw_char(uint16_t x, uint16_t y, char c, uint16_t color, uint16_t bg, uint8_t scale) {
    if (scale == 0) scale = 1;

    unsigned char uc = (unsigned char)c;
    const uint8_t *glyph;

    if (uc >= 1 && uc <= 5) {
        glyph = font5x7_icons[uc - 1];
    } else {
        if (uc < ' ' || uc > 'z') uc = ' '; // fuera de rango -> hueco en blanco
        glyph = font5x7[uc - ' '];
    }

    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            bool on = (bits >> (FONT_WIDTH - 1 - col)) & 0x01;
            uint16_t px_color = on ? color : bg;
            st7789_fill_rect(x + col * scale, y + row * scale, scale, scale, px_color);
        }
    }
}

void st7789_draw_text(uint16_t x, uint16_t y, const char *str, uint16_t color, uint16_t bg, uint8_t scale) {
    uint16_t cursor_x = x;
    while (*str) {
        st7789_draw_char(cursor_x, y, *str, color, bg, scale);
        cursor_x += (FONT_WIDTH + 1) * scale;
        str++;
    }
}

uint16_t st7789_text_width(const char *str, uint8_t scale) {
    if (scale == 0) scale = 1;
    size_t len = 0;
    while (str[len]) len++;
    if (len == 0) return 0;
    return (uint16_t)(len * (FONT_WIDTH + 1) * scale - scale);
}

/* ---------------------------------------------------------
 * blit: escritura DIRECTA al panel. Aqui tambien hace falta
 * esperar al semaforo, porque el buffer que nos pasa el llamador
 * es suyo y no sabemos cuanto va a tardar en reutilizarlo -- si
 * no esperamos, podria sobreescribirlo antes de que termine de
 * transmitirse.
 * --------------------------------------------------------- */
void st7789_blit(uint16_t x0, uint16_t y0, uint16_t w, uint16_t h, const uint16_t *buf) {
    xSemaphoreTake(s_color_done_sem, 0);
    esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x0 + w, y0 + h, buf);
    xSemaphoreTake(s_color_done_sem, portMAX_DELAY);
}

void st7789_blit_to_buffer(uint16_t x0, uint16_t y0, uint16_t w, uint16_t h, const uint16_t *buf) {
    if (x0 >= TFT_WIDTH || y0 >= TFT_HEIGHT) return;
    if (x0 + w > TFT_WIDTH) w = TFT_WIDTH - x0;
    if (y0 + h > TFT_HEIGHT) h = TFT_HEIGHT - y0;

    for (uint16_t row = 0; row < h; row++) {
        uint32_t offset = fb_index(x0, y0 + row);
        memcpy(&framebuffer[offset], buf + (uint32_t)row * w, (size_t)w * sizeof(uint16_t));
    }
    mark_dirty(x0, y0, x0 + w - 1, y0 + h - 1);
}

void st7789_set_rotation(uint8_t rotation) {
    // Ahora que el bug real de sincronizacion esta arreglado,
    // recuperamos swap_xy para probar apaisado de verdad -- antes
    // no podiamos saber si el gap/swap_xy fallaban por si mismos o
    // por la corrupcion de fondo que lo enmascaraba todo.
    switch (rotation & 0x03) {
        case 0:
            esp_lcd_panel_swap_xy(s_panel, false);
            esp_lcd_panel_mirror(s_panel, true, true);
            st7789_screen_w = 240; st7789_screen_h = 320;
            break;
        case 1:
            esp_lcd_panel_swap_xy(s_panel, true);
            esp_lcd_panel_mirror(s_panel, true, false);
            st7789_screen_w = 320; st7789_screen_h = 240;
            break;
        case 2:
            esp_lcd_panel_swap_xy(s_panel, false);
            esp_lcd_panel_mirror(s_panel, false, false);
            st7789_screen_w = 240; st7789_screen_h = 320;
            break;
        default: // 3
            esp_lcd_panel_swap_xy(s_panel, true);
            esp_lcd_panel_mirror(s_panel, false, true);
            st7789_screen_w = 320; st7789_screen_h = 240;
            break;
    }
}

void st7789_init(void) {
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_SCK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 320 * FLUSH_BAND_ROWS * sizeof(uint16_t),
    };
    spi_bus_initialize(TFT_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);

    s_color_done_sem = xSemaphoreCreateBinary();

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_DC,
        .cs_gpio_num = PIN_CS,
        .pclk_hz = TFT_SPI_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = on_color_trans_done,
        .user_ctx = NULL,
    };
    esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)TFT_SPI_HOST, &io_config, &s_io);

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        // data_endian NO se especifica a proposito -- igual que en
        // el benchmark de referencia que funciona bien en este
        // mismo panel.
    };
    esp_lcd_new_panel_st7789(s_io, &panel_config, &s_panel);

    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_set_gap(s_panel, 0, 0);
    st7789_set_rotation(1); // apaisada 320x240
    esp_lcd_panel_disp_on_off(s_panel, true);

    s_band_buf = heap_caps_malloc(320 * FLUSH_BAND_ROWS * sizeof(uint16_t), MALLOC_CAP_DMA);

    memset(framebuffer, 0, sizeof(framebuffer));
    mark_dirty(0, 0, TFT_WIDTH - 1, TFT_HEIGHT - 1);
    st7789_flush();
}