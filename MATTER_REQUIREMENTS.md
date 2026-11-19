# MATTER REQUIREMENTS

Matter-over-Wi-Fi node architecture, endpoints, commissioning portal, and Home Assistant behavior for **Milieu Panel**.

For wiring, DIP switch addresses, I2C bus details, and the 3.3V power budget, see [HARDWARE_SPECIFICATION.md](HARDWARE_SPECIFICATION.md).

## Scope
* **Pure Matter device (1–8 buttons):** Exposes each discovered button (`0x23`–`0x2A`) as 2 Matter endpoints (`MatterGenericSwitch` + `MatterColorLight`, up to 16 endpoints total).
* **No middleware or secrets:** Uses standard Matter BLE/Wi-Fi commissioning stored in NVS. No MQTT, `.env` files, or external YAML/Blueprints required.
* **Out of scope:** OTA updates, Thread transport.

## Matter Node & Endpoints
* **Node Metadata:** Vendor `"Make Rob Make"`, Product/Device Name `"Milieu Panel"`.
* **Two-Pass Endpoint Registration & Labeling:** For `N` discovered buttons (`i = 1..N`, ordered by I2C address `0x23`–`0x2A`), all switches are registered first (`EP 1..N`) and all lights second (`EP N+1..2N`). Each endpoint is assigned a Matter Semantic Tag (`MatterTags::Number::NS = i` + custom label tag) and a `BridgedDeviceBasicInformation` `NodeLabel` for clear naming in Home Assistant:

| Endpoint | Matter Class          | NodeLabel & Custom Tag | Home Assistant Entity |
|----------|-----------------------|------------------------|-----------------------|
| `i` (`1..N`) | `MatterGenericSwitch` | `"Button i"`           | `event`               |
| `N + i` (`N+1..2N`) | `MatterColorLight` | `"Button i Light"`   | `light`               |

## Persistence & Decommissioning
* **Locked while commissioned:** Once commissioned (`commissioned = true` in NVS namespace `"milieu"`), the firmware loads the saved 8-bit `btn_mask` from NVS at boot to keep Matter endpoint IDs fixed across reboots.
* **Remote fabric removal:** If the device was removed from the Matter fabric remotely (`g_nvsCommissioned && !Matter.isDeviceCommissioned()`), it clears NVS and restarts to re-scan the I2C bus.
* **Manual decommission:** Triggered by holding `BOOT` (`GPIO9`) for `5 s` (see [HARDWARE_SPECIFICATION.md](HARDWARE_SPECIFICATION.md)); clears NVS, calls `Matter.decommission()`, and restarts.

## Behavior

### Button Events (`MatterGenericSwitch`)
Initialized with `FEATURE_MOMENTARY | FEATURE_RELEASE | FEATURE_LONG_PRESS`. Polled every `20 ms` with `50 ms` debounce and `1000 ms` long-press threshold (active only when `ONLINE`):
* **Press (`50 ms`):** Emits `InitialPress` (`sw.press()`).
* **Release `< 1000 ms`:** Emits `ShortRelease` (`sw.release()`).
* **Hold `≥ 1000 ms`:** Emits `LongPress` (`sw.longPress()`) at `1000 ms`, then `LongRelease` (`sw.longRelease()`) on release.

### RGB LEDs (`MatterColorLight` & Status)
Each `MatterColorLight` starts `Off` with HSV `{0, 0, 254}` (White). When Home Assistant sends a state/color update (`onChange`), `haSynced` is set to `true` and the HSV hue/saturation are converted to full-value RGB, then scaled linearly by Home Assistant's `0–100%` brightness (`hsv.v` / `254`) onto `0..LED_PWM_MAX`:
* **Compile-Time Max Brightness Cap (`MAX_BRIGHTNESS_PCT`):** Defaults to **`10%`** (`PWM 26`) in [`config.h`](sketches/milieuMatter/config.h) and is enforced at compile time via `static_assert` to **`≤ 48%`** on `3.3V` (`POWER_RAIL_5V 0`), or up to **`100%`** (`PWM 255`) when `POWER_RAIL_5V 1` is explicitly enabled for a 5V hardware build (see [5V_UPGRADE_README.md](5V_UPGRADE_README.md)).
* **Minimum Visible Floor:** When a light is `On` with a non-zero color/level and linear scaling rounds all RGB channels to `0` (e.g., `1%` slider at `10%` cap), the strongest channel(s) are floored to `PWM 1` so the button stays visibly lit.
* **Status LEDs (`LED_PWM_STATUS`):** Status states stay capped at `min(10% / PWM 26, LED_PWM_MAX)`.

| State | LED Behavior |
|-------|--------------|
| **Uncommissioned (pairing)** | Blink **Blue** at `LED_PWM_STATUS` (default `10%` / `PWM 26`), `1 Hz` (`500 ms` on/off) |
| **Commissioned, Wi-Fi/Matter offline** | Blink **Red** at `LED_PWM_STATUS` (default `10%` / `PWM 26`), `1 Hz` (`500 ms` on/off) |
| **Online, waiting for HA sync (`!haSynced`)** | Solid **White** at `LED_PWM_STATUS` (default `10%` / `PWM 26`) until first HA light command |
| **Online (`isOn = true`)** | Solid **HA RGB color**, brightness scaled linearly onto `0..LED_PWM_MAX` (min `PWM 1` floor when non-zero) |
| **Online (`isOn = false` after sync)** | **Off (`PWM 0`)** |
| **Factory reset (`BOOT` held `5 s`)** | Flash **Purple** 3x at `LED_PWM_STATUS` (`150 ms` on/off) |

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

## Software & Build (Matter & Firmware Layer)
* **Toolchain:** Arduino IDE 2.x, Espressif `esp32` core v3.x+ (`XIAO_ESP32C6`), Partition Scheme `Huge APP (3MB No OTA/1MB SPIFFS)`, `USB CDC On Boot: "Enabled"` (`115200` baud).
* **Libraries:** Built-in `Matter`, `WiFi`, `WebServer`, `DNSServer`, `Preferences`.
* **Application:** [`sketches/milieuMatter/milieuMatter.ino`](sketches/milieuMatter/milieuMatter.ino) (Matter endpoints, switch/light callbacks, NVS persistence, captive portal, and status LED state machine) with SoftAP settings in [`sketches/milieuMatter/config.h`](sketches/milieuMatter/config.h).

## Acceptance Criteria
- [x] Saves `btn_mask` to NVS when uncommissioned, blinks blue (`10%`), prints Matter pairing details on Serial, and hosts the `"Milieu-Panel-Setup"` (`192.168.4.1`) offline QR code captive portal until commissioned.
- [x] Registers Switches on `EP 1..N` (`"Button i"`) and Lights on `EP N+1..2N` (`"Button i Light"`) with `BridgedDeviceBasicInformation` labels and semantic tags, under Vendor `"Make Rob Make"` / Product `"Milieu Panel"`.
- [x] Automatically clears saved NVS mask and re-scans if remotely decommissioned from the Matter fabric, or on `5 s` `BOOT` hold (`Matter.decommission()`).
- [x] Emits `InitialPress`, `ShortRelease`, `LongPress`, and `LongRelease` switch events.
- [ ] Displays status White (`min(10%, LED_PWM_MAX)`) on boot (while reporting `Off` to Matter for HA reconnect sync), scales HA `0–100%` brightness (`hsv.v`) linearly onto `0..LED_PWM_MAX` (default `10%`, `48%` 3.3V compile-time cap, `100%` with `POWER_RAIL_5V 1`, min `PWM 1` floor when On), and blinks red when offline.
