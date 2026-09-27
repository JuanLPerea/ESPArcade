#include "st7789.h"

#include <string.h>
#include <stdio.h>
#include <assert.h>
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
 *
 * RECTANGULO(S) SUCIO(S) -- SEGUNDA VUELTA:
 *
 * La primera version llevaba UN solo rectangulo sucio (una caja
 * englobante de todo lo dibujado desde el ultimo flush). Con pocos
 * objetos moviendose eso es barato, pero con muchos objetos
 * PEQUEÑOS y DISPERSOS por la pantalla (balas, particulas de varias
 * explosiones a la vez) esa caja crece hasta cubrir casi todo el
 * campo de juego -- se manda muchisimos mas bytes de los que
 * realmente cambiaron.
 *
 * El primer intento de arreglo (en asteroids.c) fue hacer un
 * flush() por objeto individual para mantener cada caja minima.
 * Eso evita la caja gigante, pero cambia el problema por otro: cada
 * flush() en ESP32 no es gratis como spi_write_blocking() en la
 * Pico (un bucle directo sin sistema operativo de por medio) --
 * aqui cada flush() implica encolar una transaccion DMA y esperar
 * de verdad al callback on_color_trans_done via un semaforo, que
 * tiene su propio coste fijo (turno de ISR, cambio de contexto de
 * FreeRTOS...). Con decenas de objetos pequeños cada uno con su
 * propio flush(), ese coste fijo, multiplicado por muchos, puede
 * pesar tanto o mas que la caja gigante que se queria evitar.
 *
 * La solucion de verdad es llevar una LISTA de varios rectangulos
 * sucios en vez de uno solo (s_dirty[], hasta MAX_DIRTY_RECTS), y
 * fusionar SOLO los que estan realmente cerca entre si (ver
 * should_merge() -- el criterio es que fusionarlos no desperdicie
 * mas del triple del area real). Dos balas en polos opuestos de la
 * pantalla quedan como dos rectangulos separados y pequeños; un
 * grupo de particulas de una misma explosion, al estar juntas, se
 * fusiona solo entre ellas en un rectangulo razonable. Si en algun
 * momento hay mas zonas sueltas de las que caben en la lista (caso
 * raro: caos total en pantalla), se renuncia a la lista ESE frame y
 * se cae de vuelta al comportamiento anterior (un unico rectangulo
 * englobante) -- sigue siendo correcto, solo menos eficiente, y
 * nunca peor que la version anterior a este cambio.
 *
 * Ademas, la transmision de banda a banda ahora usa DOBLE BUFFER
 * (s_band_buf[0]/[1], ping-pong): mientras la DMA transmite una
 * banda, la CPU ya puede ir copiando la siguiente al OTRO buffer,
 * en vez de quedarse parada esperando a que la banda actual termine
 * antes de tocar nada mas. Solo se espera de verdad cuando hace
 * falta reutilizar un buffer que todavia esta en vuelo (como mucho
 * 2 transacciones en vuelo a la vez, una por buffer).
 *
 * El framebuffer en RAM y la fuente 5x7 siguen siendo la misma
 * logica de siempre (C puro, no toca hardware).
 *
 * Pines: SCK=18 MOSI=23 CS=5 DC=21 RST=22
 * =========================================================== */

#define PIN_SCK   18
#define PIN_MOSI  23
#define PIN_CS    5
#define PIN_DC    21
#define PIN_RST   22

#define TFT_SPI_HOST SPI2_HOST
#define TFT_SPI_HZ   (80 * 1000 * 1000)

// Resolucion: retrato nativo 240x320, confirmado funcionando en
// el benchmark de referencia. TFT_WIDTH/TFT_HEIGHT (en st7789.h)
// apuntan a estas variables, asi que el resto del codigo (menu.c,
// games/*.c) se adapta solo, sin cambios.
uint16_t st7789_screen_w = 240;
uint16_t st7789_screen_h = 320;

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;

// Semaforo CONTADOR (no binario): cada banda en vuelo suma una
// "cuenta pendiente" al encolarse, y se resta al esperarla. Con
// doble buffer puede haber como mucho 2 en vuelo a la vez, de ahi
// el limite de 2. Un binario no vale aqui porque necesitamos saber
// CUANTAS transacciones han terminado, no solo si alguna ha
// terminado.
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
 *
 * RESERVADO EN HEAP, no static -- 320*240*2 = 153600 bytes (150KB)
 * es demasiado para vivir como array `static` en dram0_0_seg (la
 * región fija del enlazador donde tiene que caber TODA la memoria
 * .bss/.data de TODOS los .c del proyecto, juegos incluidos). Con
 * esta placa (ESP32 WROOM, sin PSRAM) sigue siendo RAM interna
 * normal -- heap_caps_malloc() sin más flags que MALLOC_CAP_8BIT
 * tira del heap general, que es una región del enlazador bastante
 * más holgada que dram0_0_seg. st7789_init() lo reserva una sola
 * vez al arrancar y no se libera nunca (vive toda la vida del
 * firmware, igual que antes como array `static`) -- si
 * heap_caps_malloc() devuelve NULL aquí, no hay pantalla posible,
 * así que se aborta con ESP_ERROR_CHECK en vez de seguir con un
 * puntero nulo.
 * --------------------------------------------------------- */
#define FB_MAX_PIXELS ((uint32_t)320 * 240)
#define FB_SIZE_BYTES  (FB_MAX_PIXELS * sizeof(uint16_t))
static uint16_t *framebuffer;

/* ---------------------------------------------------------
 * Lista de rectangulos sucios -- ver comentario largo de arriba.
 * --------------------------------------------------------- */
#define MAX_DIRTY_RECTS 8

typedef struct { uint16_t x0, y0, x1, y1; } dirty_rect_t;

static dirty_rect_t s_dirty[MAX_DIRTY_RECTS];
static int          s_dirty_count = 0;

static inline uint32_t rect_area(const dirty_rect_t *r) {
    return (uint32_t)(r->x1 - r->x0 + 1) * (uint32_t)(r->y1 - r->y0 + 1);
}

static inline void rect_union(dirty_rect_t *dst, const dirty_rect_t *a, const dirty_rect_t *b) {
    dst->x0 = a->x0 < b->x0 ? a->x0 : b->x0;
    dst->y0 = a->y0 < b->y0 ? a->y0 : b->y0;
    dst->x1 = a->x1 > b->x1 ? a->x1 : b->x1;
    dst->y1 = a->y1 > b->y1 ? a->y1 : b->y1;
}

// Cuanto se "desperdicia" al fusionar dos rectangulos en uno solo,
// respecto a la suma de sus areas reales. Un valor bajo (p.ej. 3)
// significa que solo se fusionan los que ya estaban realmente cerca
// -- dos objetos en polos opuestos de la pantalla producen una
// union enorme comparada con la suma de sus areas, asi que NUNCA
// pasan este umbral y se quedan como rectangulos separados.
#define MERGE_WASTE_FACTOR 3

static inline bool should_merge(const dirty_rect_t *a, const dirty_rect_t *b) {
    dirty_rect_t u;
    rect_union(&u, a, b);
    return (uint64_t)rect_area(&u) <= (uint64_t)(rect_area(a) + rect_area(b)) * MERGE_WASTE_FACTOR;
}

static void mark_dirty(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    if (x0 >= TFT_WIDTH || y0 >= TFT_HEIGHT) return;
    if (x1 >= TFT_WIDTH)  x1 = TFT_WIDTH - 1;
    if (y1 >= TFT_HEIGHT) y1 = TFT_HEIGHT - 1;

    dirty_rect_t nr = { x0, y0, x1, y1 };

    // 1) Si ya hay un rectangulo cercano/solapado en la lista,
    //    fusiona ahi mismo (no cuenta como una region nueva).
    for (int i = 0; i < s_dirty_count; i++) {
        if (should_merge(&s_dirty[i], &nr)) {
            rect_union(&s_dirty[i], &s_dirty[i], &nr);
            return;
        }
    }

    // 2) Si no, y todavia hay hueco en la lista, se añade como
    //    region nueva independiente.
    if (s_dirty_count < MAX_DIRTY_RECTS) {
        s_dirty[s_dirty_count++] = nr;
        return;
    }

    // 3) Lista llena y esta nueva zona no esta cerca de ninguna de
    //    las que ya hay -- en vez de acumular MAS listas/mas
    //    transacciones sueltas (la misma sobrecarga que queremos
    //    evitar), se renuncia a la lista ESTE frame y se colapsa
    //    TODO en un unico rectangulo englobante, exactamente el
    //    comportamiento (correcto, aunque menos eficiente) de la
    //    version anterior. Con MAX_DIRTY_RECTS=8 deberia ser rarisimo
    //    en la practica -- solo con caos simultaneo de verdad.
    dirty_rect_t all = s_dirty[0];
    for (int i = 1; i < s_dirty_count; i++) rect_union(&all, &all, &s_dirty[i]);
    rect_union(&all, &all, &nr);
    s_dirty[0] = all;
    s_dirty_count = 1;
}

static inline uint32_t fb_index(uint16_t x, uint16_t y) {
    return (uint32_t)y * TFT_WIDTH + x;
}

/* ---------------------------------------------------------
 * Banda de volcado: agrupa filas para no hacer una llamada a
 * draw_bitmap por fila, con DOBLE BUFFER (ping-pong) para que la
 * DMA de una banda pueda ir transmitiendo mientras la CPU ya
 * prepara la siguiente en el OTRO buffer -- ver comentario largo
 * de cabecera.
 * --------------------------------------------------------- */
#define FLUSH_BAND_ROWS 40
static uint16_t *s_band_buf[2]; // heap_caps_malloc, DMA-capable

void st7789_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    (void)x0; (void)y0; (void)x1; (void)y1;
}

// Manda UN rectangulo (ya recortado a pantalla) en bandas de
// FLUSH_BAND_ROWS filas, con doble buffer: como mucho 2 bandas en
// vuelo a la vez (una por buffer), solo se espera de verdad cuando
// hace falta reutilizar un buffer que la DMA todavia no ha
// terminado de transmitir.
static void flush_one_rect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    uint16_t w = x1 - x0 + 1;
    int buf_idx = 0;
    int in_flight = 0;

    for (uint16_t band_y0 = y0; band_y0 <= y1; band_y0 += FLUSH_BAND_ROWS) {
        uint16_t rows = (band_y0 + FLUSH_BAND_ROWS - 1 <= y1)
                             ? FLUSH_BAND_ROWS
                             : (uint16_t)(y1 - band_y0 + 1);

        if (in_flight == 2) {
            // Los 2 buffers estan ocupados -- espera a que el mas
            // antiguo (el que estamos a punto de reescribir) haya
            // terminado de verdad.
            xSemaphoreTake(s_color_done_sem, portMAX_DELAY);
            in_flight--;
        }

        uint16_t *buf = s_band_buf[buf_idx];
        for (uint16_t r = 0; r < rows; r++) {
            uint32_t off = fb_index(x0, band_y0 + r);
            memcpy(&buf[(size_t)r * w], &framebuffer[off], (size_t)w * sizeof(uint16_t));
        }

        esp_lcd_panel_draw_bitmap(s_panel, x0, band_y0, x1 + 1, band_y0 + rows, buf);
        in_flight++;
        buf_idx ^= 1;
    }

    // Antes de volver, hay que asegurarse de que TODAS las bandas en
    // vuelo de este rectangulo han terminado de verdad -- si no, el
    // siguiente rectangulo (u otra llamada) podria pisar un buffer
    // que la DMA todavia esta leyendo.
    while (in_flight > 0) {
        xSemaphoreTake(s_color_done_sem, portMAX_DELAY);
        in_flight--;
    }
}

void st7789_flush(void) {
    for (int i = 0; i < s_dirty_count; i++) {
        flush_one_rect(s_dirty[i].x0, s_dirty[i].y0, s_dirty[i].x1, s_dirty[i].y1);
    }
    s_dirty_count = 0;
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

    s_color_done_sem = xSemaphoreCreateCounting(2, 0); // maximo 2 en vuelo (doble buffer)

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

    // Framebuffer principal PRIMERO, antes que s_band_buf[] -- orden
    // importante, no arbitrario. En esta placa (ESP32 WROOM) el heap
    // interno arranca ya fragmentado en varios bloques no contiguos
    // (así es el mapa de memoria de la ESP32); el framebuffer (150KB)
    // solo cabe entero en el bloque más grande de todos. Si
    // s_band_buf[] se reservara antes y el asignador metiera esos
    // ~50KB en ese mismo bloque grande, ya no quedaría hueco
    // contiguo para el framebuffer después -- eso es exactamente lo
    // que pasaba en el orden anterior (assert saltando en tiempo de
    // arranque con heap total de sobra, pero sin un solo bloque de
    // 150KB libre). Reservando el framebuffer primero se queda con
    // ese bloque grande casi entero, y s_band_buf[] (más pequeño)
    // encaja después en el siguiente bloque libre sin problema.
    //
    // NOTA: en la ESP32 "clásica" (a diferencia de la S2/S3) casi
    // toda la RAM interna es DMA-capable, así que MALLOC_CAP_DMA y
    // MALLOC_CAP_8BIT tiran del mismo heap, no de pools separados --
    // el framebuffer no necesita MALLOC_CAP_DMA porque nunca se pasa
    // directo a esp_lcd_panel_draw_bitmap() (solo se copia banda a
    // banda a s_band_buf[], que sí lo es), pero eso no lo aísla de
    // competir por el mismo espacio; lo que de verdad evita el
    // conflicto es el ORDEN de reserva, no la capability distinta.
    // DIAGNÓSTICO TEMPORAL -- ver qué bloque libre hay de verdad justo
    // antes de cada reserva, y qué puntero concreto sale NULL. El
    // assert combinado no decía CUÁL de los tres fallaba; esto sí.
    // Quitar este bloque (dejando solo las 3 líneas de malloc + un
    // assert normal) en cuanto boot sin problemas.
    printf("[st7789] antes de framebuffer: 8BIT free=%u largest=%u | DMA free=%u largest=%u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

    framebuffer = heap_caps_malloc(FB_SIZE_BYTES, MALLOC_CAP_8BIT);
    printf("[st7789] framebuffer (%u bytes) = %p\n", (unsigned)FB_SIZE_BYTES, (void *)framebuffer);

    printf("[st7789] antes de band_buf: 8BIT free=%u largest=%u | DMA free=%u largest=%u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

    s_band_buf[0] = heap_caps_malloc(320 * FLUSH_BAND_ROWS * sizeof(uint16_t), MALLOC_CAP_DMA);
    printf("[st7789] s_band_buf[0] (%u bytes) = %p\n",
           (unsigned)(320 * FLUSH_BAND_ROWS * sizeof(uint16_t)), (void *)s_band_buf[0]);

    s_band_buf[1] = heap_caps_malloc(320 * FLUSH_BAND_ROWS * sizeof(uint16_t), MALLOC_CAP_DMA);
    printf("[st7789] s_band_buf[1] (%u bytes) = %p\n",
           (unsigned)(320 * FLUSH_BAND_ROWS * sizeof(uint16_t)), (void *)s_band_buf[1]);

    if (!framebuffer)     printf("[st7789] FALLO: framebuffer es NULL\n");
    if (!s_band_buf[0])   printf("[st7789] FALLO: s_band_buf[0] es NULL\n");
    if (!s_band_buf[1])   printf("[st7789] FALLO: s_band_buf[1] es NULL\n");
    fflush(stdout);

    assert(framebuffer && s_band_buf[0] && s_band_buf[1]
           && "st7789: sin RAM suficiente para framebuffer/band buffers");

    memset(framebuffer, 0, FB_SIZE_BYTES);
    mark_dirty(0, 0, TFT_WIDTH - 1, TFT_HEIGHT - 1);
    st7789_flush();
}