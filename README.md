# ESPArcade

12 juegos clásicos para montar una mini-consola arcade con una placa ESP32 WROOM y una pantalla TFT SPI de 240x320 px
Controles: 2 mandos ananógicos y 4 botones, para jugador 1 y 2
1 Speaker 
## Pinout

Pinout completo para ESP32-WROOM-32 DevKit de 30 pines. Son 16 señales,
por lo que quedan varios pines libres.

| Bloque | Señal | GPIO | Notas |
|---|---|---:|---|
| **TFT ST7789** | SCK | **18** | IOMUX VSPI → 40 MHz limpios |
| | MOSI (SDA) | **23** | IOMUX VSPI |
| | CS | **5** | Strapping, pero como salida es inofensivo |
| | DC | **21** | |
| | RST | **22** | |
| | BLK *(opcional)* | **27** | Solo si quieres regular brillo por LEDC |
| **Joystick J1** | VRx | **32** | ADC1_CH4 |
| | VRy | **33** | ADC1_CH5 |
| | SW | **19** | Pull-up interno |
| **Joystick J2** | VRx | **34** | ADC1_CH6, solo entrada |
| | VRy | **35** | ADC1_CH7, solo entrada |
| | SW | **13** | Pull-up interno |
| **Botones** | J1_A | **4** | |
| | J1_B | **14** | |
| | J2_A | **16** | |
| | J2_B | **17** | |
| **Audio** | DAC / I2S | **25** | DAC1, 8 bits |


### Reglas que respeta este mapeo

1. **Nada en GPIO 1 y 3**  
   Son el UART del CH340/CP2102. Se necesitan para flashear y para `printf`.

2. **Nada con pull-up en GPIO 12**  
   Si está alto al arrancar, el chip configura la flash a 1,8 V y la
   placa no arranca.

3. **Los cuatro ejes analógicos están en ADC1**  
   ADC2 deja de funcionar en cuanto se activa WiFi, y el driver de IDF
   lo bloquea igualmente.

4. **Botones y SW a masa, sin resistencias**  
   Usar `GPIO_PULLUP_ONLY` en `gpio_config()`. La lógica es invertida:
   `0 = pulsado`, `1 = suelto`.
