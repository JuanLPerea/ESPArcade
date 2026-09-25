# Los 12 juegos: Pong portado de verdad, el resto placeholder
# ("PROXIMAMENTE") hasta que se porten sus fuentes reales.

idf_component_register(
    SRCS
        "st7789.c"
        "renderer.c"
        "controls.c"
        "sound.c"
        "highscores.c"
        "menu.c"
        "games/games_list.c"
        "games/pong.c"
        "games/space_invaders.c"
        "games/asteroids.c"
        "games/breakout.c"
        "games/scramble.c"
        "games/snake.c"
        "games/lunar_lander.c"
        "games/night_driver.c"
        "games/pacman.c"
        "games/paratrooper.c"
        "games/frogger.c"
        "games/tetris.c"
        "ArcadeColor.c"
    INCLUDE_DIRS
        "."
        "games"
    REQUIRES
        driver
        esp_lcd
        esp_adc
        esp_timer
        esp_driver_dac
        nvs_flash
)
