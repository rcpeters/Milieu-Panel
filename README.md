# Milieu Panel

Matter-over-Wi-Fi multi-button RGB controller for Home Assistant, built on the [Seeed Studio XIAO ESP32C6](https://www.seeedstudio.com/Seeed-Studio-XIAO-ESP32C6-p-5884.html?srsltid=AYCX9vJlRaSb-U1HvQprHTlOPNjOPJUSlCkTo8RSpt2fEll0ksC4HNig) supporting **1 to 8 [DFRobot Gravity I2C RGB LED Button Modules (DFR0991)**](https://www.dfrobot.com/product-2634.html?srsltid=AYCX9vJISmFw0WdmE3JjDOWw4sSpV45JZOlOm4r8jJHsSLDiXGs4eMlx).

Connected buttons are auto-discovered over I2C (`0x23`–`0x2A`) and exposed to Home Assistant as two Matter endpoints per button (up to 16 endpoints total):
- **Matter Generic Switch (`event` entity):** Emits `initial_press`, `short_release`, `long_press`, and `long_release` events so Home Assistant knows both the press type and button hold duration.
- **Matter Color Light (`light` entity):** Exposes each button's onboard RGB LED so Home Assistant automations can control its On/Off state and RGB color (firmware automatically keeps LEDs at **10% brightness** when idle and boosts to **40% brightness** while pressed).

See [REQUIREMENTS.md](REQUIREMENTS.md) for the full design specification.

---

## Use Cases

- **Smart Outlet / Appliance Toggle (1–2 Buttons):** Short press turns a Home Assistant switch on (`Green` LED) and long press turns it off (`Red` LED). Because Home Assistant controls the LED color, the button always reflects the true state even when toggled from the app or a schedule.
- **Scene & Hold-Duration Timer (4 Buttons):** Tap a button to activate a room scene (e.g., `Warm Amber` for Relax, `Blue` for Movie, `Cyan` for Focus), or hold it down so Home Assistant uses the elapsed hold time (`initial_press` to `long_release`) to set a proportional sleep timer or fan speed.
- **8-Zone Home Status & Control Deck (8 Buttons):** Build a desk or entryway status panel for garage doors, locks, alarm state, and laundry alerts—each button's `10%` ambient RGB color shows live status at a glance (`Green` = secure, `Yellow` = running, `Red` = open/alert) and boosts to `40%` when pressed to trigger or acknowledge an action.

---

## Hardware & Wiring (3.3V Direct I2C)

All button modules (1 to 8) share the same 4-wire I2C bus in parallel (`100 kHz` clock; each module has built-in pull-up resistors, so no external pull-ups are needed) and draw power from the XIAO ESP32C6's `3V3` pin (3.3V logic, no level shifter required). The firmware also initializes the XIAO ESP32C6 RF switch (`GPIO3 = LOW`, `GPIO14 = LOW`) to select the built-in ceramic antenna by default.

| Signal | Seeed Studio XIAO ESP32C6 | Each Button Module (`0x23`–`0x2A`) |
|--------|---------------------------|------------------------------------|
| SDA    | `D4` (`GPIO22`)           | `D` (SDA)                          |
| SCL    | `D5` (`GPIO23`)           | `C` (SCL)                          |
| 3.3V   | `3V3`                     | `+` (VCC)                          |
| GND    | `GND`                     | `-` (GND)                          |

### DIP Switch I2C Addresses
Give each button module a unique address using the 3-bit DIP switch (`A2`, `A1`, `A0`) on the back before powering on:

| Button   | I2C Address | DIP Switch (`A2 A1 A0`) |
|----------|-------------|-------------------------|
| Button 1 | `0x23`      | `0 0 0`                 |
| Button 2 | `0x24`      | `0 0 1`                 |
| Button 3 | `0x25`      | `0 1 0`                 |
| Button 4 | `0x26`      | `0 1 1`                 |
| Button 5 | `0x27`      | `1 0 0`                 |
| Button 6 | `0x28`      | `1 0 1`                 |
| Button 7 | `0x29`      | `1 1 0`                 |
| Button 8 | `0x2A`      | `1 1 1`                 |

### 3.3V Peripheral Power & Max Brightness (8 Buttons White)
* **XIAO ESP32C6 3.3V LDO Rating:** `700 mA` total shared between the ESP32-C6 SoC and the `3V3` pin.
* **ESP32-C6 Peak Wi-Fi TX Draw:** `354 mA` (802.11b @ 21 dBm), leaving `346 mA` absolute peak headroom (`250 mA` recommended continuous peripheral budget with thermal/RF margin; `200 mA` conservative).
* **8x Button Modules (`DFR0991`) Draw:** `~2 mA` MCU quiescent per button (`16 mA` for 8 buttons), plus up to `60 mA` worst-case (`20 mA × 3` channels) or `~35 mA` typical at 3.3V for 100% white (`[255, 255, 255]`).
* **Max Safe Brightness for 8x Full White (`250 mA` Budget):**
  $$\text{Max LED Current / Button} = \frac{250\text{ mA} - 16\text{ mA}}{8} = 29.25\text{ mA} \implies \text{Max PWM} = \left\lfloor 255 \times \frac{29.25\text{ mA}}{60\text{ mA}} \right\rfloor = \mathbf{124\text{ / }255\text{ (48\%)}}$$
* **Firmware Operating Levels (`10%` Idle / `40%` Pressed):**
  * **Idle (`10%`, `PWM 26`):** `16 mA` MCUs + $8 \times 6.0\text{ mA} = \mathbf{64\text{ mA}}$ worst-case (`44 mA` typical) when all 8 buttons are white.
  * **Pressed (`40%`, `PWM 102`):** `16 mA` MCUs + $8 \times 24.0\text{ mA} = \mathbf{208\text{ mA}}$ worst-case (`128 mA` typical) even if all 8 buttons are held down simultaneously in white — safely under the `250 mA` peripheral budget.

---

## Firmware Build & Flash (Arduino IDE)

1. **Install the ESP32 Board Package (v3.x+):**
   * In Arduino IDE, open **File → Preferences** and add the official Espressif Boards Manager URL if not already present:
     ```text
     https://espressif.github.io/arduino-esp32/package_esp32_index.json
     ```
   * Open **Tools → Board → Boards Manager**, search for **`esp32`** by **Espressif Systems**, and install **version 3.x or newer** (required for `ESP32-C6` and `Matter`).

2. **Install the `DFRobot_RGBButton` Library:**
   * Open **Tools → Manage Libraries...** (Library Manager), search for **`DFRobot_RGBButton`**, and click **Install** (or download and install [DFRobot_RGBButton from GitHub](https://github.com/DFRobot/DFRobot_RGBButton) via **Sketch → Include Library → Add .ZIP Library...**).

3. **Open the Sketch & Configure Board Settings:**
   * Open [`Milieu-Panel.ino`](Milieu-Panel.ino) in Arduino IDE and configure the **Tools** menu as follows:
     * **Board:** `XIAO_ESP32C6` (`Tools → Board → esp32 → XIAO_ESP32C6`)
     * **Port:** Select your XIAO ESP32C6 USB serial port
     * **USB CDC On Boot:** `"Enabled"` (required for `Serial` logs over USB-C)
     * **Partition Scheme:** `"Huge APP (3MB No OTA/1MB SPIFFS)"` (required so the Matter stack fits in flash)

4. **Upload & Monitor:**
   * Click **Upload** (`Ctrl+U` / `Cmd+U`) to build and flash the firmware.
   * Open **Tools → Serial Monitor** and set baud rate to **`115200`** to view discovered button addresses and the Matter pairing code.

---

## Matter Commissioning & Home Assistant Setup

1. Open the Serial Monitor (`115200` baud) after flashing. On an uncommissioned boot, the firmware scans `0x23`–`0x2A` to auto-discover connected buttons, blinks them blue (`10%`), and prints the **Matter manual pairing code** and **QR code URL**.
2. In the **Home Assistant Companion App**, go to **Settings → Devices & services → Matter → Add device** and scan the QR code (or enter the manual pairing code).
3. Once commissioned, the discovered button list is locked in NVS and Home Assistant exposes one **Event** entity (`initial_press`, `short_release`, `long_press`, `long_release`) and one **Light** entity per button.
4. **Boot / Reconnect Color Sync:** On boot, each button LED displays `10%` White locally while reporting `Off` to Matter. In your Home Assistant automation, add a trigger when the button `light` entity transitions `from: unavailable` (or to `off` on boot) so Home Assistant automatically re-sends the current live state and RGB color whenever the board reboots.
5. **Factory Reset / Add or Remove Buttons:** Hold the onboard `BOOT` button (`GPIO9`) on the XIAO ESP32C6 for 5 seconds until all connected buttons flash **purple 3 times** (`40%`), erasing Matter credentials and the saved button list to re-scan the I2C bus on reboot.

