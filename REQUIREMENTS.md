# REQUIREMENTS

**Milieu Panel (v1):** A Matter-over-Wi-Fi multi-button RGB controller (1 to 8 buttons) built on the **Seeed Studio XIAO ESP32C6** for Home Assistant.

## Scope
* **Pure Matter device (1–8 buttons):** Auto-discovers 1 to 8 physical I2C RGB buttons (`0x23`–`0x2A`) and exposes each as 2 Matter endpoints (`MatterGenericSwitch` + `MatterColorLight`, up to 16 endpoints total).
* **No middleware or secrets:** Uses standard Matter BLE/Wi-Fi commissioning (stored in NVS). No MQTT, no `.env` files, and no Home Assistant YAML/Blueprints required in the repo.
* **Out of scope:** Enclosure, battery power, OTA updates, Thread transport.

## Hardware & Pinout
* **Board:** [Seeed Studio XIAO ESP32C6](https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/) (USB-C powered)
* **Buttons:** 1 to 8x [DFRobot Gravity I2C RGB LED Button Module (DFR0991)](https://www.dfrobot.com/product-2634.html), each assigned a unique I2C address (`0x23` to `0x2A`) via its 3-bit DIP switch (`A2 A1 A0` = `000` to `111`).
* **I2C Bus (`100 kHz`, no external pull-ups):** Each `DFR0991` module includes onboard I2C pull-up resistors. With up to 8 modules in parallel, the effective pull-up resistance drops to ~1/8th (`~580–1250 Ω`), so the firmware runs I2C at standard **`100 kHz`** (`Wire.setClock(100000)`) with no additional external pull-up resistors.
* **RF Antenna Switch (`GPIO3` & `GPIO14`):** The XIAO ESP32C6 onboard RF switch (`FM8625H`) is initialized at boot by driving `GPIO3 = LOW` (RF switch enable) and `GPIO14 = LOW` (built-in ceramic antenna default; configurable to `HIGH` via build flag for an external U.FL antenna).
* **Wiring (3.3V direct I2C in parallel, no level shifter):**

| Signal | XIAO ESP32C6    | Each Button Module (`0x23`–`0x2A`) |
|--------|-----------------|------------------------------------|
| SDA    | `D4` (`GPIO22`) | `D` (SDA)                          |
| SCL    | `D5` (`GPIO23`) | `C` (SCL)                          |
| 3.3V   | `3V3`           | `+` (VCC)                          |
| GND    | `GND`           | `-` (GND)                          |

## 3.3V Peripheral Power & Max Brightness Calculation

### 1. XIAO ESP32C6 3.3V Rail Specifications
* **Onboard 3.3V LDO Regulator Max Output ($I_{\text{LDO,max}}$):** `700 mA` total (shared between the ESP32-C6 SoC and the `3V3` pin).
* **ESP32-C6 Peak Wi-Fi TX Current ($I_{\text{ESP,peak}}$):** `354 mA` (802.11b @ 21 dBm per ESP32-C6 datasheet; Espressif recommends reserving up to `500 mA` for RF peaks).
* **LDO Thermal Limit ($5.0\text{ V} \to 3.3\text{ V}$ drop):** Dropping `1.7 V` across the onboard LDO generates $P = 1.7\text{ V} \times I_{\text{total}}$. Keeping continuous peripheral draw $\le 250\text{ mA}$ prevents thermal throttling.
* **Available Peripheral Current Budget ($I_{\text{periph}}$):**
  * **Absolute Peak Headroom:** $700\text{ mA} - 354\text{ mA} = \mathbf{346\text{ mA}}$
  * **Recommended Continuous Budget (with ~20% thermal/RF margin):** $\mathbf{250\text{ mA}}$
  * **Conservative Budget (reserving full 500 mA for ESP32-C6):** $700\text{ mA} - 500\text{ mA} = \mathbf{200\text{ mA}}$

### 2. Per-Button Current Draw (DFR0991 at Full White `[255, 255, 255]`)
* **Onboard I2C MCU quiescent current ($I_{\text{MCU}}$):** `~2 mA` per button (`16 mA` total for 8 buttons).
* **RGB LED Full White (`R=255, G=255, B=255`):**
  * **Worst-case (20 mA/channel $\times$ 3):** `60 mA` LED + `2 mA` MCU = **`62 mA` per button** (`496 mA` for 8 buttons — exceeds the `346 mA` headroom).
  * **Typical at 3.3V supply ($V_f$ limited on G/B):** `~35 mA` LED + `2 mA` MCU = **`37 mA` per button** (`296 mA` for 8 buttons).

### 3. Maximum Brightness & Firmware Operating Levels (All 8 Buttons White)
After subtracting `16 mA` for the 8 button MCUs, the maximum per-button LED current is $I_{\text{LED,btn}} = (I_{\text{periph}} - 16\text{ mA}) / 8$, and the maximum PWM value is $\text{PWM}_{\max} = \lfloor 255 \times (I_{\text{LED,btn}} / I_{\text{full\_white}}) \rfloor$:

| Operating / Budget Tier | Brightness (`PWM`) | LED Current / Button (`60 mA` / `35 mA` White) | Total 8-Button Draw (incl. `16 mA` MCUs) | Status vs. `3V3` Rail Limit |
|-------------------------|--------------------|------------------------------------------------|------------------------------------------|-----------------------------|
| **Normal Idle (Firmware)** | **`10%` (`PWM 26 / 255`)** | `6.0 mA` worst-case / `3.5 mA` typical | **`64 mA` worst-case / `44 mA` typical** | Well within `200 mA` conservative limit |
| **Pressed Hold (Firmware)** | **`40%` (`PWM 102 / 255`)** | `24.0 mA` worst-case / `14.0 mA` typical | **`208 mA` worst-case / `128 mA` typical** *(if all 8 held at once)* | Safely under `250 mA` recommended budget |
| **Recommended Max Cap** | **`48%` (`PWM 124 / 255`)** | `29.25 mA` worst-case | `250 mA` | Max continuous safe peripheral budget |
| **Absolute Hardware Limit** | `68%` (`PWM 175 / 255`) | `41.25 mA` worst-case | `346 mA` | Zero margin at peak Wi-Fi TX (`700 mA` total) |

* **Firmware Fixed Brightness (`10%` Idle / `40%` Pressed):** The firmware controls brightness locally — buttons run at **`10%` (`PWM 26`)** when idle and boost to **`40%` (`PWM 102`)** while held down. Home Assistant controls only On/Off and RGB color via `MatterColorLight` (ignoring the Matter brightness slider), guaranteeing that even all 8 buttons held simultaneously at full white draw at most `208 mA`.

## Discovery, Persistence & Reset
* **Uncommissioned I2C scan:** When uncommissioned, the firmware scans `0x23`–`0x2A` at boot. If 0 buttons are found, it logs on Serial and retries the scan every 5 s before starting Matter. Once ≥ 1 button is found, the discovered 8-bit address mask is saved to NVS and Matter endpoints are registered.
* **Locked while commissioned:** While commissioned, the firmware always loads the saved NVS button mask at boot so the Matter endpoint topology stays fixed. If a previously paired button is temporarily missing on the I2C bus, the firmware logs the missing address, keeps running the remaining buttons, and retries reconnecting to the missing button every 5 s.
* **Factory reset (5 s `BOOT` hold):** Holding the onboard `BOOT` button (`GPIO9`) for 5 s flashes all connected buttons **Purple 3 times** (`40%` brightness, 150 ms on / 150 ms off) for visual confirmation, clears the saved button mask in NVS, and calls `Matter.decommission()`, restarting the board to re-scan the I2C bus and enter pairing mode.

## Matter Endpoints
For each discovered button `i` (`1..N`, where `N <= 8`, ordered by I2C address `0x23`–`0x2A`):

| Endpoint | Matter Class          | Function                      | Home Assistant Entity |
|----------|-----------------------|-------------------------------|-----------------------|
| `2i - 1` | `MatterGenericSwitch` | Button `i` press/hold events  | `event`               |
| `2i`     | `MatterColorLight`    | Button `i` RGB LED            | `light`               |

## Behavior

### Button Events (`MatterGenericSwitch`)
Each `MatterGenericSwitch` endpoint is initialized with the **`FEATURE_MOMENTARY | FEATURE_RELEASE | FEATURE_LONG_PRESS`** (`MomentarySwitchLongPress`) Switch Cluster feature flags enabled so Home Assistant exposes all press and release events. Active buttons are polled over I2C every **20 ms** with **50 ms** debounce and a **1000 ms** long-press threshold (ignored while uncommissioned or offline):
* **Push down (at 50 ms):** Emits `InitialPress`.
* **Release < 1000 ms:** Emits `ShortRelease`.
* **Held ≥ 1000 ms:** Emits `LongPress` immediately at 1000 ms, then `LongRelease` when released.
* **Press duration:** Home Assistant receives both the discrete event type (`short_release` vs `long_press`/`long_release`) and the exact hold duration from the time between `initial_press` and release.

### RGB LEDs (`MatterColorLight` & Status)
Home Assistant controls each button's **On/Off** state and **RGB color** via `MatterColorLight` (the Matter brightness attribute is ignored; brightness is managed locally by firmware at **`10%` (`PWM 26`)** idle and **`40%` (`PWM 102`)** while pressed).
* **Boot / Reconnect State Sync with HA:** In Matter, light endpoints are server clusters (they don't pull state from the controller), so on boot the firmware does not restore stale colors from NVS. Instead, it initializes each `MatterColorLight` endpoint as `Off` (`[255, 255, 255]`) while showing **`10%` White** locally on the physical LED. When the Matter session connects, Home Assistant sees the light entity transition from `unavailable` / `off` and its automation immediately pushes the current live state and RGB color.

| State | LED Behavior |
|-------|--------------|
| **Uncommissioned (pairing mode)** | Blink **Blue** at `10%` brightness (`PWM 26`) at 1 Hz (500 ms on / 500 ms off) on all connected buttons |
| **Commissioned, Wi-Fi or Matter offline** | Blink **Red** at `10%` brightness (`PWM 26`) at 1 Hz (500 ms on / 500 ms off) on all connected buttons |
| **Online, waiting for HA sync (after boot)** | Solid **White** at **`10%` brightness (`PWM 26`)** until HA sends the first `MatterColorLight` command |
| **Online, idle (`MatterColorLight` On)** | Solid **HA-selected RGB color** at **`10%` brightness (`PWM 26`)** |
| **Online, idle (`MatterColorLight` Off)** | **Off (`PWM 0`)** |
| **Online, button held down (`InitialPress` to release)** | Boosts to **`40%` brightness (`PWM 102`)** of the active RGB color (or **`40%` White** if `MatterColorLight` is `Off` / uninitialized), returning immediately to `10%` (or `Off`) when released |
| **Factory reset triggered (`BOOT` held 5 s)** | Flash **Purple** 3 times at `40%` brightness (`PWM 102`, 150 ms on / 150 ms off) before rebooting |

## Software & Build
* **Toolchain:** PlatformIO, Arduino-ESP32 v3.x+ (`seeed_xiao_esp32c6`), `huge_app.csv` partition table.
* **Libraries:** Built-in `Matter`, `WiFi`, and `Preferences` (NVS), plus `DFRobot_RGBButton`.
* **Configuration:** Pin definitions (`GPIO22`/`GPIO23` I2C at `100 kHz`, `GPIO3`/`GPIO14` RF antenna switch), I2C address range (`0x23`–`0x2A`), brightness constants (`LED_PWM_IDLE = 26`, `LED_PWM_PRESSED = 102`), and timing constants live in `platformio.ini` / code.
* **Serial:** USB CDC on boot enabled (`115200` baud); prints discovered I2C button addresses, Matter manual pairing code, and QR code URL.

## Acceptance Criteria
- [ ] Firmware builds and flashes for `seeed_xiao_esp32c6` via PlatformIO with no `.env` file needed, initializing the RF switch (`GPIO3 = LOW`, `GPIO14 = LOW`) and `100 kHz` I2C bus.
- [ ] Uncommissioned boot scans `0x23`–`0x2A` (retrying every 5 s if 0 buttons are found), saves the discovered button mask in NVS, blinks connected buttons blue (`10%`) at 1 Hz, and prints the Matter QR code URL and manual pairing code on Serial.
- [ ] Commissions into Home Assistant over Matter (BLE + Wi-Fi) with 1 `event` entity (`FEATURE_MOMENTARY | FEATURE_RELEASE | FEATURE_LONG_PRESS`) and 1 RGB `light` entity per discovered button (supporting 1 to 8 buttons).
- [ ] Holding the onboard `BOOT` button (`GPIO9`) for 5 s flashes all connected buttons purple 3 times (`40%`), clears the NVS button mask, decommissions the Matter node, and re-scans on reboot.
- [ ] Button presses emit `InitialPress`, `ShortRelease`, `LongPress`, and `LongRelease` events with accurate timing.
- [ ] Online buttons start at `10%` White on boot (reporting `Off` on `MatterColorLight` so HA automations can detect boot/reconnection and push current colors), display HA's selected RGB color at `10%` when `On` (or dark when `Off`), and boost to `40%` brightness while held down (using `40%` White if `Off`), dropping back to `10%` (or `Off`) immediately on release.
- [ ] LEDs blink red (`10%`) at 1 Hz when Wi-Fi or Matter connectivity is lost and restore their state when reconnected.
- [ ] Any paired button that is temporarily disconnected on I2C does not block other buttons and retries connection every 5 s.
