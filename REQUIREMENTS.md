# REQUIREMENTS

**Milieu Panel:** A Matter-over-Wi-Fi multi-button RGB controller (1 to 8 buttons) built on the **Seeed Studio XIAO ESP32C6** for Home Assistant.

## Scope
* **Pure Matter device (1–8 buttons):** Auto-discovers 1 to 8 physical I2C RGB buttons (`0x23`–`0x2A`) and exposes each as 2 Matter endpoints (`MatterGenericSwitch` + `MatterColorLight`, up to 16 endpoints total).
* **No middleware or secrets:** Uses standard Matter BLE/Wi-Fi commissioning stored in NVS. No MQTT, `.env` files, or external YAML/Blueprints required.
* **Out of scope:** Enclosure, battery power, OTA updates, Thread transport.

## Hardware & Wiring
* **Board:** [Seeed Studio XIAO ESP32C6](https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/) (USB-C powered)
* **Buttons:** 1 to 8x [DFRobot Gravity I2C RGB LED Button Module (DFR0991)](https://www.dfrobot.com/product-2634.html), each set to a unique I2C address (`0x23`–`0x2A`) via its 3-bit DIP switch (`A2 A1 A0` = `000`–`111`).
* **I2C Bus (`100 kHz`, `50 ms` timeout, no external pull-ups):** Each `DFR0991` includes onboard pull-up resistors. With up to 8 modules in parallel (`~580–1250 Ω` effective pull-up), the bus runs at `100 kHz` (`Wire.setTimeOut(50)`) using direct I2C register access so a missing button never blocks the loop.
* **RF Antenna Switch (`GPIO3` & `GPIO14`):** Initialized at boot with `GPIO3 = LOW` (enable RF switch) and `GPIO14 = LOW` (internal ceramic antenna; set `USE_EXTERNAL_ANTENNA=1` for external U.FL).

| Signal | XIAO ESP32C6    | Button Module (`0x23`–`0x2A`) | Gravity Wire |
|--------|-----------------|-------------------------------|--------------|
| SDA    | `D4` (`GPIO22`) | `D` (SDA)                     | Green        |
| SCL    | `D5` (`GPIO23`) | `C` (SCL)                     | Blue         |
| 3.3V   | `3V3`           | `+` (VCC)                     | Red          |
| GND    | `GND`           | `-` (GND)                     | Black        |

## 3.3V Power Budget & Brightness Limits
* **XIAO ESP32C6 `3V3` Rail:** `700 mA` LDO max minus `354 mA` ESP32-C6 peak Wi-Fi TX = `346 mA` peak headroom (`250 mA` recommended continuous peripheral budget; `200 mA` conservative).
* **Per-Button Draw (`DFR0991` Full White):** `~2 mA` MCU (`16 mA` for 8 buttons) + up to `60 mA` worst-case (`~35 mA` typical at 3.3V) LED current at 100% white.
* **Firmware Fixed Brightness (`10%` / `PWM 26`):** Brightness is fixed locally (`hsv.v` from Matter is ignored, always converting color at full value `254` and scaling to `10%` in firmware) so all 8 buttons in full white stay well within the `3V3` budget:

| Mode / Budget Tier | Brightness (`PWM`) | Per-Button LED (White) | 8-Button Total (incl. `16 mA` MCUs) |
|--------------------|--------------------|------------------------|-------------------------------------|
| **Active (Firmware)** | **`10%` (`PWM 26`)** | `6.0 mA` max / `3.5 mA` typ | **`64 mA` max / `44 mA` typ** |
| **Recommended Cap** | `48%` (`PWM 124`) | `29.25 mA` max | `250 mA` |
| **Hardware Limit** | `68%` (`PWM 175`) | `41.25 mA` max | `346 mA` |

## Discovery, Persistence & Reset
* **Uncommissioned I2C scan:** At boot, if uncommissioned (or no mask is saved in NVS namespace `"milieu"`), the firmware scans `0x23`–`0x2A`, retrying every `5 s` until ≥ 1 button responds. The discovered 8-bit `btn_mask` is saved to NVS.
* **Locked while commissioned:** Once commissioned (`commissioned = true` in NVS), the firmware loads `btn_mask` from NVS at boot to keep Matter endpoint IDs fixed. If the device was removed from the Matter fabric remotely (`g_nvsCommissioned && !Matter.isDeviceCommissioned()`), it clears NVS and restarts to re-scan.
* **Hot-plug recovery:** If a paired button disconnects from I2C at runtime, the firmware marks it disconnected without blocking other buttons and probes it every `5 s` to reconnect and restore its LED state.
* **Factory reset (`5 s` `BOOT` hold):** Holding `BOOT` (`GPIO9`) for `5 s` flashes all connected buttons **Purple 3x** (`10%` brightness, `150 ms` on/off), clears NVS, calls `Matter.decommission()`, and restarts.

## Matter Node & Endpoints
* **Node Metadata:** Vendor `"Make Rob Make"`, Product/Device Name `"Milieu Panel"`.
* **Two-Pass Endpoint Registration & Labeling:** For `N` discovered buttons (`i = 1..N`, ordered by I2C address `0x23`–`0x2A`), all switches are registered first (`EP 1..N`) and all lights second (`EP N+1..2N`). Each endpoint is assigned a Matter Semantic Tag (`MatterTags::Number::NS = i` + custom label tag) and a `BridgedDeviceBasicInformation` `NodeLabel` for clear naming in Home Assistant:

| Endpoint | Matter Class          | NodeLabel & Custom Tag | Home Assistant Entity |
|----------|-----------------------|------------------------|-----------------------|
| `i` (`1..N`) | `MatterGenericSwitch` | `"Button i"`           | `event`               |
| `N + i` (`N+1..2N`) | `MatterColorLight` | `"Button i Light"`   | `light`               |

## Behavior

### Button Events (`MatterGenericSwitch`)
Initialized with `FEATURE_MOMENTARY | FEATURE_RELEASE | FEATURE_LONG_PRESS`. Polled every `20 ms` with `50 ms` debounce and `1000 ms` long-press threshold (active only when `ONLINE`):
* **Press (`50 ms`):** Emits `InitialPress` (`sw.press()`).
* **Release `< 1000 ms`:** Emits `ShortRelease` (`sw.release()`).
* **Hold `≥ 1000 ms`:** Emits `LongPress` (`sw.longPress()`) at `1000 ms`, then `LongRelease` (`sw.longRelease()`) on release.

### RGB LEDs (`MatterColorLight` & Status)
Each `MatterColorLight` starts `Off` with HSV `{0, 0, 254}` (White). When Home Assistant sends a state/color update (`onChange`), `haSynced` is set to `true` and the HSV hue/saturation are converted to RGB at full value (`v = 254`).

| State | LED Behavior |
|-------|--------------|
| **Uncommissioned (pairing)** | Blink **Blue** at `10%` (`PWM 26`), `1 Hz` (`500 ms` on/off) |
| **Commissioned, Wi-Fi/Matter offline** | Blink **Red** at `10%` (`PWM 26`), `1 Hz` (`500 ms` on/off) |
| **Online, waiting for HA sync (`!haSynced`)** | Solid **White** at `10%` (`PWM 26`) until first HA light command |
| **Online (`isOn = true`)** | Solid **HA RGB color** at `10%` (`PWM 26`) |
| **Online (`isOn = false` after sync)** | **Off (`PWM 0`)** |
| **Factory reset (`BOOT` held `5 s`)** | Flash **Purple** 3x at `10%` (`PWM 26`, `150 ms` on/off) |

## Software & Build
* **Toolchain:** Arduino IDE 2.x, Espressif `esp32` core v3.x+ (`XIAO_ESP32C6`), Partition Scheme `Huge APP (3MB No OTA/1MB SPIFFS)`, `USB CDC On Boot: "Enabled"` (`115200` baud).
* **Libraries:** Built-in `Matter`, `WiFi`, `WebServer`, `DNSServer`, `Wire`, `Preferences`, plus `DFRobot_RGBButton`.
* **Files:** [`Milieu-Panel.ino`](Milieu-Panel.ino) (application logic) and [`config.h`](config.h) (pins, I2C addresses, PWM levels, timing, SoftAP settings, and `ButtonSlot` state).

## Temporary Commissioning Wi-Fi Portal & Offline QR Code Page

When the panel is **uncommissioned**, the ESP32-C6 broadcasts an open temporary Wi-Fi SoftAP alongside BLE Matter advertising (`WIFI_MODE_APSTA` coexistence mode) so the user can view the Matter **QR Code** and **Manual Pairing Code** without needing a USB Serial connection:
1. **Temporary SoftAP Lifecycle:**
   * **Start condition:** Started automatically at boot only when `!Matter.isDeviceCommissioned()` (after `Matter.begin()` initializes the STA interface).
   * **SSID:** `"Milieu-Panel-Setup"` (open network, default IP `192.168.4.1`).
   * **Coexistence (`WIFI_MODE_APSTA`):** Uses ESP-IDF `esp_netif` / `esp_wifi` directly to keep the Matter STA interface active so the Matter `NetworkCommissioning` cluster can scan and join the user's home Wi-Fi network over BLE during commissioning.
   * **Automatic teardown:** Immediately stops the HTTP server, DNS server, and SoftAP (`esp_wifi_set_mode(WIFI_MODE_STA)`) once `Matter.isDeviceCommissioned()` becomes `true`.
2. **Captive Portal & Web Server (`http://192.168.4.1`):**
   * Runs built-in `DNSServer` (port `53`, redirecting all DNS queries to `192.168.4.1`) and `WebServer` (port `80`) so connecting on a phone, tablet, or laptop automatically opens (or resolves any URL to) the onboarding page.
   * Serves a single self-contained HTML/CSS/JS page from flash (`PROGMEM`) with **zero external internet dependencies**.
3. **Offline JavaScript QR Code & Onboarding UI:**
   * Extracts the raw Matter QR payload string (`MT:...`) from `Matter.getOnboardingQRCodeUrl()` and injects it with `Matter.getManualPairingCode()` and the discovered button count/addresses.
   * Includes an embedded, dependency-free JavaScript QR encoder (`makeQrSvg`) that renders the `MT:...` payload to an inline `<svg>` QR code directly in the browser, alongside a one-tap **Copy Manual Code** button and discovered hardware summary.

## Acceptance Criteria
- [x] Builds for `XIAO_ESP32C6` (`Huge APP`), initializes RF switch (`GPIO3 = LOW`, `GPIO14 = LOW`) and `100 kHz` I2C with `50 ms` timeout.
- [x] Scans `0x23`–`0x2A` when uncommissioned (retrying every `5 s` if empty), saves `btn_mask` to NVS, blinks blue (`10%`), prints Matter pairing details on Serial, and hosts the `"Milieu-Panel-Setup"` (`192.168.4.1`) offline QR code captive portal until commissioned.
- [x] Registers Switches on `EP 1..N` (`"Button i"`) and Lights on `EP N+1..2N` (`"Button i Light"`) with `BridgedDeviceBasicInformation` labels and semantic tags, under Vendor `"Make Rob Make"` / Product `"Milieu Panel"`.
- [x] Automatically clears saved NVS mask and re-scans if remotely decommissioned from the Matter fabric, or when `BOOT` (`GPIO9`) is held for `5 s` (flashing purple 3x).
- [x] Emits `InitialPress`, `ShortRelease`, `LongPress`, and `LongRelease` switch events.
- [x] Displays `10%` White on boot (while reporting `Off` to Matter for HA reconnect sync) and shows HA RGB colors at a fixed `10%` brightness (ignoring the Matter brightness slider).
- [x] Blinks red (`10%`) when offline and auto-recovers hot-plugged I2C buttons every `5 s`.
