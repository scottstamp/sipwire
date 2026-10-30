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
components/audio/    I2S speaker + mic, tones, echo canceller glue (setting `aec`, console `aec`)
components/aec/      echo canceller, pure C (host-tested): vendored SpeexDSP 1.2.1 echo
  speexdsp/          canceller + residual echo suppressor (BSD-3-Clause, speexdsp/COPYING;
                     version, commit and local changes in speexdsp/README.md). No managed
                     component, no Espressif audio libraries (esp-sr / esp-adf / esp-dsp).
components/ota/      firmware update over HTTP(S) (`ota <url>`, Settings > Update), rollback safety;
                     uses ESP-IDF's own esp_https_ota (no managed component added)
components/sip/      own SIP UA incl. BLF (dialog) / MWI (message-summary) subscriptions;
                     setting `blf` (e.g. `cfg set blf "1002:Bob,1003"`), `vm_number`
                     (no managed component added)
components/phone/    call features: settings `find_alert` (find the handset, default "find"),
                     `quiet_hours` + `quiet_start` / `quiet_end` (HH:MM) + `quiet_allow_contacts`,
                     `fwd_always` / `fwd_busy` / `fwd_noanswer` + `fwd_noanswer_s` (302
                     forwarding), console `redial` (also: Call on an empty dialer)
sdkconfig.defaults   FreeRTOS + LVGL Kconfig defaults
```

## Build

```bash
. $IDF_PATH/export.sh
idf.py set-target esp32s3   # or your chip
idf.py build flash monitor
```

Flash is Quad I/O (`CONFIG_ESPTOOLPY_FLASHMODE_QIO`, 80 MHz). The bootloader sets that
mode, so flash every unit over serial once with the full `idf.py -p PORT flash`
(bootloader + partition table + otadata + app; settings and contacts are kept). OTA does
not update the bootloader and refuses a QIO image on a unit still running a DIO build
(`SPI flash mode mismatch`). An older local `sdkconfig` keeps DIO: delete its `FLASHMODE`
lines and run `idf.py reconfigure`. Details: `docs/ARCHITECTURE.md` "Flash mode".

## Next steps

- Verify display orientation, colour order and touch axes on the real board
  (see `components/display/include/display.h`).
- Run Settings -> Touch calibration on the real board and check the result
  (`display_touch_raw_hook()`, `components/ui/scr_calibrate.c`).
- BLF / MWI on the real PBX: mirror the hints, `mailboxes=` and voicemail setup
  of `tools/asterisk/conf/` (see `tools/asterisk/README.md`).
