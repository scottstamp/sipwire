# sipwire

ESP-IDF v6.1 project with an LVGL 9 UI (via `espressif/esp_lvgl_port`).

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
