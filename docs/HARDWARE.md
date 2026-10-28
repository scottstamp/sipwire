# Hardware

Reference build of the sipwire handset. The firmware stays configurable for
any board of the same class (ESP32-S3 + ILI9341/XPT2046 + I2S amp + I2S MEMS
mic + 3x4 matrix): every pin and option lives in `menuconfig → Sipwire board`
(`components/board/Kconfig`), never in code. The table in
[Firmware configuration](#firmware-configuration) lists the values for this
build.

## Parts

| Block | Part | Interface | Power |
|---|---|---|---|
| MCU | ESP32-S3-WROOM-1 on an ESP32-S3-DevKitC-1 (N8R8 or N16R8: octal PSRAM) | — | USB / 5Vin |
| Display | 2.8" TFT SPI 240x320 "V1.2" module, ILI9341 | SPI2 | VCC 5V (on-board 3.3V regulator) |
| Touch | XPT2046 resistive, on the display module | SPI2 (shared), own CS + IRQ | from module |
| Speaker amp | MAX98357A I2S class-D DAC/amp, 3 W | I2S (TX) | Vin 5V |
| Speaker | 8 Ω (4 Ω also works with the MAX98357A) | amp SPK+/SPK− | — |
| Microphone | MS3625 I2S digital MEMS, omnidirectional | I2S (RX), clocks shared with amp | VDD 3.3V only |
| Keypad | 3x4 matrix (0-9, *, #), 7-pin ribbon (4 rows + 3 cols) | GPIO | — |
| Status LED | WS2812 RGB, on the DevKit | GPIO (RMT) | — |
| Buttons | BOOT button on the DevKit; handset dock-detect switch | GPIO | — |
| Battery | 3.7V Li-ion pack, voltage sense via ADC | ADC1_CH3 | — |
| SD card | not fitted in this build | — | — |

## Pin map

| GPIO | Function | Peripheral | Notes |
|---:|---|---|---|
| 0 | BOOT button | DevKit | strapping pin, no external load |
| 1 | Keypad row 2 (4 5 6) | keypad ribbon pin 7 | |
| 2 | Keypad row 3 (7 8 9) | keypad ribbon pin 6 | |
| 4 | Battery voltage sense | ADC1_CH3 | via divider, see [Power](#power) |
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
| 21 | Handset dock detect | dock switch | |
| 38 | Keypad column 1 (1 4 7 *) | keypad ribbon pin 1 | see [Open points](#open-points) |
| 39 | Keypad row 1 (1 2 3) | keypad ribbon pin 2 | |
| 40 | Keypad column 2 (2 5 8 0) | keypad ribbon pin 3 | |
| 41 | Keypad row 4 (* 0 #) | keypad ribbon pin 4 | |
| 42 | Keypad column 3 (3 6 9 #) | keypad ribbon pin 5 | |
| 43 | UART0 TX | console (USB-UART bridge) | |
| 44 | UART0 RX | console (USB-UART bridge) | |
| 48 | WS2812 status LED | DevKit on-board LED | |

Reserved / unused:

- **35, 36, 37**: wired to the octal PSRAM/flash on N8R8/N16R8 modules. Never use.
- **3, 45, 46**: strapping pins, left unconnected (0 is the BOOT button).
- **19, 20**: native USB D−/D+, left free for USB-JTAG.
- **18, 47**: free. Either can serve as an SD card CS on the SPI2 bus later.

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
- **MAX98357A**
  - SD_MODE is tied to 3V3 (directly or through 100 kΩ). The amp is always
    enabled and plays the left channel.
  - There is no shutdown/enable GPIO, so pop suppression is up to the
    firmware (fade in/out, silence before stop).
  - GAIN is tied to GND for +12 dB. Floating gives +9 dB.
  - The amp is powered from 5V; its logic inputs accept the 3.3V I2S levels.
- **MS3625**
  - L/R is tied to GND, so the mic outputs on the left slot.
  - VDD is 3.3V only. Never connect it to 5V.

## Power

- **5V rail** (DevKit 5Vin): MAX98357A Vin and TFT VCC.
- **3.3V rail** (DevKit 3V3): MS3625 VDD.
- **Ground**: all modules share a common ground.
- **Battery**: 3.7V Li-ion pack; its voltage is sensed on GPIO 4 (ADC1_CH3).
  A full cell (4.2V) is above the ADC input range, so the sense line needs a
  divider. The ratio is not documented yet.

## Firmware configuration

Kconfig values (`CONFIG_SIPWIRE_*`) for this build. Other settings keep their
defaults.

| Option | Value | Option | Value |
|---|---|---|---|
| `LCD_SPI_HOST` | 2 | `SPK_BCLK` | 15 |
| `LCD_SCLK` | 12 | `SPK_WS` | 7 |
| `LCD_MOSI` | 11 | `SPK_DOUT` | 6 |
| `LCD_MISO` | 13 | `SPK_EN` | -1 (no pin) |
| `LCD_CS` | 10 | `I2S_FULL_DUPLEX` | y |
| `LCD_DC` | 14 | `MIC_DIN` | 5 |
| `LCD_RST` | 9 | `MIC_SLOT_RIGHT` | n |
| `LCD_BL` | 8 | `KP_ROW0` / `ROW1` / `ROW2` / `ROW3` | 39 / 1 / 2 / 41 |
| `TOUCH_ENABLE` | y | `KP_COL0` / `COL1` / `COL2` | 38 / 40 / 42 |
| `TOUCH_CS` | 16 | `KP_MAP` | `123456789*0#` |
| `TOUCH_IRQ` | 17 | `SD_ENABLE` | n |

Also needed: octal PSRAM enabled (`CONFIG_SPIRAM`, octal mode), and the flash
size of the actual module (8 MB for N8R8, 16 MB for N16R8; the partition table
fits 8 MB).

Hardware without firmware support yet: WS2812 status LED (48), dock detect
(21), battery ADC (4), BOOT button (0).

## Open points

Check these on the real hardware:

1. **DevKitC-1 revision vs. keypad column 1.** On DevKitC-1 v1.0 the on-board
   RGB LED is on GPIO 48, but on v1.1 it is on GPIO 38, the keypad column 1
   pin. Confirm the board revision; on a v1.1 board column 1 must move (to 18
   or 47).
2. **Module variant**: N8R8 or N16R8 (flash size).
3. **MS3625 data format**: sample width and justification, and its supported
   BCLK range at the shared 16 kHz × 64 clock. This sets `MIC_SHIFT` and
   confirms full-duplex timing.
4. **Battery**: divider ratio on GPIO 4, charger, and how the pack feeds the
   5V/3.3V rails.
5. **Dock detect**: switch type and polarity on GPIO 21 (pull-up, and which
   level means "docked").
6. **Backlight**: whether the module's LED pin is driven through a
   transistor (active high assumed) and the PWM range it tolerates.
