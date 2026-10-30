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
| `50-call-waiting` | DND (486 + missed call event), page auto answer (Asterisk ext 650: Dial with a `Call-Info: answer-after=0` pre-dial header), call waiting answer2/swap/hangup → held call promoted, reject2 | 12 s |
| `60-transfer` | blind transfer of a bridged inbound call (REFER → ext 603, sipfrag 200, we hang up), failed transfer (404) keeps the call, attended transfer (consultation call to 600, REFER with Replaces), core dump (`coredump` with none, `coredump crash` panic, reboot warning, decoded summary, `coredump erase`) | 15 s |

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

## Former flake: USB-Serial-JTAG tick hook

Until 2026-10-29 roughly 1 run in 8 failed in early boot or during OTA image verification: a
hang, `Guru Meditation Error ... LoadStorePIFAddrError` (`EXCVADDR` 0x60038008), or QEMU itself
exiting (segfault, `Bad ram pointer`). The panic PC was `usb_serial_jtag_sof_tick_hook`, the
FreeRTOS tick hook that IDF installs for the USB-Serial-JTAG secondary console; QEMU handles
that register badly. `sdkconfig.qemu` now sets `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y` (the
device keeps the default; its console is UART0). Afterwards: `40-ota` 15/15, full suite 3/3.
PSRAM was ruled out (no PSRAM: still 3 failures in 15). If it comes back, decode the panic PC
with `xtensa-esp32s3-elf-addr2line -e build_qemu/sipwire.elf` first.

## Emulator faults (retried once)

Still seen after that fix, only with the host CPU saturated (8 shards next to 8 busy loops: 2
failures in 16; 0 in 26 without the load): around the OTA write / verify / reboot QEMU either
segfaults (`QEMU exited (rc=-11)`) or the guest takes a `LoadStorePIFAddrError` on a valid MMIO
register (systimer `0x6002304c`, `0x60020050`) with no sipwire code on the stack. Guest code
cannot crash the emulator, so this is QEMU (its multi-threaded TCG, one host thread per core).
`-accel tcg,thread=single` is ~3x slower and breaks the post-OTA boot, so it is not used.
`run.py` exits 3 on such a fault and `smoke.sh` re-runs that shard once (`[re-run after
emulator fault]` in the summary; both attempts are in the shard log). Failed steps are never
retried. A `Guru Meditation` with any other cause, or one in our code, is a real failure.

A log line from another task can land in the middle of a console reply (`sip_port = I (..)
phone: ... 5182`), so `expect` on a reply that follows a `cfg set` matches the key, not the value. The same
interleaving can put a reply and a log line on one line: never `expect` the same alternation
twice (`(reply|log)` x2); expect the log line once (`do` has already waited for the prompt).
