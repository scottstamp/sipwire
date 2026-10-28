# Plan

Cost tags: **[cheap]** = sonnet/haiku-class subagent, well-specified, mechanical.
**[core]** = opus-class, design-heavy or subtle. Phases run in order; items
inside a phase are parallel unless noted.

## Phase 0 — Foundation (before final hardware)
- [core] `components/board`: Kconfig menu for every pin/option; `board_cfg_t`;
  `board_init()`. Default pins = a plausible dev layout, all overridable.
- [cheap] `components/net`: wifi station manager, reconnect/backoff, events.
- [cheap] `components/storage`: NVS settings struct + load/save + defaults.
- [cheap] Serial console REPL (`esp_console`): `wifi`, `sip set`, `call`,
  `hangup`, `cfg show`. Primary dev/test interface until UI matures.
- [cheap] `esp_event` base + event list header (`sipwire_events.h`).
- [cheap] Host test scaffolding: `test_host/` building `codec`/`sip`/`rtp`
  for IDF linux target + Unity. CI script `tools/ci.sh`.
- [cheap] Asterisk dev stack: `tools/asterisk/` docker-compose + minimal
  `pjsip.conf` (2 extensions, echo test ext, music-on-hold ext).

## Phase 1 — Drivers (when hardware arrives)
- [cheap] Display: ILI9341 via `esp_lcd_ili9341` + `lvgl_port_add_disp`.
  Backlight PWM. Replace stub in `display.c`.
- [cheap] Touch: XPT2046 via `esp_lcd_touch_xpt2046`, calibration screen,
  store in NVS.
- [cheap] Keypad: matrix scan task, debounce, long-press, repeat; key events.
- [cheap] Audio I2S: TX+RX tasks, ring buffers, loopback test (mic→speaker),
  tone generator (dial/ring/busy). Verify levels, DC offset.
- [cheap] SD card: mount if present, `config.json` import, call log append.
- [core] Review: task priorities, DMA buffer sizing, PSRAM placement.

## Phase 2 — SIP core (host-testable, no hardware needed)
- [core] SIP message parser/serializer (headers, compact forms, Via branch,
  Contact, CSeq, Route) — strict, fuzz-friendly.
- [core] Digest auth (MD5, qop=auth), nonce reuse, 401/407 handling.
- [core] Transaction layer (RFC 3261 timers A/B/E/F/K, retransmits) + dialog
  state (early/confirmed), UDP transport, NAT keepalive, rport.
- [core] UA: REGISTER + refresh, outgoing INVITE (100/180/183/200/ACK),
  incoming INVITE (180 → 200 → ACK), BYE both ways, CANCEL, 486/603 reject,
  OPTIONS reply, re-INVITE (hold) answer.
- [cheap] SDP build/parse: PCMU/PCMA/telephone-event, ptime, c= line.
- [cheap] Unit tests for parser, auth, SDP, timers (Unity on host).
- [cheap] Integration test: `tools/siptest/` python (pjsua2 or sipp)
  scripts against docker Asterisk.

## Phase 3 — Media
- [cheap] G.711 µ-law/A-law codec + tests.
- [core] RTP tx/rx, SSRC, seq/timestamp, RFC 2833 DTMF send; adaptive
  jitter buffer (40–120 ms) with PLC (simple repeat/fade).
- [core] Audio pipeline glue: 20 ms framing, mixer, clock drift handling.
- [cheap] RTCP minimal (SR/RR) — optional.

## Phase 4 — Phone application
- [core] `components/phone`: call state machine (idle, dialing, ringing-out,
  ringing-in, active, held, ending), maps SIP/RTP/keypad events ↔ audio/UI.
- [cheap] UI screens (LVGL 9): home/status bar (wifi, reg state, battery),
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
1. **M1** Phase 0 + 2 + G.711: register with Asterisk, call echo test ext
   from console, audio over I2S headphone-grade loopback (no UI needed).
2. **M2** Phase 1 + 4 basic: dial on keypad, see call on screen, answer
   incoming with touch/key.
3. **M3** Polish: contacts, call log, settings UI, ringtones, AEC.

## Subagent brief template
Each [cheap] task gets: files to touch, public API header (written by core),
Kconfig names, test to pass, "do not change" list. Core agent reviews diff.
