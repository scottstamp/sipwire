# QEMU smoke test

Boots the real firmware in Espressif's QEMU (`qemu-system-xtensa -M esp32s3`)
and drives it over the serial console against a private Asterisk on the host.
Runs in CI (`.github/workflows/ci.yml`, job `smoke`, manual trigger only).

The build uses `sdkconfig.qemu` on top of `sdkconfig.defaults`, which sets
`CONFIG_SIPWIRE_QEMU`. That swaps only what QEMU can't emulate:

| Part | Hardware build | QEMU build |
|---|---|---|
| Network | Wi-Fi STA | OpenCores Ethernet (slirp, guest 10.0.2.15, host 10.0.2.2) |
| Audio | I2S speaker + mic | paced silence (same 10 ms cadence) |
| Echo canceller | SpeexDSP (`components/aec`) | the same code, on a silent mic: exercises the reference plumbing, stats and CPU path, not the cancellation |
| PSRAM | 8 MB octal | 8 MB quad (`-m 8M`; QEMU has no octal mode) |
| Display | ILI9341 + XPT2046 | LVGL renders into a discarding display |
| Keypad | matrix scan | disabled (floating GPIOs read as "pressed") |

Everything else (SIP, RTP, phone state machine, UI, console, NVS) is the
production code.

## Run

```bash
. $IDF_PATH/export.sh
python $IDF_PATH/tools/idf_tools.py install qemu-xtensa   # once
sudo apt-get install asterisk                             # once
tools/qemu/build.sh     # -> build_qemu/qemu_flash.bin (skips the 16 MB merge if nothing changed)
tools/qemu/smoke.sh     # all shards in parallel, ~20 s -> RESULT OK
tools/qemu/smoke.sh smoke.d/30-blf-mwi.txt   # one scenario file, output streamed
```

## Shards

`smoke.sh` runs every `smoke.d/[0-9]*.txt` at the same time. A shard has its own QEMU
(scratch copy of the flash image, so NVS starts empty), its own private Asterisk (SIP port
`$SIP_PORT` = 5180 + shard index, RTP range 12100 + 100 * index, own state dir and CLI socket)
and its own OTA HTTP server. The guest reaches the host at 10.0.2.2 through slirp, so only the
host ports differ. Override the bases with `SMOKE_PORT_BASE` / `SMOKE_RTP_BASE`. The shards
include `_boot.inc` / `_account.inc` (boot, account config, registration) as common prefix.

| Shard | Covers | ~Wall |
|---|---|---|
| `10-call-echo` | G.722 echo call, hold/resume, RTP counters, echo canceller on/off, DTMF, remote hangup | 12 s |
| `20-call-inbound` | OTA refusals (wrong/junk/missing image), inbound answer/reject, unknown number, account change, account removal, `tasks`/`cpu` | 13 s |
| `30-blf-mwi` | BLF ringing/busy/idle, MWI voicemail left and deleted, live BLF list change | 13 s |
| `40-ota` | live time settings, the real OTA update, reboot, boot from `ota_1`, marked valid | 18 s |

Add a shard by dropping a file in `smoke.d/`; keep the slowest one near the others (the run
takes as long as the slowest shard).

Output: the console shows one PASS/FAIL line per shard with its wall time (the tail of a
failed shard's log is printed). Per shard in `build_qemu/`: `smoke-<shard>.log` (guest
console plus the runner's `###` steps), `smoke-<shard>.asterisk.log`,
`smoke-<shard>.ota-http.log`. Exit status is non-zero if any shard failed.

## Scenario files

One step per line, `#` comments:

| Step | Meaning |
|---|---|
| `do <text>` | type a console line, wait for the next `sipwire> ` prompt |
| `send <text>` | type a console line, do not wait for the reply |
| `prompt [timeout_s]` | wait until the console is idle (boot prompt) |
| `expect <regex> [timeout_s]` | wait for a matching output line (default 15 s) |
| `absent <regex> [seconds]` | fail if a matching line shows up in the window (default 3 s; 0 = only what is already buffered) |
| `poll <timeout_s> <regex> :: <text>` | repeat `do <text>` until its reply has a matching line (state that follows a command a moment later) |
| `sleep <s>` | only where time itself is tested; say why in a comment |
| `shell <cmd>` | host command (`$ASTX '<cli command>'` runs an Asterisk CLI command) |
| `include <file>` | steps of another file, relative to this one |

`${NAME}` in `do/send/expect/absent/poll` lines expands environment variables (`${SIP_PORT}`,
`${OTA_BASE}`, see below). The first failed step stops the run. The reader works on the raw
serial bytes (the prompt has no newline) and keeps a line view for `expect`. `do`/`send` first
wait for the previous command's prompt, so a line is never typed into a running command. Do not wait
with `sleep` for a reply or a log line: `expect` or `poll` it.

Every `###` line in the output starts with `[total s +step s]` (host wall time since start and
duration of the step), and the run ends with the five slowest steps and the total, so slow
spots are easy to find.

## OTA scenarios

`components/ota` is tested in two shards. Each shard's `smoke.sh` serves three files over HTTP on
a free host port (`$OTA_BASE`, guest `http://10.0.2.2:<port>`): `sipwire.bin` (the firmware
just built), `other.bin` (same image, other `project_name`) and `junk.bin` (random bytes). In
`20-call-inbound` the last two and a 404 must be refused with a reason and no reboot (OTA is also
refused while a call is up or just ended, so that block runs before any call). In `40-ota`
`sipwire.bin` must show progress, reboot (QEMU keeps running, so the written flash image stays),
boot from `ota_1` (offset 0x320000) and mark itself valid.

## Known flake

QEMU itself fails now and then in early boot or during OTA image verification, in any shard
(most often `40-ota`, around the reboot after the update): a hang, `qemu-system-xtensa: Bad ram
pointer 0x4209....` (QEMU exits; the runner reports `QEMU exited` at once), or `Guru Meditation
Error ... LoadStorePIFAddrError` with `EXCVADDR` 0x6002300x (SYSTIMER registers) right after
`esp_eth` init or `esp_image: segment 3`. A single unloaded QEMU running the old sequential
scenario showed it as well. It looks like a bug in QEMU's dual-core flash-cache emulation, not
firmware or runner. Check the shard log before blaming a change; re-run the shard
(`tools/qemu/smoke.sh smoke.d/40-ota.txt`).

A log line from another task can land in the middle of a console reply (`sip_port = I (..)
phone: ... 5182`), so `expect` on a reply that follows a `cfg set` matches the key, not the value.
