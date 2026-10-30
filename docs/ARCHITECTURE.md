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
  power/             DFS policy (esp_pm, CPU_FREQ_MAX holds: call, display), console `power`
  cli/               serial REPL (esp_console): cfg/wifi/ip/free/tasks; console_register_command()
  net/               wifi manager, reconnect, SNTP, mDNS (optional)
  storage/           NVS config (settings)
  directory/         contacts (200), speed dial (keys 2-9), call history (100) on the `storage`
                     FAT partition (wear-levelled, /data; contacts.csv, speed.csv, history.bin,
                     atomic tmp+rename writes); `dir_*` API, thread-safe; console: contact,
                     speed, history. dir_core.c is pure C (host-tested in test_host/directory).
                     Mutators change RAM only; the "dir_save" task writes snapshots (taken
                     under the lock) ~200 ms later, so the LVGL / sip tasks never wait for
                     flash; dir_flush() waits for the write (console commands use it).
                     While a call exists (`phone.c` `update_power()` ->
                     `dir_defer_writes()`) the writes wait until the call is over: see
                     "Flash writes during calls" (Memory and tasks). dir_flush() writes anyway.
                     phone.c logs every ended call (dir_history_add; phone_log_* in
                     phone_logic.c: answered / missed / declined / DND, outgoing no-answer =
                     missed); the UI reads the lists through ui_actions_t (bridge); the model
                     carries the unseen-missed count + newest caller, history_rev (an open
                     History screen refetches when it changes) and the last
                     CALL_EVENT_MISSED (missed_seq/from/dnd: a notice over any screen).
  ota/               firmware update (esp_https_ota, http or https), rollback health watch
  phone/             call state machine, ties sip+rtp+audio; emits UI events
  ui/                LVGL screens: home, dialer (up to 3 contact matches, speed dial), incoming, in-call,
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
  keypad (core0, low), lvgl (core0, esp_lvgl_port default). Sizes and budget: "Memory and tasks".

## Memory and tasks

Internal RAM (~340 KB DIRAM, shared by IRAM code, .data/.bss, task stacks, DMA buffers and the
internal heap) is the scarce resource; PSRAM (8 MB octal) is plentiful but has rules.

**Placement rules**
- PSRAM goes through the MMU cache. While flash is written or erased (NVS, FAT `storage`,
  OTA, otadata) the cache is off: only IRAM code with internal-RAM data may run. A task whose
  *stack* is in PSRAM must therefore never do a flash operation itself
  (`SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` only permits it). An ISR that is IRAM-safe must not
  touch PSRAM. A task merely holding PSRAM *data* is fine: the cache is back before it runs.
- Must stay internal: DMA buffers (LCD draw buffers, I2S rings, SPI), anything an ISR touches,
  all task stacks (every task here can reach a flash write: LVGL saves contacts synchronously,
  the bridge/console/sip tasks write NVS), FreeRTOS objects (`pvPortMalloc` is internal).
- Hot audio state stays internal: RTP rx buffer, `rtp_session_t` (5.3 KB, jitter buffer),
  audio task frames.
- PSRAM: the LVGL heap (`components/display/lv_mem_psram.c`: objects, styles, layer buffers),
  `sip_ua_t` (89 KB; plain `malloc` above `SPIRAM_MALLOC_ALWAYSINTERNAL` = 16 KB goes to
  PSRAM), mbedTLS (`MBEDTLS_EXTERNAL_MEM_ALLOC`: https OTA record buffers 16 + 4 KB, WPA
  crypto), the contacts-search scratch (12.8 KB, `heap_caps_malloc_prefer`), the AEC
  (internal only while > 64 KB internal would stay free, i.e. PSRAM on the device).
- `SPIRAM_MALLOC_RESERVE_INTERNAL` = 32 KB stays reserved for DMA/internal-only requests.
- Allocations below 16 KB with plain `malloc` land in internal RAM: use
  `heap_caps_malloc_prefer(n, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT)` for
  larger buffers only tasks use.

**DMA**: LCD draw buffers 2 x 1/20 screen (240 x 16 px x 2 B = 7.5 KB each, 15 KB internal DMA,
double buffered so LVGL renders while SPI at 40 MHz flushes; was 2 x 1/10 = 30 KB: 15 KB of
internal heap back, a full-screen redraw now takes 20 flushes instead of 10);
`max_transfer_sz` = 240 x 40 x 2 = 19.2 KB still covers one buffer in a single transaction. I2S: 4 descriptors x 160 frames (10 ms)
per direction (TX 4 x 320 B, RX 4 x 640 B for 32-bit mic slots); the 40 ms TX ring is the AEC
delay `D` (see Echo canceller) and the main playout latency besides the jitter buffer.

**Flash writes during calls**: a wear-levelled FAT sector write erases a 4 KB flash sector
(~30-45 ms with the cache off). Core 1 then parks in IRAM and the audio tasks (flash-resident
code, PSRAM data) cannot run for longer than the 40 ms I2S TX ring lasts: a click or a gap.
So `directory` holds its writes back while a call exists (`dir_defer_writes()`, driven by
`phone.c` `update_power()` together with the `call` power hold): history entries of a
rejected or ended background call, missed-call "seen" marks and contact / speed-dial edits
change RAM at once (the UI sees them), and the `dir_save` task writes them when the last call
is gone (200 ms coalescing as usual). `dir_flush()` (console `contact` / `history` commands)
writes regardless. A reboot during a call loses those changes, as a reset within the 200 ms
coalescing window always did. NVS writes are not deferred: a volume change during a call is
saved 3 s after the last key press (`vol_save_at` -> `settings_set`); an NVS write is mostly
an entry append (no erase), but a full NVS page costs an erase too.

**Heap debugging** (`sdkconfig.debug-heap` overlay, `HEAP_POISONING_COMPREHENSIVE`, console
`heapwatch [ms [psram_s]]`): a background task runs `heap_caps_check_integrity` on the internal
heap every `ms` (default 200; a few ms each) and on PSRAM only every `psram_s` seconds (default
10, 0 = never). Each check holds that heap's spinlock for the whole walk (interrupts masked on
the checking core, the other core spins on its next malloc); the 7.5 MB PSRAM walk with
poison verification takes ~100 ms, longer than the 40 ms I2S ring, so every PSRAM check is
an expected audio click / Wi-Fi hiccup in that build. Release builds have no `heapwatch`.

**Tasks** (stack bytes; high water = least free ever. "QEMU worst" = least free over the smoke
shards incl. calls with AEC, transfers, call waiting, CSV import; QEMU has no Wi-Fi radio or
LCD, so its heap numbers are far too optimistic, stack numbers are representative)

| task | core | prio | stack | device free (idle) | QEMU worst free | note |
|---|---|---|---|---|---|---|
| audio_tx | 1 | 22 | 6144 | (call only) | 2664 | frames + rtp decode |
| audio_rx | 1 | 22 | 6144 | (call only) | 2628 | AEC (~5-6 ms per 20 ms frame) |
| rtp_rx | 1 | 21 | 4096 | 3336 | 2616 | |
| sip | 0 | 5 | 8192 -> **7168** | 4580 | 3504 (4.7 KB used) | ~2.5 KB margin |
| taskLVGL | 0 | 4 | 8192 | 2264 | 2448 | software rendering, history page on stack |
| ui_bridge | 0 | 3 | 6144 | 2172 | 2004 | |
| console_repl | any | 2 | 6144 | 4748 | 2044 | `contact import`, `cfg show`: keep |
| keypad | 0 | 2 | 3072 | 2292 | - | |
| dir_save | any | 2 | 4096 | 3408 | 2172 | |
| ota | any | 3 | 8192 | (updates only) | - | TLS handshake on https |
| sys_evt | 0 | 20 | 3584 | 2524 | 2640 | |
| tcpip | any | 18 | 3072 | 1772 | 1936 | |
| esp_timer | 0 | 22 | 3584 | 3004 | 3268 | |
| wifi | 0 | 23 | (IDF) | 4404 | - | |
| ipc0/ipc1 | 0/1 | 24 | 1280 -> **2048** | ? | 228 -> 996 (1 KB used) | IDF cross-core calls |
| Tmr Svc | any | 1 | 2048 | 1336 | 1380 | |

Audio/RTP/AEC own core 1 (prio 21-22, above everything but IPC); Wi-Fi, LVGL, SIP and the UI
glue are on core 0. tcpip (prio 18, no affinity) may use core 1 when the audio tasks sleep but
cannot preempt them.

**Budget** (device numbers from `free` / `tasks`; static sizes from `idf.py size`)

| | before | after |
|---|---|---|
| DIRAM static (.text+.data+.bss) | 159.8 KB | 160.4 KB (+0.7 KB spi_flash .data, toolchain noise) |
| internal free idle / min / largest | 53 / 40 / 31 KB | check on device |
| task stacks changed | | sip -1.5 KB, ipc0+ipc1 +1.5 KB (net 0) |
| https OTA (mbedTLS, ~20-40 KB peak) | internal | PSRAM |
| contacts screen refetch (12.8 KB transient) | internal | PSRAM |
| LCD draw buffers (internal DMA heap) | 2 x 15 KB | 2 x 7.5 KB (-15 KB heap; static DIRAM 164562 -> 164662 B, +100 B from the power changes' code) |

Options not taken (need device testing): Wi-Fi/LWIP
buffers in PSRAM (`SPIRAM_TRY_ALLOCATE_WIFI_LWIP`), Wi-Fi IRAM optimisations off
(`ESP_WIFI_IRAM_OPT`, `ESP_WIFI_RX_IRAM_OPT`: ~15-19 KB of IRAM code back to the heap),
LVGL task stack in PSRAM (unsafe: contact
saves write FAT from the LVGL task), settings snapshots in PSRAM via `EXT_RAM_BSS_ATTR`
(~9 KB of static `settings_t` copies; conflicts with `SPIRAM_IGNORE_NOTFOUND`), fewer I2S
descriptors (lower latency, changes the AEC delay).

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
The canceller state is allocated first (63 blocks, in SpeexDSP's init order), the ring last:
the greedy split gives internal RAM to the state touched all over every frame, the ring
(one sequential 640 B write and read per frame) gets internal only if everything fits. With
the ring first, any create with 72-193 KB internal free put 8 KB of ring internal and the
same 8 KB of canceller state in PSRAM. On the handset (internal free ~40-53 KB, below the
64 KB reserve) all ~132 KB is PSRAM either way.
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

## Power (`components/power`, `net`, console `power`)
Rule: nothing that saves power may run while a call exists. `phone.c` `update_power()` runs
on every foreground / background call state change and sets, for as long as any call exists
(dialing, ringing in or out, active, held, call waiting, two calls, the ended screen):
`net_set_low_latency(true)` (Wi-Fi `WIFI_PS_NONE`) and `power_hold(POWER_HOLD_CALL)` (CPU at
full speed, no light sleep). Both drop when the phone is idle again.

- **Wi-Fi modem sleep** (`net`, board Kconfig `SIPWIRE_WIFI_LISTEN_INTERVAL`, default 3,
  range 1-10). Idle, the radio sleeps between beacons and the AP buffers unicast frames for
  us meanwhile (announced in the beacon's TIM).
  - Listen interval > 1 (default): `WIFI_PS_MAX_MODEM`. The STA config carries
    `listen_interval` (set in `apply_creds` before every connect; the AP learns it from the
    association request) and the radio wakes every `listen_interval` beacons. INVITE
    latency: up to listen interval x beacon interval (3 x 102.4 ms = 0.31 s worst case, half
    on average; 10 = 1.02 s, past SIP T1 = 500 ms, so the INVITE is retransmitted once and
    both copies arrive together). After an ARP cache expiry the PBX's ARP request comes
    first: broadcasts are sent only after DTIM beacons, and a wake-up that is not on a DTIM
    beacon misses them (the PBX repeats its ARP, ~1 s later). Keep the listen interval a
    multiple of the AP's DTIM period (DTIM 1 or 3 with the default 3).
  - Listen interval 1: `WIFI_PS_MIN_MODEM` as before, wakes at every DTIM beacon (DTIM 1-3 =
    0.1-0.3 s worst case INVITE latency, no missed broadcasts).
  - Saving: with DTIM 1, MAX_MODEM at 3 receives one beacon in three; the radio's receive
    wake-ups dominate idle Wi-Fi current. To measure on the device.
  During calls (`net_set_low_latency(true)`) modem sleep is off: beacon-paced RTP delivery
  underruns the jitter buffer. Afterwards the idle mode returns. `power` prints the mode
  (`wifi ps`), incl. the listen interval.
- **Dynamic frequency scaling** (`power`, `CONFIG_PM_ENABLE`, board Kconfig
  `SIPWIRE_POWER_DFS` / `SIPWIRE_POWER_MIN_CPU_MHZ`): `esp_pm_configure` max =
  `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ` (240), min = 80, light sleep below. One
  `ESP_PM_CPU_FREQ_MAX` lock per reason: `call` (above) and `display` (backlight on or dimmed,
  incl. boot; `display.c` `bl_apply_locked`). So the CPU is at 80 MHz only when idle with the
  backlight off; a key, touch or incoming call takes it back to 240 before anything renders
  or plays. Why min 80, not 40 (XTAL): from 80 MHz up the CPU runs from the PLL and APB stays
  80 MHz, so no peripheral clock ever changes. Driver check: UART console (APB source),
  LEDC backlight (`LEDC_AUTO_CLK`, APB), SPI LCD + XPT2046 (spi_master takes its own APB lock
  per transaction), I2S (PLL_F160M source, its driver holds a PM lock while enabled; calls
  hold `call` anyway), RMT status LED (APB, own lock), keypad (GPIO), esp_timer
  (SYSTIMER, XTAL), PSRAM/flash (MSPI, PLL) and FreeRTOS run-time stats (esp_timer) all
  ignore CPU-only changes. The QEMU build has `CONFIG_PM_ENABLE` off and `power.c` skips DFS
  under `CONFIG_SIPWIRE_QEMU` (QEMU does not model the clock switching).
- **Audio hardware** (`audio.c`): the stream (tones, ringtone, call) starts on demand and
  stops when the phone has nothing to play; stopping fades out 20 ms, writes silence, drives
  the amp SD_MODE low (MAX98357A shutdown) and disables both I2S channels (BCLK stops, the
  MEMS mic sleeps without a clock). Starting primes the DMA with one silent frame before the
  amp wakes (no pop), so the ringtone starts ~20-40 ms after the INVITE is processed.
  Gap closed: a tone-only stream started outside a call (ringtone preview in Settings >
  Sounds, console `ring`) used to keep I2S + amp running after `stop` until the next call
  ended. Now a stream with no callbacks whose tone has stopped (or a one-shot ended) for 2 s
  is stopped by `idle_stop_cb` (esp_timer, under `api_mtx`, so it cannot race
  `audio_start`; a tone requested meanwhile stays queued and restarts the stream). Phone
  tones (ring, ringback, busy) loop, so no call path is affected.
  Amp pin (`SIPWIRE_SPK_EN`, GPIO 18): `amp_set(true)` exists only in `tx_task` after the
  priming frame; its exit, `stream_stop_locked` and the start failure path set it off, and
  stop waits for `tx_task` to exit. So the amp is on exactly while a stream runs: never idle,
  not after the tone-only idle stop, not after a call (`phone_media_audio(false, NONE)` ->
  `audio_stop`). Init sets the output latch off before the pin becomes an output (it used to
  go through `gpio_reset_pin`, which enables the pull-up for a moment), with the internal
  pull towards off, and takes the pin out of the light-sleep GPIO switch
  (`PM_SLP_DISABLE_GPIO` would float it in sleep). From reset until `audio_init` the pin is
  not driven: a breakout pull-up on SD_MODE needs the external pull-down (`HARDWARE.md`).
- **Idle wake-ups** (each one costs a light-sleep exit, so they come first). Screen off and
  idle, measured before this work (`cpu 30`): core 0 6 % busy, sip 2.6 %, wifi 1.2 %, keypad
  0.7 %, esp_timer 0.7 %, taskLVGL 0.4 %. Now:
  - *sip task* (`phone.c` `sip_task`): event-driven instead of a 20 ms poll. It blocks in
    `select()` on the SIP socket plus an eventfd (`esp_vfs_eventfd`, `wake_fd`) until the
    earliest deadline: `sip_ua_next_deadline_ms()` (every timer in `components/sip`:
    retransmissions and transaction timers, re-REGISTER / retry / keepalive, subscription
    refresh / retry / expiry, call cleanup, pending hold re-INVITE, REFER timeout) combined with
    the phone's own (`close_deadline`, `retry_at`, `ended_until`, `aa_at`, DTMF sequence,
    `vol_save_at`), at most 2 s (safety net). Every producer that hands the task work writes the
    eventfd: `send_cmd` (all commands), the net / settings notify bits, `phone_set_volume`.
    Host tests check the deadline on every tick of every SIP test: a tick before it must change
    nothing, a tick at it must act or move it (`test_host/sip/sip_test_util.c`). A deadline that
    stays due is throttled to 10 ms after 16 passes (logged). Idle: one wake-up per 2 s plus
    the 25 s CRLF keepalive. Without the eventfd it falls back to the 20 ms poll.
  - *keypad*: scans every 10 ms only while a key is down or settling
    (`keypad_scan_idle`); idle, all rows are parked low and the task sleeps until a
    level-low interrupt on any column (masked by the ISR, re-armed when idle again).
  - *LVGL*: esp_lvgl_port's 5 ms `lv_tick_inc` timer (200 esp_timer wake-ups/s) is replaced
    by `lv_tick_set_cb` (esp_timer clock; the port timer runs every ~17 min, unused). The
    LVGL task sleeps up to 2 s when no lv_timer is due and is woken by
    `lv_timer_handler_set_resume_cb` (any timer created / resumed, incl. the refresh timer on
    invalidation) and by the touch interrupt. LVGL 9 pauses the refresh timer when nothing is
    invalid; the home screen's BLF blink timer pauses itself unless a tile rings. Screen
    changes are instant (`ui.c` `show()`, no 120 ms slide: every slide frame re-rendered and
    flushed the whole screen); the old screen is deleted 100 ms later, which is safe because
    a load without animation never sets LVGL's `prev_scr`.
  - *touch*: the PENIRQ falling-edge interrupt of esp_lcd_touch is swapped for a level-low
    one (`display.c` `touch_isr`, masked until a read finds the panel released; while a
    finger is down but not reported, swallowed wake-up touch or calibration, it re-arms after
    30 ms). It wakes the LVGL task only on a touch. A release is reported only after
    `CONFIG_SIPWIRE_TOUCH_RELEASE_DEBOUNCE_MS` (80) without a touch, at the last point: the
    resistive panel drops out for a read while a finger drags lightly, and that early release
    used to turn a scroll into a tap on the item it started on.
  - *backlight*: the 500 ms dim/off check (`bl_tick`) runs only while the backlight is on or
    dimmed. *rtp_rx* waits for its socket without a 500 ms timeout.
  - *ui_bridge*: the 1 s refresh (clock, call timer, phone status snapshot) runs only while
    the backlight is on or dimmed (`display_is_lit()`); dark, the task blocks on its queue
    until a message. Events still refresh at once while dark (incoming / missed call,
    registration, BLF, settings, OTA), except the 10 s RSSI update (kept in `b.loc`, shown at
    wake-up). `DISPLAY_EVENT_BACKLIGHT` (on) kicks one refresh and the ticks resume; every
    refresh rebuilds the whole model, so nothing that changed while dark is lost. Incoming
    call while dark: `on_event` calls `display_wake` (SLPOUT) before it kicks the bridge, so
    the incoming-call screen is rendered into the panel during its 120 ms start-up, before
    the backlight lights (see Panel sleep). (`CALL_EVENT_SECOND` is the background call's
    state change, not a per-second tick: the call timers come from the 1 s refresh above.)
  - *UI updates* (`components/ui`): lit, every 1 s refresh with a changed model (the clock or
    a call timer) runs the screen's update. The hot paths only touch what changed:
    `ui_label_set_text_if_changed` (each `lv_label_set_text` frees and reallocates the text
    in the PSRAM LVGL heap and invalidates the label), `ui_style_set_color` / `_num` /
    `ui_set_text_color` (a style setter re-applies and invalidates even with the same value),
    and the fitted labels (`ui_fit_set`, DOTS mode, whose LVGL buffer may end in "...")
    remember their last text, start font and width (`fit_cache_t`, 84 B per fit plus its
    delete callback, LVGL heap). In a call one tick rewrites the timer label only (was ~10 labels and colours); the home screen per
    minute the clock only. A model whose only change is the RSSI (every 10 s) updates the
    status bar alone (it redraws only on a new bar count); only Settings (About shows dBm)
    still gets the screen update.
  - Left: net RSSI every 10 s (Wi-Fi driver call; the UI ignores it while dark), LVGL's 2 s
    max sleep, SNTP, Wi-Fi beacons.
- **Automatic light sleep** (`power`, board Kconfig `SIPWIRE_POWER_LIGHT_SLEEP`, **default n**:
  on the handset, after the chip had light-slept once, answering or ending a call sometimes
  crashed it (PSRAM "MMU entry fault" / panic handler hung with the cache off, watchdog reset),
  even after `power sleep off`; never with the option off. Cause not found. When on:
  needs `FREERTOS_USE_TICKLESS_IDLE`, set in `sdkconfig.defaults`; the QEMU overlay has
  `PM_ENABLE` off, which drops it). `esp_pm_configure(light_sleep_enable)`: the chip
  light-sleeps whenever both cores idle for 8 ticks and no `ESP_PM_NO_LIGHT_SLEEP` lock is
  held. If esp_pm rejects it, DFS alone is configured (logged).
  - Holds: `call` and `display` take a NO_LIGHT_SLEEP lock besides CPU_FREQ_MAX, so never
    during a call (ringing, dialing, ended screen) or with the backlight on / dimmed (LEDC
    PWM stops in light sleep, the panel needs SPI). `console` (NO_LIGHT_SLEEP only, DFS
    unaffected): see console below.
  - Wi-Fi: stays associated; modem sleep (above) + light sleep, the PM layer wakes the chip
    for every beacon the radio listens to (each DTIM, or each listen interval). Unicast for us
    (INVITE, ARP) is buffered by the AP and announced in the beacon's TIM, so INVITE latency
    is as for modem sleep (see above) plus the light-sleep exit (order of a millisecond). The
    INVITE turns into a call state change: `call` hold (no more sleep), `WIFI_PS_NONE`,
    backlight on. Wi-Fi sleep options left at the IDF defaults (`ESP_WIFI_SLP_IRAM_OPT`,
    `PM_SLP_IRAM_OPT` off: they would cost IRAM, see "Memory and tasks").
  - Wake sources (`power.c` `light_sleep_setup`): GPIO (`esp_sleep_enable_gpio_wakeup`) with
    the keypad columns and the touch PENIRQ armed level-low by their drivers
    (`gpio_wakeup_enable`). Both interrupts are level-triggered too, so the key or touch that
    woke the chip is still pending afterwards: the first press is not lost (the touch that
    wakes a dark screen is swallowed by design, as before). Columns can only go low while the
    rows are parked low, i.e. while the keypad idles. Timer (esp_timer alarms, FreeRTOS).
  - Console (UART0): RX edges wake the chip (`uart_wakeup_setup`, 3 edges), but the
    characters that wake it are lost. A UART wake-up (seen in the light-sleep exit callback)
    or any console input takes the `console` hold for `SIPWIRE_POWER_CONSOLE_AWAKE_S` (60 s)
    after the last input. So: press Enter once, wait for the prompt, then type. Input on the
    USB-Serial-JTAG secondary console does not wake the chip (use the UART port), and with
    USB connected the host may see the JTAG device drop while the chip sleeps
    (`USJ_NO_AUTO_LS_ON_CONNECTION` stays off so a USB-powered handset still sleeps).
  - Audio: the I2S driver's PM lock is held only while its channels are enabled, i.e. while
    a stream runs; the tone-only stream stops after 2 s idle (below), calls hold `call`.
  - Stats: `power` prints sleep count, time asleep and % since boot and since the previous
    `power`, and wake causes (timer / gpio / uart / wifi / other), from the exit callback
    (`PM_LIGHT_SLEEP_CALLBACKS`). Note the `console` hold: the 60 s after typing `power` are
    awake by design. `power locks` dumps every PM lock (a driver lock that blocks light sleep
    shows up there with its time held) and the mode / light-sleep counters.
  - To verify on the device: INVITE to ring latency asleep (DTIM 1-3), no missed INVITEs over
    hours, key and touch wake with the first key press delivered, console wake, Wi-Fi beacon
    loss / reconnects, current draw idle (screen off) before / after.
- **Panel sleep** (`display.c`, board Kconfig `SIPWIRE_DISPLAY_PANEL_SLEEP`, default y): when
  the backlight goes off (`bl_tick_cb`) the PWM goes to 0 first, then the ILI9341 gets SLPIN
  (DC/DC, oscillator and scanning stop). Any way back (`display_wake`: key, touch, incoming or
  missed call; `display_set_backlight`; a settings change that disables the timeout) sends
  SLPOUT first and lights the backlight only 120 ms later (`bl_ready` one-shot): the panel's
  start-up is never visible. Commands go straight through `esp_lcd_panel_io_tx_param`: the
  driver's `esp_lcd_panel_disp_sleep()` blocks 120 ms after both commands. Datasheet gaps:
  5 ms after SLPIN / SLPOUT before the next command (waited with the LVGL lock held), SLPIN
  no sooner than 120 ms after SLPOUT.
  - Flush safety: panel commands are sent only with the LVGL lock held, so the LVGL task is
    outside `lv_timer_handler` and no flush can start; LVGL waits for its last flush before it
    leaves a refresh, and `tx_param` drains queued colour transfers before a command anyway.
    LVGL is *not* paused while the panel sleeps: in Sleep In the SPI interface and frame
    memory keep working, so a flush to a sleeping panel completes normally (its
    `on_color_trans_done` still calls `lv_display_flush_ready`) and lands in GRAM. That is the
    safe choice: pausing the refresh timer would need every invalidation path to resume it,
    and a missed resume is a frozen screen; with flushes allowed, whatever LVGL drew while dark
    (the incoming-call screen, built during the 120 ms before the backlight lights) is what
    SLPOUT shows. While dark the bridge skips its 1 s refresh, so such flushes are rare.
  - Lock order: LVGL lock, then `bl_lock` (the touch path calls `display_wake` from the LVGL
    task with the recursive LVGL lock held). `bl_take()` takes the LVGL lock first whenever
    the backlight is off; `bl_tick_cb` (esp_timer task) waits at most 20 ms for it and
    otherwise goes dark at a later tick. `display_wake` from off therefore blocks until the
    LVGL task finishes its current pass (plus 5 ms): never call it holding a lock the LVGL
    task may wait for.
- **Status LED** (`components/status_led`): the RMT channel is enabled only around each
  ~30 us WS2812 frame (an enabled RMT TX channel holds a CPU_FREQ_MAX lock, which pinned DFS
  at 240 MHz). While the backlight is off (`SIPWIRE_DISPLAY_EVENT` / `DISPLAY_EVENT_BACKLIGHT`)
  the LED is dark and its 20 ms pattern timer is stopped, except for an incoming call.
- **Console `power`**: current CPU MHz, DFS range, active holds (`call`, `display`,
  `console`), Wi-Fi power save mode, light sleep (state, wake sources, stats); `power locks` dumps the IDF PM lock table (`esp_pm_dump_locks`) and, with
  `CONFIG_PM_PROFILING`, the time spent in each mode since boot (CPU_MAX = 240 MHz, APB_MIN /
  APB_MAX = 80 MHz). "cpu now" always reads 240 MHz: IDF holds its `rtos0`/`rtos1` CPU_FREQ_MAX
  locks while a core runs any task (here: the console), so DFS only lowers the clock while both
  cores idle. That idle time is where the saving is.

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

## Flash mode (QIO, 80 MHz)
`sdkconfig.defaults`: `CONFIG_ESPTOOLPY_FLASHMODE_QIO`, `CONFIG_ESPTOOLPY_FLASHFREQ_80M` (was
DIO). Every flash cache miss (code not in IRAM, rodata, fonts) moves address and data over
four lines instead of two, so a miss costs roughly half the time; nothing else changes (no
HPM, no timing tuning at 80 MHz; the PSRAM is octal on its own pins).

Who sets the mode:
- The image header (offset 0x2, what esptool writes and the ROM reads) stays `dio` for a QIO
  build (`CONFIG_ESPTOOLPY_FLASHMODE` = "dio"): the ROM loads the 2nd-stage bootloader in DIO.
- The **bootloader** (built with QIO) reads the flash ID, sets the QE bit in the flash status
  register (non-volatile; known chips by table, else the generic SR2 bit 1) and switches the
  cache's SPI0 read mode to QIO before it loads the app. Boot log: `Enabling QIO for flash chip
  ...` (or `Enabling default flash chip QIO`) and `SPI Mode : QIO`. If the QE bit does not
  stick it logs `Failed to set QIE bit, not enabling QIO mode` and stays DIO.
- The **app** never reprograms SPI0: code fetch runs in whatever mode the bootloader left.
  Its own `esp_flash` driver (SPI1: NVS, FAT `storage`, OTA writes, partition reads) uses the
  app's configured mode (log `spi_flash: flash io: qio`) and sets QE itself if needed.

So the bootloader decides, and a unit has to be flashed over serial once (bootloader +
partition table + otadata + app, `idf.py -p PORT flash`; NVS and `storage` are not touched,
settings and contacts stay). Mixed cases:
- DIO bootloader + QIO app (only `app-flash`, or an old unit): boots and works, the app sets
  QE and its SPI1 accesses use QIO, but code still runs from a DIO cache (no gain) while the
  app log says `flash io: qio`. Check the bootloader's `SPI Mode` line, not the app's.
- OTA: `esp_https_ota` checks the app descriptor's `spi_flash_mode` against the running app
  (`CONFIG_ESP_HTTPS_OTA_VERIFY_SPI_MODE`, default y; QIO = 1, DIO = 3). A DIO unit refuses
  a QIO image at the first block (`SPI flash mode mismatch: running app has mode 3, new app
  has mode 1`, `ESP_ERR_OTA_SPI_MODE_MISMATCH`, OTA FAILED before any image data is written,
  boot slot unchanged) and a QIO unit
  refuses a DIO image. OTA cannot migrate a unit, and cannot put a QIO app behind a DIO
  bootloader; between QIO builds it works as before.

QEMU (`tools/qemu/sdkconfig.qemu`) stays DIO: with QIO its flash controller model does not
keep the quad read mode (bootloader `SPI Mode : SLOW READ`) and the FAT `storage` partition
fails to mount even after formatting (smoke shards 30 and 40 fail). The QEMU smoke test
therefore does not cover QIO; the handset does.

An existing `sdkconfig` keeps its old choice over the new default. Delete every line
containing `FLASHMODE` (`CONFIG_ESPTOOLPY_FLASHMODE_QIO/QOUT/DIO/DOUT`,
`CONFIG_ESPTOOLPY_FLASHMODE`, `CONFIG_ESPTOOLPY_FLASHMODE_VAL` and the deprecated
`CONFIG_FLASHMODE_QIO/QOUT/DIO/DOUT`), then `idf.py reconfigure`.

## OTA update (`components/ota`)
`ota_start(url)` (console `ota [url]`, Settings > Update) downloads with
`esp_https_ota` into the inactive slot (`partitions.csv`: ota_0 / ota_1, 3 MB
each) in its own task. Plain http is allowed (`CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP`,
LAN update server; https uses the mbedtls cert bundle). Refused while a call is
not idle or another update runs. After the first block and before anything
else is written: the image header must be for esp32s3 (esp_https_ota rejects
other chips, and an image whose flash mode differs from the running app's: see Flash mode)
and the app descriptor's `project_name` must be `sipwire`. OTA never updates the
bootloader. Events
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
