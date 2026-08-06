# UI slave (LVGL) — quick start

This is a minimal skeleton PlatformIO project for the second ESP32 that runs a touch UI (LVGL) and communicates with the main OpenChess board over Serial2.

Wiring (connect to main board master):
- 3.3V -> 3.3V
- GND  -> GND
- UI RX (GPIO25) <- Master TX (GPIO25)
- UI TX (GPIO34) -> Master RX (GPIO34)

Notes:
- Configure `TFT_eSPI/User_Setup.h` for your display and pins.
- The LVGL display driver here is a minimal flush that draws pixels via `tft.drawPixel()`; replace with optimized pushImage if needed.
- The UI sends `TOUCH|action=hint;x=0;y=0` on button press and expects `HINT|move=<uci>` from the master.
