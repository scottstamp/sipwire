# Hardware

Target class: Wi-Fi "cordless" SIP handset. Final pinout TBD; firmware must
work for any board matching this description. All pins/options live in
`menuconfig → Sipwire board` (Kconfig in `components/board`), never hardcoded.

| Block     | Part class                         | Bus   | Notes |
|-----------|------------------------------------|-------|-------|
| MCU       | ESP32-S3 (PSRAM strongly preferred)| —     | Wi-Fi 2.4 GHz. 8 MB flash min, PSRAM for LVGL buffers + jitter buffer |
| Speaker   | I2S DAC + 3 W class-D amp (e.g. MAX98357A) | I2S0 TX | 16-bit, 8/16 kHz. Optional SD/EN gpio for mute |
| Mic       | I2S MEMS (e.g. INMP441, SPH0645)   | I2S1 RX (or I2S0 full-duplex if shared clocks) | 24-bit in 32-bit slot, L/R select pin |
| Display   | 240x320 ILI9341                    | SPI2  | DC, RST, CS, BL (PWM). 16-bit RGB565 |
| Touch     | XPT2046 (resistive, on ILI9341 module) | SPI2 shared, own CS + IRQ | Calibration stored in NVS |
| Keypad    | 3x4 matrix, 7 pins (4 rows + 3 cols) | GPIO | Internal pull-ups, scan task, debounce |
| SD card   | microSD, optional                  | SDMMC 1-bit or SPI | Detect at boot; absent = fine |
| Buttons   | optional: hook/call, end, vol+/-   | GPIO  | can be mapped to keypad keys instead |
| Battery   | optional: ADC divider, charge stat | ADC   | later |

## Pin budget (S3, approx)
SPI display+touch 7, I2S out 3(+1 EN), I2S in 3(+1 L/R), keypad 7, SD 3–6,
backlight 1 → ~26–30 GPIOs. Fits S3 (45 GPIO) but avoid strapping pins
(0, 3, 45, 46) and 26–32 (PSRAM/flash) and USB 19/20 if using USB-JTAG.

## I2S modes
- Preferred: two controllers (I2S0 TX dac, I2S1 RX mic), independent clocks.
- Fallback: one controller full-duplex (shared BCLK/WS) if pins are tight.
  Kconfig `SIPWIRE_I2S_FULL_DUPLEX` selects this.

## Audio format
Internal: 16-bit mono PCM, 8 kHz (G.711) — 16 kHz when G.722 enabled.
Mic 24→16 bit shift + DC-block; speaker gain in software; amp EN for mute.
