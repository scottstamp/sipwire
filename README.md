# sipwire

Wi-Fi SIP handset firmware for ESP32-S3 (Asterisk-compatible). ESP-IDF v6.1,
LVGL 9 via `espressif/esp_lvgl_port`.

Docs: [hardware](docs/HARDWARE.md) · [architecture](docs/ARCHITECTURE.md) · [plan](docs/PLAN.md)

## Layout

```
main/
  main.c          app entry
  ui/ui.c/.h      LVGL screens
  idf_component.yml  managed deps: lvgl/lvgl 9.6, esp_lvgl_port 2.x
components/display/  ILI9341 + XPT2046 + PWM backlight via esp_lcd / esp_lvgl_port
  idf_component.yml  managed deps: lvgl/lvgl ~9.6, espressif/esp_lvgl_port ^2.9,
                     espressif/esp_lcd_ili9341 ^2.1, atanisoft/esp_lcd_touch_xpt2046 ^1.0.6
                     (which pulls espressif/esp_lcd_touch)
components/ota/      firmware update over HTTP(S) (`ota <url>`, Settings > Update), rollback safety;
                     uses ESP-IDF's own esp_https_ota (no managed component added)
sdkconfig.defaults   FreeRTOS + LVGL Kconfig defaults
```

## Build

```bash
. $IDF_PATH/export.sh
idf.py set-target esp32s3   # or your chip
idf.py build flash monitor
```

## Next steps

- Verify display orientation, colour order and touch axes on the real board
  (see `components/display/include/display.h`).
- Run Settings -> Touch calibration on the real board and check the result
  (`display_touch_raw_hook()`, `components/ui/scr_calibrate.c`).
