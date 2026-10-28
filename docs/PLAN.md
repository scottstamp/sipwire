# Plan

Status: `[x]` = merged (build-clean / host-tested; hardware verification pending).

Cost tags: **[cheap]** = sonnet/haiku-class subagent, well-specified, mechanical.
**[core]** = opus-class, design-heavy or subtle. Phases run in order; items
inside a phase are parallel unless noted.

## Phase 0 — Foundation (before final hardware)
- [x] [core] `components/board`: Kconfig menu for every pin/option; `board_cfg_t`;
  `board_init()`. Default pins = a plausible dev layout, all overridable.
- [x] [cheap] `components/net`: wifi station manager, reconnect/backoff, events.
- [x] [cheap] `components/storage`: NVS settings struct + load/save + defaults.
- [x] [cheap] Serial console REPL (`esp_console`): `wifi`, `sip set`, `call`,
  `hangup`, `cfg show`. Primary dev/test interface until UI matures.
- [x] [cheap] `esp_event` base + event list header (`sipwire_events.h`).
- [x] [cheap] Host test scaffolding: `test_host/` building `codec`/`sip`/`rtp`
  for IDF linux target + Unity. CI script `tools/ci.sh`.
- [x] [cheap] Asterisk dev stack: `tools/asterisk/` docker-compose + minimal
  `pjsip.conf` (2 extensions, echo test ext, music-on-hold ext).
- [x] [core] UI simulator contract: `components/ui` depends only on LVGL +
  `ui_model.h` (phone state snapshot) + `ui_actions.h` (callbacks). No IDF
  headers. Core writes these two headers; everything else follows them.
- [x] [cheap] Web UI simulator (`sim/web/`): LVGL 9 → WebAssembly via Emscripten
  (official `lv_web_emscripten` pattern, SDL2 backend, 240x320 canvas).
  Same `components/ui` sources, mock `ui_model` driven from an HTML side
  panel: fake keypad (3x4 buttons), reg state, incoming call, in-call timer,
  wifi RSSI, battery. `sim/web/build.sh` (emcmake) + `serve.sh` (python
  http.server, rebuild-on-save). ~5 s rebuild, no flash. Touch = mouse.
  Also target for screenshot-based UI review by agents (headless chromium).

## Phase 1 — Drivers (when hardware arrives)
- [x] [cheap] Display: ILI9341 via `esp_lcd_ili9341` + `lvgl_port_add_disp`.
  Backlight PWM. Replace stub in `display.c`.
- [x] [cheap] Touch: XPT2046 via `esp_lcd_touch_xpt2046` (raw coords, swap/mirror/cal in
  `display.c`). Calibration screen (Settings) + `display_touch_raw_hook()` done; separate
  touch swap/mirror Kconfig if a board's touch axes differ from the panel's.
- [x] [cheap] Keypad: matrix scan task, debounce, long-press, repeat; key events.
- [x] [cheap] Audio I2S: TX+RX tasks, ring buffers, loopback test (mic→speaker),
  tone generator (dial/ring/busy). Verify levels, DC offset.
- [cheap] SD card: mount if present, `config.json` import, call log append.
- [core] Review: task priorities, DMA buffer sizing, PSRAM placement.

## Phase 2 — SIP core (host-testable, no hardware needed)
- [x] [core] SIP message parser/serializer (headers, compact forms, Via branch,
  Contact, CSeq, Route) — strict, fuzz-friendly.
- [x] [core] Digest auth (MD5, qop=auth), nonce reuse, 401/407 handling.
- [x] [core] Transaction layer (RFC 3261 timers A/B/E/F/K, retransmits) + dialog
  state (early/confirmed), UDP transport, NAT keepalive, rport.
- [x] [core] UA: REGISTER + refresh, outgoing INVITE (100/180/183/200/ACK),
  incoming INVITE (180 → 200 → ACK), BYE both ways, CANCEL, 486/603 reject,
  OPTIONS reply, re-INVITE (hold) answer.
- [x] [cheap] SDP build/parse: PCMU/PCMA/telephone-event, ptime, c= line.
- [x] [cheap] Unit tests for parser, auth, SDP, timers (Unity on host).
- [x] [cheap] Integration test: `test_host/sip/integration.sh` (sipcli + local
  Asterisk 20: register, echo call, hold, remote BYE, inbound, unregister).

## Phase 3 — Media
- [x] [cheap] G.711 µ-law/A-law codec + tests.
- [x] [core] RTP tx/rx, SSRC, seq/timestamp, RFC 2833 DTMF send; adaptive
  jitter buffer (40–120 ms) with PLC (simple repeat/fade).
- [x] [core] Audio pipeline glue: 20 ms framing, mixer, clock drift handling.
- [cheap] RTCP minimal (SR/RR) — optional.

## Phase 4 — Phone application
- [x] [core] `components/phone`: call state machine (idle, dialing, ringing-out,
  ringing-in, active, held, ending), maps SIP/RTP/keypad events ↔ audio/UI.
- [x] [cheap] UI screens (LVGL 9), built & reviewed in the web simulator first,
  then verified on device: home/status bar (wifi, reg state, battery),
  dialer with number entry from keypad+touch, incoming call, in-call
  (timer, mute, speaker, DTMF), settings (wifi, SIP account), contacts,
  call log. Use `lv_screen_load_anim`, one file per screen.
- [cheap] Ringtones (tone sequences; later WAV from SD), ring/talk volume.
- [cheap] Contacts + call log on SD (CSV/JSON), speed dial from keypad.

## Phase 5 — Hardening / nice-to-have
- [core] AEC/noise suppression for speakerphone (esp-sr AEC if license OK,
  else simple NLMS). Half-duplex suppressor as fallback.
- [cheap] G.722 (wideband) via libg722.
- [cheap] OTA update from SD / HTTP; version in UI.
- [cheap] Power: light sleep when idle, backlight timeout, battery ADC.
- [core] SIP over TCP/TLS, SRTP (mbedtls) — optional.
- [cheap] Docs, CLAUDE.md upkeep, CI.

## Milestones
1. **M1** (done in QEMU with stubbed I2S/openeth; pending real hardware) Phase 0 + 2 + G.711: register with Asterisk, call echo test ext
   from console, audio over I2S headphone-grade loopback (no UI needed).
2. **M2** Phase 1 + 4 basic: dial on keypad, see call on screen, answer
   incoming with touch/key.
3. **M3** Polish: contacts, call log, settings UI, ringtones, AEC.

## Subagent brief template
Each [cheap] task gets: files to touch, public API header (written by core),
Kconfig names, test to pass, "do not change" list. Core agent reviews diff.

## CI
`.github/workflows/ci.yml` (manual trigger only): host unit tests, firmware build, SIP-vs-Asterisk
integration, and the QEMU smoke test (`tools/qemu/README.md`).

## Next (after hardware spec)
- Set pins/flash size in `sdkconfig.defaults` for the real board; verify panel
  orientation/colors, touch axes, mic shift/slot, amp pops (see component notes).
- Verify the touch calibration screen on hardware (done in sim + QEMU build only).
- AEC / speakerphone (Phase 5), OTA (partitions already dual-slot).
