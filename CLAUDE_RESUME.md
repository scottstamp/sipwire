# Resume notes for the next Claude session

Written at the end of the first (cloud) session, 2026-10-29, to hand over to
a session on the user's own Linux box. Read this, then `CLAUDE.md`,
`docs/ARCHITECTURE.md`, `docs/PLAN.md`, `docs/HARDWARE.md`.

## Where things stand

`main` = `bbdaa95` (`v0.1.0-5-gbbdaa95`), pushed to
`github.com/scottstamp/sipwire` (private). Firmware is running on the real
handset and works: registration, calls, G.722 wideband, UI, touch, keypad,
OTA (confirmed on device, much faster than serial flashing).

Built so far (all merged, host-tested, QEMU-smoke-tested):
- `board` (all pins in Kconfig; defaults = the reference board), `common`
  (event bus), `storage` (NVS settings), `net` (Wi-Fi; power save off during
  calls), `cli` (serial console `sipwire>`), `display` (ILI9341 + XPT2046,
  backlight, raw-touch hook), `keypad`, `audio` (I2S full duplex 16 kHz,
  tones), `codec` (G.711, G.722, 2x resampler), `rtp` (jitter buffer, PLC,
  RFC 4733 DTMF), `sip` (own RFC 3261 UA + RFC 6665 subscriptions),
  `phone` (call control, BLF/MWI), `ota` (HTTP OTA + rollback), `status_led`
  (WS2812), `ui` (LVGL screens), `main/ui_bridge*`.
- UI: home (clock 12/24 h, BLF tiles, voicemail button), dialer, in-call,
  incoming, ended, Settings: Wi-Fi, SIP account, Calibrate, Time (NTP + TZ),
  Update (OTA), BLF, About.
- Web UI simulator (`sim/web`), QEMU smoke test (`tools/qemu`), Asterisk dev
  PBX (`tools/asterisk`), SIP-vs-Asterisk integration test
  (`test_host/sip/integration.sh`), manual-only GitHub Actions CI.

## Action items

Needs the user / hardware:
1. **Verified on device (2026-10-29)**: BLF tiles + MWI against the user's
   Asterisk, touch calibration, call audio (no stutter with power save off,
   G.722 quality good, mic fine), one successful OTA.
   Still open: OTA rollback (pull power mid-download; flash an image that
   crashes at boot and confirm it rolls back); echo and core-1 load
   (`tasks` during a call) not reported yet.
2. `v0.1.0` tag pushed from the local box (2026-10-29).
3. **Unknown hardware facts**: MS3625 data format/clock range, backlight
   drive (active high assumed), battery divider (not wired yet), dock-detect
   design (not wired yet; dock only has power + USB data).
4. **Amp SD_MODE on GPIO 18**: firmware drives it (high = on). Confirm the
   user wired it; if the amp pops at boot add a pull-down ≤ 22 kΩ.

Open development work (see `docs/PLAN.md` for the full list):
- AEC / speakerphone echo (Phase 5, `[core]`).
- Battery ADC (GPIO 4 reserved) once the divider exists; dock detect design.
- Contacts, call log (ringtones done: `ring` console cmd, setting ring_tone) (no SD card for now — user doesn't want it;
  use the 9.9 MB `storage` partition if storage is needed).
- Review task priorities, DMA sizing, PSRAM placement (`[core]`).
- SIP over TCP/TLS, SRTP, RTCP (optional).
- Known small gaps: https OTA untested. (SIP INFO DTMF fallback, 491 retry,
  RTP timestamps after hold and speaker-volume persistence are done.)
- Call waiting / transfer / DND / page auto answer: phone + SIP done (see
  ARCHITECTURE "Call features"); no UI screens for them yet. Incoming
  INVITE with Replaces and incoming REFER are not supported (REFER → 603).
- QEMU emulator faults (segfault / LoadStorePIFAddrError on valid MMIO
  around OTA, only with the host CPU saturated) are QEMU's multi-threaded
  TCG; smoke.sh re-runs such a shard once (tools/qemu/README.md "Emulator
  faults"). Any other failure is real.

## Hardware (details in docs/HARDWARE.md)

- ESP32-S3-WROOM-1-**N16R8** (16 MB flash, 8 MB octal PSRAM) on a
  DevKitC-1 v1.0-compatible AliExpress clone. GPIO 38 is free, the WS2812
  LED is on 48 (user probed it).
- ILI9341 240x320 portrait: `LCD_MIRROR_X=y` correct on device.
  XPT2046 touch axes are rotated 180° vs the panel: `TOUCH_MIRROR_Y=y`
  (separate touch swap/mirror options exist).
- MAX98357A + MS3625 share BCLK 15 / WS 7 (one I2S controller, full
  duplex). DOUT 6, mic DIN 5, amp SD_MODE 18.
- Keypad rows 39/1/2/41, cols 38/40/42. Console UART0 (DevKit "UART" USB
  port, COM4 on the user's Windows PC).
- CPU 240 MHz. Partitions: dual 3 MB OTA slots + 9.9 MB `storage`;
  rollback bootloader (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`).

## Workflow

- Build: `idf.py set-target esp32s3 && idf.py build` (ESP-IDF **v6.1**).
  Version string comes from `git describe` (root `CMakeLists.txt`).
- Host tests: `tools/ci.sh host-test` (needs `IDF_PATH` for Unity; ASan/UBSan,
  `-Werror`). SIP vs Asterisk: `test_host/sip/integration.sh` (needs
  `asterisk` from apt; runs its own instance on 127.0.0.1:5080).
- QEMU smoke: `tools/qemu/build.sh && tools/qemu/smoke.sh` (needs
  `idf_tools.py install qemu-xtensa`). Four shards in `tools/qemu/smoke.d/`
  run in parallel, ~18 s wall; one shard: `smoke.sh smoke.d/40-ota.txt`.
  Logs: `build_qemu/smoke-<shard>.log`. `build.sh` regenerates
  `build_qemu/sdkconfig` only when the defaults files change (hash stamp),
  so don't delete it by hand. `build.sh` and `ci.sh build` pass `--ccache`
  (ccache `base_dir=/home/scott`, so worktrees hit the same cache); a warm
  clean build is ~40 s vs ~85 s. Host tests run with `ctest -j`, ~3 s.
- UI sim: `sim/web/build.sh` (Emscripten), `serve.sh --watch`,
  `node sim/web/screenshot.mjs out.png <scenario>` or `--all <dir>`; look at
  screenshots before shipping UI changes.
- Device updates: OTA. Serve `build/sipwire.bin` as `ota.bin` with
  any HTTP server on 192.168.2.193:80 (default OTA URL
  `http://192.168.2.193/ota.bin`), then Settings → Update or console
  `ota`. Serial flashing (`write_flash 0x20000 sipwire.bin`) keeps settings;
  a merged image at `0x0` erases them.
- Console setup after an erase: `wifi <ssid> <pass>`, `cfg set sip_server
  …`, `sip_user`, `sip_password`, `blf 1002:Bob,…`, `vm_number *97`.

## Conventions and preferences

- User prefers terse, direct replies.
- Git identity in this repo: `Claude <noreply@anthropic.com>` (a stop hook
  rejected commits under the user's email). Commit messages end with
  `Co-Authored-By: Claude <model> <noreply@anthropic.com>`. Push to `main`
  after verified work (the user has been fine with that); no PRs so far.
- GitHub Actions CI is **manual only** (`workflow_dispatch`) — the user
  wants to save runner minutes. Don't re-enable push triggers.
- Work was split across subagents in git worktrees: opus for design-heavy
  parts (SIP, phone glue, G.722, BLF/MWI), sonnet for well-specified
  components and UI, haiku for small mechanical ones. Each brief named
  scope, contract headers, and required verification; the main session
  reviewed, merged, and re-ran all checks. Keep doing that.
- Rules from `CLAUDE.md`: pins only via board Kconfig; `codec`/`rtp`/`sip`
  stay host-buildable (no IDF headers); `ui` is pure LVGL (sim builds it);
  LVGL only under `lvgl_port_lock()`; no new managed components without a
  README note.

## Environment notes (old cloud box only, safe to ignore now)

The cloud sandbox couldn't reach Espressif's component registry, so builds
went through a wrapper with local clones of lvgl / esp_lvgl_port /
esp_lcd_ili9341 / esp_lcd_touch(_xpt2046) / cmake_utilities. On a normal
machine `idf.py build` fetches them from the registry (`main/` and
`components/*/idf_component.yml`). Managed deps in use: `lvgl/lvgl ~9.6.0`,
`espressif/esp_lvgl_port ^2.9.0`, `espressif/esp_lcd_ili9341 ^2.1.0`,
`atanisoft/esp_lcd_touch_xpt2046 ^1.0.6`.
