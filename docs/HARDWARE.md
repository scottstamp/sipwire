# Hardware

Reference build of the sipwire handset. The firmware stays configurable for
any board of the same class (ESP32-S3 + ILI9341/XPT2046 + I2S amp + I2S MEMS
mic + 3x4 matrix): every pin and option lives in `menuconfig → Sipwire board`
(`components/board/Kconfig`), never in code. The Kconfig defaults are this
build; [Firmware configuration](#firmware-configuration) lists them.

## Parts

| Block | Part | Interface | Power |
|---|---|---|---|
| MCU | ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB octal PSRAM in the module) on an ESP32-S3-DevKitC-1 v1.0-compatible clone | — | USB / 5Vin |
| Display | 2.8" TFT SPI 240x320 "V1.2" module, ILI9341 | SPI2 | VCC 5V (on-board 3.3V regulator) |
| Touch | XPT2046 resistive, on the display module | SPI2 (shared), own CS + IRQ | from module |
| Speaker amp | MAX98357A I2S class-D DAC/amp, 3 W | I2S (TX) | Vin 5V |
| Speaker | 8 Ω (4 Ω also works with the MAX98357A) | amp SPK+/SPK− | — |
| Microphone | MS3625 I2S digital MEMS, omnidirectional | I2S (RX), clocks shared with amp | VDD 3.3V only |
| Keypad | 3x4 matrix (0-9, *, #), 7-pin ribbon (4 rows + 3 cols) | GPIO | — |
| Status LED | WS2812 RGB, on the DevKit | GPIO (RMT) | — |
| Button | BOOT button on the DevKit | GPIO | — |
| Battery | 3.7V Li-ion pack (planned; no voltage sense wired yet) | — | — |
| Dock | planned: power + USB data only, no detect signal yet | — | — |
| SD card | not fitted | — | — |

## Pin map

| GPIO | Function | Peripheral | Notes |
|---:|---|---|---|
| 0 | BOOT button | DevKit | strapping pin, no external load |
| 1 | Keypad row 2 (4 5 6) | keypad ribbon pin 7 | |
| 2 | Keypad row 3 (7 8 9) | keypad ribbon pin 6 | |
| 5 | I2S data in | MS3625 SD | |
| 6 | I2S data out | MAX98357A DIN | |
| 7 | I2S word select (LRCLK) | MAX98357A LRC + MS3625 WS | shared |
| 8 | Backlight (PWM) | TFT LED | LEDC |
| 9 | Display reset | TFT RESET | |
| 10 | Display chip select | TFT CS | active low |
| 11 | SPI MOSI | TFT SDI + touch T_DIN | shared |
| 12 | SPI clock | TFT SCK + touch T_CLK | shared |
| 13 | SPI MISO | TFT SDO + touch T_DO | shared |
| 14 | Display data/command | TFT DC | |
| 15 | I2S bit clock | MAX98357A BCLK + MS3625 SCK | shared |
| 16 | Touch chip select | XPT2046 T_CS | active low |
| 17 | Touch interrupt | XPT2046 T_IRQ | active low while touched |
| 18 | Amp shutdown / enable | MAX98357A SD_MODE | high = on (left channel), low = shutdown |
| 38 | Keypad column 1 (1 4 7 *) | keypad ribbon pin 1 | free on this board (probed; LED is on 48) |
| 39 | Keypad row 1 (1 2 3) | keypad ribbon pin 2 | |
| 40 | Keypad column 2 (2 5 8 0) | keypad ribbon pin 3 | |
| 41 | Keypad row 4 (* 0 #) | keypad ribbon pin 4 | |
| 42 | Keypad column 3 (3 6 9 #) | keypad ribbon pin 5 | |
| 43 | UART0 TX | console (USB-UART bridge) | |
| 44 | UART0 RX | console (USB-UART bridge) | |
| 48 | WS2812 status LED | DevKit on-board LED | |

Reserved / unused:

- **35, 36, 37**: wired to the module's octal PSRAM. Never use.
- **3, 45, 46**: strapping pins, left unconnected (0 is the BOOT button).
- **19, 20**: native USB D−/D+, left free for USB-JTAG.
- **4, 21, 47**: free. Reserved uses: 4 (ADC1_CH3) for battery sense; 47 and
  21 for separate mic clocks (see [Audio](#audio)) or an SD card CS on SPI2.

The keypad ribbon pins 1–7 land in order on DevKit left-header pins 13–19
(GPIO 38, 39, 40, 41, 42, 2, 1), so the ribbon solders straight across with
no crossed wires. The ribbon order interleaves rows and columns.

## Display and touch

- ILI9341 and XPT2046 share SPI2 (SCK 12, MOSI 11, MISO 13). Each has its own
  chip select: display CS 10, touch T_CS 16.
- On the module, bridge SDI–T_DIN, SDO–T_DO and SCK–T_CLK.
- Display control: DC 14, RESET 9, backlight LED 8 (PWM).
- Touch interrupt T_IRQ on 17.
- Native panel orientation is 240x320 portrait.

## Audio

- **One I2S controller, full duplex.** The amp and the mic share BCLK (15)
  and WS (7). Data out goes to the amp on 6, and data in comes from the mic
  on 5. Both directions therefore run at the same sample rate and slot format.
- **Shared clocks are fine** for this design. Constraints, all handled in
  firmware: speaker and mic use the same rate (16 kHz on the wire) and slot
  format (32-bit stereo frames, BCLK = 64 × 16 kHz = 1.024 MHz), and both
  directions start and stop together. If the MS3625 misbehaves at that clock,
  give the mic its own BCLK/WS on GPIO 47/21 and set `I2S_FULL_DUPLEX=n`,
  `MIC_BCLK=47`, `MIC_WS=21`; no code change.
- **MAX98357A**
  - SD_MODE goes to GPIO 18 (remove the 3V3 tie). The firmware drives it high
    (> 1.4 V: on, left channel) while audio runs and low (< 0.16 V: shutdown)
    when idle, which removes idle hiss and lets it mute around start/stop.
  - If the breakout pulls SD_MODE up and the amp pops at boot, add a pull-down
    of 22 kΩ or less so it stays in shutdown until the firmware takes over.
  - GAIN is tied to GND for +12 dB. Floating gives +9 dB.
  - The amp is powered from 5V; its logic inputs accept the 3.3V I2S levels.
- **MS3625**
  - L/R is tied to GND, so the mic outputs on the left slot.
  - VDD is 3.3V only. Never connect it to 5V.

## Power

- **5V rail** (DevKit 5Vin): MAX98357A Vin and TFT VCC.
- **3.3V rail** (DevKit 3V3): MS3625 VDD.
- **Ground**: all modules share a common ground.
- **Battery** (planned): 3.7V Li-ion pack. Voltage sensing is not wired; when
  it is, use GPIO 4 (ADC1_CH3) through a divider (a full cell, 4.2V, is above
  the ADC range).

## Firmware configuration

Kconfig values (`CONFIG_SIPWIRE_*`) for this build. Other settings keep their
defaults.

| Option | Value | Option | Value |
|---|---|---|---|
| `LCD_SPI_HOST` | 2 | `SPK_BCLK` | 15 |
| `LCD_SCLK` | 12 | `SPK_WS` | 7 |
| `LCD_MOSI` | 11 | `SPK_DOUT` | 6 |
| `LCD_MISO` | 13 | `SPK_EN` | 18 (active high) |
| `LCD_CS` | 10 | `I2S_FULL_DUPLEX` | y |
| `LCD_DC` | 14 | `MIC_DIN` | 5 |
| `LCD_RST` | 9 | `MIC_SLOT_RIGHT` | n |
| `LCD_BL` | 8 | `KP_ROW0` / `ROW1` / `ROW2` / `ROW3` | 39 / 1 / 2 / 41 |
| `TOUCH_ENABLE` | y | `KP_COL0` / `COL1` / `COL2` | 38 / 40 / 42 |
| `TOUCH_CS` | 16 | `KP_MAP` | `123456789*0#` |
| `TOUCH_IRQ` | 17 | `SD_ENABLE` | n |
| `STATUS_LED` | 48 | `LCD_MIRROR_X` | y |
| `TOUCH_MIRROR_Y` | y (touch axes differ from the panel's) | | |

`sdkconfig.defaults` also sets 16 MB flash in Quad I/O mode at 80 MHz
(`CONFIG_ESPTOOLPY_FLASHMODE_QIO`: the N16R8's flash is quad, its WP/HD lines are wired to
the module's SPIWP/SPIHD; the bootloader enables QIO, so flash each unit over serial once,
see `docs/ARCHITECTURE.md` "Flash mode") and octal PSRAM
(`CONFIG_SPIRAM_MODE_OCT`, malloc may use it; boot continues if absent).
Flash layout (`partitions.csv`): NVS 24 KB @ 0x9000, otadata, phy_init, `coredump` 56 KB @
0x12000, `ota_0` / `ota_1` 3 MB each @ 0x20000 / 0x320000, FAT `storage` 9.9 MB @ 0x620000
(contacts, history). A partition table change needs one serial flash per unit (OTA does not
write it).

Status LED patterns (`components/status_led`): white = booting, red blink =
no network, amber blink = registering, red = registration failed, green
pulse every 5 s = ready, amber pulse = no account, blue fast blink =
incoming call, cyan blink = calling, green = in call, green blink = on hold.

Not used by the firmware yet: BOOT button (0).

## Open points

Check these on the real hardware:

1. **MS3625 data format**: sample width and justification, and whether it
   runs at a 1.024 MHz BCLK. This sets `MIC_SHIFT` and confirms the shared
   clocks (fallback above).
2. **Backlight**: whether the module's LED pin is driven through a
   transistor (active high assumed) and the PWM range it tolerates.
3. **Panel orientation, color order, touch axes**: `LCD_BGR`, `LCD_MIRROR_*`,
   `LCD_SWAP_XY` (see `components/display`).
