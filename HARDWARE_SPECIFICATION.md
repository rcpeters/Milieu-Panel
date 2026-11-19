# HARDWARE SPECIFICATION

Hardware specification, wiring, power budget, and physical I2C/button behavior for **Milieu Panel** (1 to 8 RGB buttons on the **Seeed Studio XIAO ESP32C6**).

For firmware protocol and Home Assistant integration details, see [MATTER_REQUIREMENTS.md](MATTER_REQUIREMENTS.md) (Matter implementation) and [ESPHOME_REQUIREMENTS.md](ESPHOME_REQUIREMENTS.md) (ESPHome implementation).

## Scope
* **Controller:** [Seeed Studio XIAO ESP32C6](https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/) (USB-C powered).
* **Buttons:** 1 to 8x [DFRobot Gravity I2C RGB LED Button Module (DFR0991)](https://www.dfrobot.com/product-2634.html).
* **Out of scope:** Enclosure, battery power.

## Hardware & Wiring
* **I2C Bus (`100 kHz`, `50 ms` timeout, no external pull-ups):** Each `DFR0991` includes onboard pull-up resistors. With up to 8 modules in parallel (`~580–1250 Ω` effective pull-up), the bus runs at `100 kHz` (`Wire.setTimeOut(50)`) using direct I2C register access so a missing button never blocks the loop.
* **RF Antenna Switch (`GPIO3` & `GPIO14`):** Initialized at boot with `GPIO3 = LOW` (enable RF switch) and `GPIO14 = LOW` (internal ceramic antenna; set `USE_EXTERNAL_ANTENNA=1` for external U.FL).

| Signal | XIAO ESP32C6    | Button Module (`0x23`–`0x2A`) | Gravity Wire |
|--------|-----------------|-------------------------------|--------------|
| SDA    | `D4` (`GPIO22`) | `D` (SDA)                     | Green        |
| SCL    | `D5` (`GPIO23`) | `C` (SCL)                     | Blue         |
| 3.3V   | `3V3`           | `+` (VCC)                     | Red          |
| GND    | `GND`           | `-` (GND)                     | Black        |

### DIP Switch Addresses
Each `DFR0991` module must be set to a unique I2C address (`0x23`–`0x2A`) via its 3-bit DIP switch (`A2 A1 A0` = `000`–`111`) before powering on:

| Button | Address | DIP (`A2 A1 A0`) | Button | Address | DIP (`A2 A1 A0`) |
|--------|---------|------------------|--------|---------|------------------|
| **1**  | `0x23`  | `0 0 0`          | **5**  | `0x27`  | `1 0 0`          |
| **2**  | `0x24`  | `0 0 1`          | **6**  | `0x28`  | `1 0 1`          |
| **3**  | `0x25`  | `0 1 0`          | **7**  | `0x29`  | `1 1 0`          |
| **4**  | `0x26`  | `0 1 1`          | **8**  | `0x2A`  | `1 1 1`          |

## 3.3V Power Budget & Brightness Limits
* **XIAO ESP32C6 `3V3` Rail:** `700 mA` LDO max minus `354 mA` ESP32-C6 peak Wi-Fi TX = `346 mA` peak headroom (`250 mA` recommended continuous peripheral budget; `200 mA` conservative).
* **Per-Button Draw (`DFR0991` Full White):** `~2 mA` MCU (`16 mA` for 8 buttons) + up to `60 mA` worst-case (`~35 mA` typical at 3.3V) LED current at 100% white.
* **Firmware Fixed Brightness (`10%` / `PWM 26`):** Brightness is fixed locally in firmware (scaling RGB output to `10%`) so all 8 buttons in full white stay well within the `3V3` budget:

| Mode / Budget Tier | Brightness (`PWM`) | Per-Button LED (White) | 8-Button Total (incl. `16 mA` MCUs) |
|--------------------|--------------------|------------------------|-------------------------------------|
| **Active (Firmware)** | **`10%` (`PWM 26`)** | `6.0 mA` max / `3.5 mA` typ | **`64 mA` max / `44 mA` typ** |
| **Recommended Cap** | `48%` (`PWM 124`) | `29.25 mA` max | `250 mA` |
| **Hardware Limit** | `68%` (`PWM 175`) | `41.25 mA` max | `346 mA` |

## I2C Discovery, Hot-Plug & Physical Reset
* **Initial I2C Scan:** When uncommissioned (or when no button mask is saved), the firmware scans `0x23`–`0x2A`, retrying every `5 s` until at least 1 button responds.
* **Hot-Plug Recovery:** If a paired button disconnects from the I2C bus at runtime, the firmware marks it disconnected without blocking other buttons and probes it every `5 s` to reconnect and restore its LED state.
* **Physical Reset (`BOOT` Pin):** Holding the XIAO's onboard `BOOT` button (`GPIO9`)—or shorting the `BOOT` (`GPIO9`) and `GND` pads/pins underneath the board—for `5 s` flashes all connected buttons **Purple 3x** (`10%` brightness, `150 ms` on/off) before clearing state and restarting.

## Software & Build (Hardware Layer)
* **Board Target:** `XIAO_ESP32C6` (Espressif `esp32` Arduino core v3.x+).
* **Libraries & Config:** `Wire` (built-in) and `DFRobot_RGBButton`, with hardware pins, I2C addresses, PWM levels, and debounce/probe timings defined in [`sketches/milieuMatter/config.h`](sketches/milieuMatter/config.h).

## Acceptance Criteria
- [x] Builds for `XIAO_ESP32C6`, initializes RF switch (`GPIO3 = LOW`, `GPIO14 = LOW`), and configures `100 kHz` I2C with a `50 ms` timeout.
- [x] Scans `0x23`–`0x2A` when uncommissioned (retrying every `5 s` if empty) and auto-recovers hot-plugged I2C buttons every `5 s`.
- [x] Caps LED brightness in firmware at `10%` (`PWM 26`) to keep 8-button full-white draw within the `3V3` power budget.
- [x] Triggers factory reset and flashes all connected buttons **Purple 3x** when `BOOT` (`GPIO9`) is held for `5 s`.
