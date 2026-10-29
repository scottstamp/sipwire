# Architecture

```
main/                app: phone state machine, UI glue
components/
  common/            sipwire_events.h: esp_event bases, IDs, payloads
  board/             Kconfig pins, board_init(), bus bring-up (SPI, I2S, SDMMC)
  display/           ILI9341 + XPT2046 via esp_lcd + esp_lvgl_port
  keypad/            matrix scanner → key events (press/release/long/repeat)
  audio/             I2S in/out tasks (16 kHz), gain, DC block, tone gen, echo canceller (esp-sr AEC)
  codec/             G.722 (64k), G.711 µ/A-law, 2:1 half-band resampler. Pure C, host-testable
  rtp/               RTP tx/rx, RFC 3550, RFC 2833 DTMF, adaptive jitter buffer
  sip/               minimal SIP UA: parser, digest auth, transactions, dialogs,
                     REGISTER / INVITE / ACK / BYE / CANCEL / OPTIONS, SDP,
                     SUBSCRIBE/NOTIFY for BLF (dialog) and MWI (message-summary)
  cli/               serial REPL (esp_console): cfg/wifi/ip/free/tasks; console_register_command()
  net/               wifi manager, reconnect, SNTP, mDNS (optional)
  storage/           NVS config (settings), SD card: contacts, call log, ringtones
  ota/               firmware update (esp_https_ota, http or https), rollback health watch
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
- The model carries the non-secret settings that prefill the Settings forms
  (passwords only as "is set"; an empty password on save keeps the stored one),
  plus the time settings (`ntp_server`, `tz`, `clock_24h`), the firmware
  (`fw_version`, `fw_partition`, `ota_url`) and the update progress
  (`ota_state`, `ota_percent`, `ota_message`). `time_hhmm` is preformatted by
  the bridge (`ui_bridge_format_time`): "3:07 PM" or "15:07"; the home screen
  splits the AM/PM off at the space and draws it smaller.
- Settings actions: `save_time(ntp, tz_posix, clock_24h)` and `ota_start(url)`.
  The bridge task persists (NVS) and calls `ota_start()`; the UI only sees
  the model change.
- BLF / MWI: setting `blf` = "ext[:label],..." (≤ 8, `components/common/sipwire_blf.h`,
  validated by `settings_set`) and `vm_number` (default `*97`). The phone subscribes
  each extension (`dialog` event package) and our mailbox (`message-summary`) once
  registered, follows a changed list live (unsubscribe removed, subscribe new) and
  posts `SIPWIRE_BLF_EVENT`/`BLF_EVENT_STATE` {index, ext, state} and
  `SIPWIRE_SIP_EVENT`/`SIP_EVENT_MWI` {known, waiting, new, old}; `phone_status_t`
  carries the list + states and MWI. Console: `blf`, `mwi`. The model has `blf[8]`
  (ext, label, state) + `blf_count`, `mwi_waiting/new/old`, `vm_number`; actions
  `save_blf(list)` and `save_vm_number(number)` (Settings > BLF keys). Home screen:
  tiles (tap = `dial(ext)`), a voicemail button that dials `vm_number`; status bar:
  envelope with the count. Status LED: amber/blue double pulse while idle with
  voicemail waiting.
- Touch calibration: `ui_actions_t.touch_cal_begin/end` + `ui_touch_raw()`. The
  bridge installs `display_touch_raw_hook()` (raw samples to the UI, none to LVGL)
  between begin and end and stores the result in `settings.touch_cal`.
- Tasks (core pin): audio_tx/rx (core1, prio high), rtp_rx (core1), sip (core0),
  keypad (core0, low), lvgl (core0, esp_lvgl_port default).

## SIP stack choice
Write our own minimal UA in C (UDP first, TCP later). Rationale:
- PJSIP/baresip too large / awkward on IDF.
- esp-adf `esp_sip` is a closed binary blob — avoid.
- A phone UA needs a small subset: REGISTER (digest), INVITE/200/ACK, BYE,
  CANCEL, re-INVITE answer, OPTIONS ping, SDP offer/answer with G.722/PCMU/
  PCMA + telephone-event. ~3–4 k lines, fully testable against Asterisk.

## Media path
Everything between the I2S driver and `rtp` runs at 16 kHz, 20 ms frames
(320 samples); `rtp` adapts to the negotiated codec.
mic → I2S RX (16 kHz) → DC block → AEC → mute → rtp: G.722 enc
                                    ↑            or ↓2 → G.711 enc → RTP tx
                          reference ring (by sample index)
                                    ↑
RTP rx → jitter buf → G.722 dec, or G.711 dec → ↑2 → mixer (+ tones, 16 kHz)
       → gain/soft clip → I2S TX → amp

### Echo canceller (`audio_aec.c`, setting `aec`, console `aec`)
esp-sr's AEC (`esp_aec.h` only: no AFE, models or model partition), mode from
Kconfig `SIPWIRE_AEC_MODE` (default FD low cost: linear filter + residual
suppression, 512-sample / 32 ms blocks). Speaker and mic share one I2S clock, so
there is no drift: `tx_write` stores every sample it hands to I2S (after volume,
tones mixed, soft clip, i.e. exactly what plays) in a 4096-sample ring indexed by
its running TX sample count; for mic frame [n, n+320) the RX task reads ring
indices [n−D, n+320−D). D = `I2S_DMA_DESC_NUM × I2S_CHUNK_SAMPLES` = 640 (40 ms):
the TX queue starts empty, so the first buffer written plays after the whole DMA
ring, while RX index 0 is captured as both channels start. Converter, I2S FIFO and
acoustic delay (≤ ~2 ms) put the echo slightly *after* n−D, inside the adaptive
filter. An I2S queue overflow (task > 30 ms late, driver drops a buffer) moves D
by 160 (TX +, RX −); `aec delay <samples>` overrides it for tuning. 20 ms frames
are adapted to esp-sr blocks by `dsp_blockfifo` (output pre-filled with
block − gcd(320, block) zeros: +448 samples = 28 ms on the mic path for 512, +12 ms
for the 256-sample VoIP modes). Off (or no mic consumer) = one branch, no copy.
The instance is allocated on first enable and kept (~19 KB internal incl. our block
buffers, ~117 KB PSRAM incl. the ring; esp-sr insists on PSRAM). Toggling on
re-primes the FIFO, the filter keeps what it learned. QEMU: `aec_process` is
replaced by a copy (Espressif's QEMU segfaults in it); the rest runs.
Codec preference (settings `codec_pref`): G.722 > PCMU > PCMA. On the wire all
three are 64 kbit/s with an 8000 Hz RTP clock (RFC 3551, G.722 included).

## Config (NVS)
wifi ssid/pass, sip server/port/transport, user, auth user, password,
display name, codec prefs, ring volume, call volume, touch calibration,
`ntp_server` (default CONFIG_SIPWIRE_NTP_SERVER), `tz` (POSIX TZ, default
CONFIG_SIPWIRE_TZ), `clock_24h` (default false = 12 h), `aec` (echo canceller,
default true, applied live), `ota_url` (default
CONFIG_SIPWIRE_OTA_URL), `blf` (BLF keys, default none), `vm_number` (default `*97`).
Set via UI settings, serial console (`sipwire>` REPL), or `config.json` on SD.

## OTA update (`components/ota`)
`ota_start(url)` (console `ota [url]`, Settings > Update) downloads with
`esp_https_ota` into the inactive slot (`partitions.csv`: ota_0 / ota_1, 3 MB
each) in its own task. Plain http is allowed (`CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP`,
LAN update server; https uses the mbedtls cert bundle). Refused while a call is
not idle or another update runs. After the first block and before anything
else is written: the image header must be for esp32s3 (esp_https_ota rejects
other chips) and the app descriptor's `project_name` must be `sipwire`. Events
on `SIPWIRE_OTA_EVENT`: STARTED, PROGRESS {percent, bytes}, DONE, FAILED
{esp_err, reason}; DONE reboots after 3 s.

Rollback: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`. A freshly installed image boots
PENDING_VERIFY. It is marked valid (`esp_ota_mark_app_valid_cancel_rollback`)
as soon as the UI has started AND the network has an IP, or 60 s after boot,
whichever comes first. A crash/reset before that makes the bootloader boot the
previous slot again. `ota status` shows slot, version and app state.

Version: `esp_app_get_description()->version` is `git describe --always --tags
--dirty` at configure time (root `CMakeLists.txt`; override with
`-DPROJECT_VER=...` or `version.txt`; "0.0-nogit" without a repository).

## UI simulator
`sim/web`: Emscripten + SDL2 backend of LVGL 9 renders `components/ui` to a
canvas. Side panel injects model changes and keypad presses. Rebuild ≈ 5 s,
served locally; no toolchain/flash loop for UI work. Fonts/assets shared
with firmware build (same `lv_conf`-equivalent Kconfig values mirrored in
`sim/web/lv_conf.h`, keep in sync).
