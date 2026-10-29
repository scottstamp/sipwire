# QEMU smoke test

Boots the real firmware in Espressif's QEMU (`qemu-system-xtensa -M esp32s3`)
and drives it over the serial console against a private Asterisk on the host.
Runs in CI (`.github/workflows/ci.yml`, job `smoke`).

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
tools/qemu/build.sh     # → build_qemu/qemu_flash.bin
tools/qemu/smoke.sh     # → RESULT OK; log in build_qemu/smoke.log
```

`smoke.sh [scenario] [log]` takes another scenario file. Format, one step per
line: `send <text>`, `expect <regex> [timeout_s]`, `absent <regex> [seconds]` (fails
if a matching line appears), `sleep <s>`, `shell <cmd>`
(`$ASTX '<cli command>'` runs an Asterisk CLI command), `# comment`. `${NAME}` in
send/expect/absent lines expands environment variables (`${OTA_BASE}`, see below). The first
failed step stops the run. Wait for a reply before the next `send`: characters
typed while the firmware is logging can be lost.

Every run boots a fresh copy of the flash image, so NVS starts empty.

## OTA scenario

The last steps of `smoke.txt` test `components/ota`. `smoke.sh` serves three files over
HTTP on a free host port (`$OTA_BASE`, guest `http://10.0.2.2:<port>`): `sipwire.bin` (the
firmware just built), `other.bin` (same image, other `project_name`) and `junk.bin`
(random bytes). The firmware runs `ota <url>`: the last two and a 404 must be refused with a
reason and no reboot; `sipwire.bin` must show progress, reboot (QEMU keeps running, so the
written flash image stays), boot from `ota_1` (offset 0x320000) and mark itself valid.
`ota-http.log` next to `smoke.log` has the server's access log.
