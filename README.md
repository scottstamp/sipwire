# sipwire

Wi-Fi SIP handset firmware for ESP32-S3 (Asterisk-compatible). ESP-IDF v6.1,
LVGL 9 via `espressif/esp_lvgl_port`.

Docs: [hardware](docs/HARDWARE.md) · [architecture](docs/ARCHITECTURE.md) · [plan](docs/PLAN.md)

## Layout

```
main/
  main.c          app entry
  display.c/.h    LVGL port init + display registration (stub panel for now)
  ui/ui.c/.h      LVGL screens
  idf_component.yml  managed deps: lvgl/lvgl 9.6, esp_lvgl_port 2.x
sdkconfig.defaults   FreeRTOS + LVGL Kconfig defaults
```

## Build

```bash
. $IDF_PATH/export.sh
idf.py set-target esp32s3   # or your chip
idf.py build flash monitor
```

## Next steps

- Wire a real panel in `display.c`: create it with `esp_lcd_new_panel_*` and
  register it with `lvgl_port_add_disp()` in place of the stub display.
- Add touch input with `lvgl_port_add_touch()`.
