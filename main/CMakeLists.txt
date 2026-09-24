# Primera version con codigo "real" navegable en pantalla:
# st7789 + renderer + controls + menu, con una games_list de
# relleno (games/games_list.c) hasta que porteemos los 12 juegos.

idf_component_register(
    SRCS
        "st7789.c"
        "renderer.c"
        "controls.c"
        "sound.c"
        "highscores.c"
        "menu.c"
        "games/games_list.c"
        "ArcadeColor.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        driver
        esp_lcd
        esp_adc
        esp_timer
        esp_driver_dac
        nvs_flash
)