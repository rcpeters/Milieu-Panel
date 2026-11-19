# ESPHOME REQUIREMENTS

ESPHome-over-Wi-Fi node architecture, external component design, wired provisioning/security model, and Home Assistant behavior for **Milieu Panel**.

For wiring, DIP switch addresses, I2C bus details, and the 3.3V power budget, see [HARDWARE_SPECIFICATION.md](HARDWARE_SPECIFICATION.md). For the Matter implementation, see [MATTER_REQUIREMENTS.md](MATTER_REQUIREMENTS.md).

## Scope
* **Native ESPHome device (1–8 buttons):** Auto-discovers 1 to 8 physical I2C RGB buttons (`0x23`–`0x2A`) and exposes each as an ESPHome `event` entity (`Button i`) and `light` entity (`Button i Light`).
* **Zero-YAML-edit friendly or compile-time configured:** Supports flashing a single prebuilt binary (with runtime I2C auto-discovery + USB-C `improv_serial` + ESPHome Builder `dashboard_import` adoption) as well as traditional `secrets.yaml` YAML builds.
* **Snooping-resistant provisioning by design:** Avoids open/HTTP fallback Wi-Fi portals (`captive_portal`) and unencrypted BLE Wi-Fi provisioning (`esp32_improv`) so Wi-Fi credentials are never broadcast in plaintext over RF.
* **Out of scope:** Matter/Thread transport, Captive Portal AP, BLE Improv, enclosure, battery power.

## ESPHome Node & Entities
* **Node Metadata:** `name: milieu-panel`, `friendly_name: "Milieu Panel"` (`name_add_mac_suffix: true` on factory/adoptable builds), `platform: esp32`, `board: seeed_xiao_esp32c6`, `framework: esp-idf`.
* **Entity Mapping:** For each discovered button `i` (`1..N`, ordered by I2C address `0x23`–`0x2A`), exposes two Home Assistant entities with matching names and parity with the Matter implementation:

| Button Slot | ESPHome Component | Entity Name        | Home Assistant Entity |
|-------------|-------------------|--------------------|-----------------------|
| `i` (`1..N`) | `event`           | `"Button i"`       | `event`               |
| `i` (`1..N`) | `light` (`RGB`)   | `"Button i Light"` | `light`               |

* **Dynamic Discovery & NVS Locking:** On first boot (no `btn_mask` in NVS namespace `"milieu"`), the component scans `0x23`–`0x2A`, retrying every `5 s` until ≥ 1 button responds, and saves the 8-bit `btn_mask` to NVS. On subsequent boots, it loads `btn_mask` from NVS before the Native API connects so only discovered button slots are exposed (unpopulated slots remain hidden/disabled) and entity IDs stay fixed across reboots, with optional YAML overrides.
* **Hot-Plug Recovery:** If a paired button disconnects from I2C at runtime, the component marks it disconnected without blocking other slots and probes it every `5 s` to reconnect and restore its LED state.

## Behavior

### Button Events (`event`)
Polled every `20 ms` with `50 ms` debounce and `1000 ms` long-press threshold (active only when Wi-Fi and the Home Assistant Native API are connected), registering `event_types: [initial_press, short_release, long_press, long_release]`:
* **Press (`50 ms`):** Triggers `initial_press`.
* **Release `< 1000 ms`:** Triggers `short_release`.
* **Hold `≥ 1000 ms`:** Triggers `long_press` at `1000 ms`, then `long_release` on release.

### RGB LEDs (`light` & Status)
Each `Button i Light` uses `ColorMode::RGB` with `restore_mode: ALWAYS_OFF`.
* **Power-Safe Brightness Scaling:** Home Assistant's `0–100%` brightness slider is scaled linearly onto `0..max_pwm`, where `max_brightness` defaults to **`10%` (`PWM 26`)** and is validated at compile time to **`≤ 48%`** on `3.3V` (`power_rail_5v: false`), or up to **`100%`** (`PWM 255`) when `power_rail_5v: true` is explicitly set for a 5V hardware build (see [5V_UPGRADE_README.md](5V_UPGRADE_README.md)).
* **Minimum Visible Floor:** When a light is `On` with a non-zero color/level and linear scaling rounds all RGB channels to `0`, the strongest channel(s) are floored to `PWM 1` so `1%` brightness stays visibly lit.
* **Status LEDs & Boot Sync Handshake (`!haSynced`):** Status states (Blue/Red blink, Solid White pre-sync, Amber OTA, Purple reset) stay capped at `min(10% / PWM 26, max_pwm)`. On boot/reconnect before Home Assistant sends a light update, each button displays solid **White** while reporting `Off` to Home Assistant so HA reconnect automations trigger cleanly.

| State | LED Behavior |
|-------|--------------|
| **Unprovisioned (no Wi-Fi, waiting for `improv_serial`)** | Blink **Blue** at `status_pwm` (default `10%` / `PWM 26`), `1 Hz` (`500 ms` on/off) |
| **Wi-Fi or HA Native API offline** | Blink **Red** at `status_pwm` (default `10%` / `PWM 26`), `1 Hz` (`500 ms` on/off) |
| **Online, waiting for HA sync (`!haSynced`)** | Solid **White** at `status_pwm` (default `10%` / `PWM 26`) until first HA light command |
| **Online (`isOn = true`)** | Solid **HA RGB color**, brightness scaled linearly onto `0..max_pwm` (min `PWM 1` floor when non-zero) |
| **Online (`isOn = false` after sync)** | **Off (`PWM 0`)** |
| **OTA update in progress** | **Amber** at `status_pwm` (default `10%` / `PWM 26`) |
| **Factory reset (`BOOT` held `5 s`)** | Flash **Purple** 3x at `status_pwm` (`150 ms` on/off) |

## Provisioning, Security & OTA
* **Wired Wi-Fi Setup (`improv_serial`):** Factory/prebuilt firmware uses USB-C Improv Serial so Wi-Fi credentials are sent strictly over the USB cable rather than over an unencrypted SoftAP or BLE link.
* **Compile-Time Credentials (Optional):** Users compiling directly from YAML can provide Wi-Fi credentials, a unique Native API Noise encryption key, and an OTA password in `esphome/secrets.yaml` for encrypted operation from first flash.
* **Encrypted Native API (`api:` + `dashboard_import:`):** Uses the ESPHome Noise protocol (`Noise_NNpsk0_25519_ChaChaPoly_SHA256`) for authenticated, encrypted communication with Home Assistant. Prebuilt factory devices can be adopted in ESPHome Builder via `dashboard_import` to generate and push a unique per-device Noise key and OTA password.
* **OTA Updates (`ota` `platform: esphome`):** Supports wireless firmware updates with password authentication and an Amber LED progress indicator.
* **Full Factory Reset (`5 s` `BOOT` Hold):** Holding the onboard `BOOT` (`GPIO9`) button—or shorting the `BOOT` (`GPIO9`) and `GND` pads/pins underneath the board—for `5 s` flashes all connected buttons **Purple 3x**, clears the saved `btn_mask` in NVS **and** wipes stored Wi-Fi/Improv credentials, then restarts to re-scan the I2C bus.

## Software & Architecture
* **Framework & Target:** ESPHome with `esp-idf` framework targeting `seeed_xiao_esp32c6` (`GPIO22` SDA, `GPIO23` SCL at `100 kHz`, `GPIO3 = LOW` RF switch enable, `GPIO14 = LOW` internal ceramic antenna by default with YAML option for external U.FL).
* **Repository Layout:**
  * [`esphome/milieu-panel.yaml`](esphome/milieu-panel.yaml) — Factory/adoptable ESPHome configuration (`improv_serial`, `dashboard_import`, `api`, `ota`).
  * [`esphome/milieu-panel-private.yaml`](esphome/milieu-panel-private.yaml) — Optional compile-time encrypted configuration using `secrets.yaml` for Wi-Fi, Noise API key, and OTA password.
  * [`esphome/packages/milieu-panel-base.yaml`](esphome/packages/milieu-panel-base.yaml) — Shared `esp-idf`, `i2c`, `milieu_panel`, `event`, and `light` definitions.
  * [`esphome/secrets.yaml.example`](esphome/secrets.yaml.example) — Template for compile-time Wi-Fi, Noise API key, and OTA password.
  * [`esphome/components/milieu_panel/`](esphome/components/milieu_panel/) — Custom ESPHome external C++ component implementing non-blocking `DFR0991` direct I2C register access, NVS `btn_mask` persistence, `event` + `light` entities, status/OTA LED state machine, and `5 s` `BOOT` reset.

## Acceptance Criteria
- [ ] Compiles under ESPHome (`esp-idf`) for `seeed_xiao_esp32c6`, initializes RF switch (`GPIO3 = LOW`, `GPIO14 = LOW`), and runs `100 kHz` I2C with non-blocking timeouts.
- [ ] Scans `0x23`–`0x2A` when no `btn_mask` is saved (retrying every `5 s` if empty), persists `btn_mask` in NVS, exposes only discovered `Button i` (`event`) and `Button i Light` (`light`) entities, and recovers hot-plugged buttons every `5 s`.
- [ ] Supports USB-C `improv_serial` provisioning and `dashboard_import` adoption (plus optional `secrets.yaml`), without enabling unencrypted `captive_portal` AP or BLE Improv.
- [ ] Emits `initial_press`, `short_release`, `long_press`, and `long_release` via native ESPHome `event` entities (`20 ms` poll, `50 ms` debounce, `1000 ms` hold).
- [ ] Displays status White (`min(10%, max_pwm)`) on boot until first HA light update (while reporting `Off`), scales HA `0–100%` brightness linearly onto `0..max_pwm` (default `10%` / `PWM 26`, `48%` 3.3V cap, `100%` with `power_rail_5v: true`, min `PWM 1` floor when On), blinks Blue/Red for unprovisioned/offline states, and indicates OTA updates in Amber.
- [ ] Flashes Purple 3x and clears both NVS `btn_mask` and stored Wi-Fi/Improv credentials when `BOOT` (`GPIO9`) is held or shorted to `GND` for `5 s`.
