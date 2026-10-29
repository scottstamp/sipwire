# Architecture

```
main/                app: phone state machine, UI glue
components/
  common/            sipwire_events.h: esp_event bases, IDs, payloads
  board/             Kconfig pins, board_init(), bus bring-up (SPI, I2S, SDMMC)
  display/           ILI9341 + XPT2046 via esp_lcd + esp_lvgl_port
  keypad/            matrix scanner → key events (press/release/long/repeat)
  audio/             I2S in/out tasks (16 kHz), gain, DC block, tone gen, echo canceller glue
  aec/               echo canceller: vendored SpeexDSP (MDF + residual suppressor), reference
                     ring. Pure C, host-testable
  codec/             G.722 (64k), G.711 µ/A-law, 2:1 half-band resampler. Pure C, host-testable
  rtp/               RTP tx/rx, RFC 3550, RFC 2833 DTMF, adaptive jitter buffer
  sip/               minimal SIP UA: parser, digest auth, transactions, dialogs,
                     REGISTER / INVITE / ACK / BYE / CANCEL / OPTIONS, SDP,
                     SUBSCRIBE/NOTIFY for BLF (dialog) and MWI (message-summary)
  cli/               serial REPL (esp_console): cfg/wifi/ip/free/tasks; console_register_command()
  net/               wifi manager, reconnect, SNTP, mDNS (optional)
  storage/           NVS config (settings)
  directory/         contacts (200), speed dial (keys 2-9), call history (100) on the `storage`
                     FAT partition (wear-levelled, /data; contacts.csv, speed.csv, history.bin,
                     atomic tmp+rename writes); `dir_*` API, thread-safe; console: contact,
                     speed, history. dir_core.c is pure C (host-tested in test_host/directory).
                     phone.c logs every ended call (dir_history_add; phone_log_* in
                     phone_logic.c: answered / missed / declined / DND, outgoing no-answer =
                     missed); the UI reads the lists through ui_actions_t (bridge) and keeps
                     only the unseen-missed count in the model.
  ota/               firmware update (esp_https_ota, http or https), rollback health watch
  phone/             call state machine, ties sip+rtp+audio; emits UI events
  ui/                LVGL screens: home, dialer (contact matches, speed dial), incoming, in-call,
                     history, contacts, settings (Sounds, Call features...). Long lists (contacts,
                     history) use ui_vlist.c: a pool of ~8 row widgets re-bound on scroll.
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
- Call features (`phone.h`, UI below): two call lines. `phone_status_t` top-level call
  fields = the foreground call (the only one with media), `line` (1/2, stays with a call
  across swaps), `second` {call, outgoing, number, name, call_start_us, line} = the
  background call: waiting (RINGING_IN, call-waiting beep `AUDIO_TONE_CALL_WAITING` in the
  foreground call) or held. When the foreground call ends the background one moves up at
  once (held stays held, waiting rings). API: `phone_answer_waiting` (hold current, answer),
  `phone_reject_waiting` (486), `phone_swap`, `phone_transfer_blind(target)`,
  `phone_transfer_attended_start(target)` / `_complete()` / `_cancel()`, `phone_set_dnd`.
  Events on `SIPWIRE_CALL_EVENT`: `CALL_EVENT_SECOND` (call_event_state_t of the background
  line; CALL_ENDED + end_code when it goes, CALL_IDLE when promoted), `CALL_EVENT_TRANSFER`
  {state XFER_CONSULT/TRYING/ACCEPTED/PROGRESS/DONE/FAILED, attended, code, target} (also in
  `phone_status_t.xfer`, reset at idle), `CALL_EVENT_MISSED` {number, name, code, dnd} for
  calls refused without ringing (DND, busy). Blind transfer: REFER; DONE (2xx sipfrag) → we
  hang up; FAILED → call kept (resumed if held). Attended: start holds + dials the target
  (consultation), complete = REFER with Replaces on the original, both hung up on DONE;
  cancel hangs up the consultation call. Settings `call_waiting` (default on), `dnd`
  (default off; every INVITE → 486, no ring; console `dnd on|off`), `auto_answer` (default
  on: Call-Info `answer-after=N` / Alert-Info auto answer, e.g. Asterisk Page() with
  PJSIP_HEADER, answered on the loudspeaker without ringing when no other call exists;
  during a call it waits like any call; DND wins, a page is refused). DTMF falls back to SIP
  INFO (application/dtmf-relay) when the peer has no telephone-event. Hold/resume re-INVITEs
  are queued per call and retried after 491 (RFC 3261 §14.1 delay). Console: `answer2`,
  `reject2`, `swap`, `xfer <n>`, `axfer <n>|complete|cancel`, `dnd [on|off]`; `sip` shows
  the second call, transfer and features.
- Call feature UI (`ui_model_t` "call features" block, `ui_actions_t` answer_waiting / reject_waiting /
  swap / transfer_blind / transfer_attended_start|complete|cancel / set_dnd; the bridge maps
  `phone_status_t.second/xfer/auto_answered` and the `dnd` setting). Call screen: Transfer button opens
  `scr_transfer.c` (BLF targets with live lamps, "Other number..." entry, then "Transfer now" blind or
  "Talk first" attended); a waiting call raises an overlay (Hold + answer / Reject); a held background
  call shows a strip with Swap (Hold / Transfer are not offered then); an attended consultation (xfer
  CONSULT) stays on the call screen with Complete / Cancel and the original on hold; transfer progress or
  failure is the status line under the timer, success ends on the ended screen ("Transferred"); an
  auto-answered call shows "Page / Intercom - speaker on|off". DND: bell (off) / red "DND" badge (on) in
  the status bar, tap toggles (written to settings by the bridge task, the phone follows). The router
  keeps the transfer picker up over the plain call and pulls back to the call screen for a waiting call
  or a consultation call. Physical keys on the call screen are DTMF only (long `*` = hang up, or cancel
  during a consultation).
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

### Echo canceller (`components/aec` + `audio_aec.c`, setting `aec`, console `aec`)
SpeexDSP 1.2.1 (BSD-3, vendored unmodified in `components/aec/speexdsp`, see its
README): the MDF echo canceller (partitioned frequency-domain adaptive filter,
two-path for double-talk robustness) followed by the SpeexDSP preprocessor as
residual echo suppressor (denoise gain stage on with noise suppression 0 dB, AGC,
VAD and dereverb off). Single-precision float throughout (`-fsingle-precision-constant`
+ `<tgmath.h>`, no double helpers linked). `aec` is pure C with its own error codes,
host-tested (`test_host/aec`: synthetic echo with a speaker nonlinearity and ±40
samples of misalignment, double talk, far-end silence, determinism, allocation).

Frames: the canceller runs one SpeexDSP frame per 20 ms audio frame (320 samples,
FFT size 640), no block adaptation. The linear filter adds no latency; the
suppressor's overlap-add adds one frame (20 ms) while it is on (esp-sr added 28 ms).
Tail: Kconfig `SIPWIRE_AEC_TAIL` (default 1280 samples = 4 partitions, 80 ms).

Reference: speaker and mic share one I2S clock, so there is no drift: `tx_write`
stores every sample it hands to I2S (after volume, tones mixed, soft clip, i.e.
exactly what plays) in a 4096-sample ring (`aec_ring_*`) indexed by its running TX
sample count. The nominal speaker->mic delay D = `I2S_DMA_DESC_NUM ×
I2S_CHUNK_SAMPLES` = 640 (40 ms): the TX queue starts empty, so the first buffer
written plays after the whole DMA ring, while RX index 0 is captured as both
channels start. For mic frame [n, n+320) the RX task reads ring indices
[n−D+L, n+320−D+L) with a lead L = `AEC_REF_LEAD` = 160 (10 ms, one DMA buffer):
the filter cannot model an echo that arrives before its reference, so the echo
may come up to 10 ms earlier than D and still be cancelled; converter, I2S FIFO
and acoustic delay (≤ ~2 ms) put it slightly after D, i.e. ~160 taps into the
filter, leaving ~70 ms of tail. An I2S queue overflow (task > 30 ms late, driver
drops a buffer) moves D by 160 (TX +, RX −); `aec delay <samples>` overrides D for
tuning. Off (or no mic consumer) = one branch, no copy.

Suppression: settings `aec_suppress` (dB of residual echo attenuation while only
the far end talks, default 40) and `aec_suppress_act` (while both talk, default 15),
SpeexDSP's ECHO_SUPPRESS / ECHO_SUPPRESS_ACTIVE defaults, applied live; both 0 =
linear filter only, no suppressor latency. `aec suppress <echo> <active> [<noise>]`
changes them until the next settings change (noise: stationary noise suppression,
default 0 = off).

Memory: allocated on first enable and kept; everything through `aec_set_allocator`
(audio: internal RAM while more than 64 KB internal would stay free, PSRAM beyond).
At the default tail ~124 KB (93 KB fixed: MDF scratch + FFT tables + preprocessor;
+7.7 KB per 320 samples of tail) + 8 KB ring; `aec` shows the internal/PSRAM split.
SpeexDSP does not check its allocations, so `create` first checks that
`aec_mem_estimate()` fits. Toggling on keeps what the filter learned; the stats
restart.

CPU (not yet measured on the device): QEMU with `-icount shift=0` (1 guest instruction
= 1 ns, so `aec` avg µs = thousands of instructions) counts ~1.02 M Xtensa
instructions per frame at the default settings: ~0.61 M for the linear filter (9 real
FFTs of 640, the partitioned filter, the adaptation) and ~0.41 M for the suppressor
(3 more FFTs and per-bin gain math with FPU-assisted divide/sqrt). The cost does
not depend on the signal (host benchmark: silent and active input are the same) and
barely on the tail (~2.5 % per 320 samples). At 240 MHz with an assumed 1.2-1.4
cycles per instruction that is ~5-6 ms per 20 ms frame (25-30 % of core 1), linear
only (`aec_suppress` = `aec_suppress_act` = 0) ~3-3.6 ms; frames never bunch, so the
worst frame is close to the average (icount max ~1.3-1.7 M instructions, ~7-9 ms).
QEMU runs the same code on a silent mic.
Codec preference (settings `codec_pref`): G.722 > PCMU > PCMA. On the wire all
three are 64 kbit/s with an 8000 Hz RTP clock (RFC 3551, G.722 included).

## Config (NVS)
wifi ssid/pass, sip server/port/transport, user, auth user, password,
display name, codec prefs, ring volume, call volume, touch calibration,
`ntp_server` (default CONFIG_SIPWIRE_NTP_SERVER), `tz` (POSIX TZ, default
CONFIG_SIPWIRE_TZ), `clock_24h` (default false = 12 h), `aec` (echo canceller,
default false until verified on hardware, applied live), `aec_suppress` /
`aec_suppress_act` (dB, default 40 / 15), `ota_url` (default
CONFIG_SIPWIRE_OTA_URL), `blf` (BLF keys, default none), `vm_number` (default `*97`),
`call_waiting` (default true), `dnd` (default false), `auto_answer` (default true).
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
