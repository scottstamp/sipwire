# Architecture

```
main/                app: phone state machine, UI glue
components/
  common/            sipwire_events.h: esp_event bases, IDs, payloads
  board/             Kconfig pins, board_init(), bus bring-up (SPI, I2S, SDMMC)
  display/           ILI9341 + XPT2046 via esp_lcd + esp_lvgl_port
  keypad/            matrix scanner → key events (press/release/long/repeat)
  audio/             I2S in/out tasks, ring buffers, gain, DC block, tone gen
  codec/             G.711 µ/A-law (G.722 later). Pure C, host-testable
  rtp/               RTP tx/rx, RFC 3550, RFC 2833 DTMF, adaptive jitter buffer
  sip/               minimal SIP UA: parser, digest auth, transactions, dialogs,
                     REGISTER / INVITE / ACK / BYE / CANCEL / OPTIONS, SDP
  net/               wifi manager, reconnect, SNTP, mDNS (optional)
  storage/           NVS config (settings), SD card: contacts, call log, ringtones
  phone/             call state machine, ties sip+rtp+audio; emits UI events
  ui/                LVGL screens: home, dialer, incoming, in-call, settings, contacts
                     (pure LVGL, no IDF deps — also built for the web simulator)
sim/web/             Emscripten build of components/ui + mock model → browser
```

## Host tests
`test_host/<name>/CMakeLists.txt` adds an executable + `add_test()`; the top
`test_host/CMakeLists.txt` picks up every subdir. Plain CMake + Unity from
`$IDF_PATH`, ASan/UBSan, `-Werror`. Run `tools/ci.sh host-test`.

## Rules
- Modules talk via `esp_event` loop (`SIPWIRE_EVENTS` base) + small C APIs.
  No module includes another's internals; UI never calls sip/rtp directly.
- `codec`, `rtp`, `sip` must build on IDF `linux` target → host unit tests.
- All hardware under `board` Kconfig; drivers take pins from `board_cfg_t`.
- LVGL only touched inside `lvgl_port_lock()`.
- `ui` sees the phone only through `ui_model_t` (snapshot: reg state, call
  state, number, timer, wifi, battery, volume) and `ui_actions_t` (dial,
  answer, hangup, dtmf, settings save...). On device `phone` fills the
  model and handles actions; in `sim/web` an HTML panel does. UI code must
  compile for both without `#ifdef`.
- Tasks (core pin): audio_tx/rx (core1, prio high), rtp_rx (core1), sip (core0),
  keypad (core0, low), lvgl (core0, esp_lvgl_port default).

## SIP stack choice
Write our own minimal UA in C (UDP first, TCP later). Rationale:
- PJSIP/baresip too large / awkward on IDF.
- esp-adf `esp_sip` is a closed binary blob — avoid.
- A phone UA needs a small subset: REGISTER (digest), INVITE/200/ACK, BYE,
  CANCEL, re-INVITE answer, OPTIONS ping, SDP offer/answer with PCMU/PCMA
  + telephone-event. ~3–4 k lines, fully testable against Asterisk.

## Media path
mic → I2S RX → 16-bit → (AEC later) → G.711 enc → RTP tx (20 ms frames)
RTP rx → jitter buf → G.711 dec → mixer (+ tones) → I2S TX → amp

## Config (NVS)
wifi ssid/pass, sip server/port/transport, user, auth user, password,
display name, codec prefs, ring volume, call volume, touch calibration.
Set via UI settings, serial console (`sipwire>` REPL), or `config.json` on SD.

## UI simulator
`sim/web`: Emscripten + SDL2 backend of LVGL 9 renders `components/ui` to a
canvas. Side panel injects model changes and keypad presses. Rebuild ≈ 5 s,
served locally; no toolchain/flash loop for UI work. Fonts/assets shared
with firmware build (same `lv_conf`-equivalent Kconfig values mirrored in
`sim/web/lv_conf.h`, keep in sync).
