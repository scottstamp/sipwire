# sipwire

ESP-IDF v6.1 + LVGL 9.6 SIP handset firmware for ESP32-S3. Read
`docs/ARCHITECTURE.md` before touching code; `docs/PLAN.md` for task order;
`docs/HARDWARE.md` for the hardware class.

## Conventions
- C17, 4-space indent, `snake_case`, module prefix on public symbols
  (`sip_`, `rtp_`, `audio_`, `keypad_`...). One public header per component.
- Pins/options only via `components/board` Kconfig (`CONFIG_SIPWIRE_*`).
- Log tags = component name. No `printf`; use `ESP_LOGx`.
- Return `esp_err_t`; use `ESP_RETURN_ON_ERROR` / `ESP_GOTO_ON_ERROR`.
- LVGL calls only under `lvgl_port_lock()`.
- `codec`, `rtp`, `sip` have no IDF hardware deps (host-buildable).
- Do not add managed components without noting them in README.

## Build
```
idf.py set-target esp32s3 && idf.py build
```
Host tests: `tools/ci.sh host-test` (once Phase 0 lands).
