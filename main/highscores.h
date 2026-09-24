#ifndef HIGHSCORES_H
#define HIGHSCORES_H

/**
 * highscores.h - Tabla de records persistente en flash
 *
 * Portado de ArcadePi. Guarda los HS_TOP_SCORES mejores resultados por
 * juego en el último sector de flash disponible. La escritura en flash
 * es la única operación que congela brevemente la CPU (~5ms); ocurre
 * solo al guardar un record nuevo.
 *
 * A diferencia de ArcadePi (vídeo compuesto por PIO+DMA continuo, que
 * necesita pausarse durante la escritura en flash), aquí el renderer
 * solo transmite por SPI bajo demanda (renderer_flush()), así que el
 * guardado en flash es más simple: basta con desactivar interrupciones.
 *
 * Iniciales: 3 caracteres A-Z, con el mismo encoder/botón de selección
 * que usa el menú (controls_menu_up/down/select).
 */

#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Configuración
// ---------------------------------------------------------------------------
#define HS_MAX_GAMES 15   // Máximo de juegos registrables
#define HS_TOP_SCORES 5   // Records guardados por juego
#define HS_NAME_LEN 3     // Longitud de las iniciales (sin null)

// ---------------------------------------------------------------------------
// Tipos
// ---------------------------------------------------------------------------
typedef struct {
    char name[HS_NAME_LEN + 1]; // Iniciales + '\0'
    uint32_t score;
} ScoreEntry;

typedef struct {
    ScoreEntry entries[HS_TOP_SCORES];
    int count; // Entradas válidas actualmente (0..HS_TOP_SCORES)
} ScoreTable;

// ---------------------------------------------------------------------------
// API pública
// ---------------------------------------------------------------------------

/**
 * Inicializa el módulo: lee la flash y carga las tablas en RAM.
 * Debe llamarse una vez en main(), antes de cualquier otra función de
 * este módulo (típicamente justo después de controls_init()).
 */
void highscores_init(void);

/**
 * Devuelve la tabla de records de un juego concreto.
 * @param game_id Índice del juego (0..HS_MAX_GAMES-1)
 */
const ScoreTable *highscores_get(int game_id);

/**
 * Comprueba si una puntuación entra en el top de un juego.
 * Útil para saber si hay que pedir iniciales al jugador.
 */
bool highscores_is_top(int game_id, uint32_t score);

/**
 * Muestra la pantalla de introducción de iniciales (bloqueante) e
 * inserta el record en RAM (se persiste con highscores_flush()).
 * Usa los mismos controles que el menú: gira para cambiar de letra,
 * pulsa para confirmar cada carácter.
 * @param game_id Índice del juego
 * @param score Puntuación a guardar
 */
void highscores_enter(int game_id, uint32_t score);

/** Inserta un record en RAM sin mostrar ninguna pantalla. */
void highscores_add(int game_id, const char *name, uint32_t score);

/** Escribe RAM -> flash. Llamar cuando no haya nada más urgente que
 * hacer (p.ej. al volver de un juego), no en mitad de una animación. */
void highscores_flush(void);

/** Borra todos los records de todos los juegos (RAM + flash). */
void highscores_reset(void);

/**
 * Dibuja la tabla de records de un juego en pantalla (no bloqueante,
 * no hace flush -- llama a renderer_flush() después si hace falta).
 * @param game_id Índice del juego
 * @param title Nombre del juego a mostrar como título
 * @param top_y Coordenada Y donde empezar a dibujar (para encajar
 *   con el layout de cada pantalla que la use)
 */
void highscores_draw(int game_id, const char *title, int top_y);

#endif // HIGHSCORES_H
