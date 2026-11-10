# Milieu Panel

Matter-over-Wi-Fi multi-button RGB controller for Home Assistant, built on the [Seeed Studio XIAO ESP32C6](https://www.seeedstudio.com/Seeed-Studio-XIAO-ESP32C6-p-5884.html) and supporting **1 to 8 [DFRobot Gravity I2C RGB LED Buttons (DFR0991)](https://www.dfrobot.com/product-2634.html)**.

Connected buttons are auto-discovered over I2C (`0x23`–`0x2A`) and exposed to Home Assistant as two labeled Matter endpoints per button:
- **`Button 1`–`Button 8` (`event` entity):** Emits `initial_press`, `short_release`, `long_press`, and `long_release` events.
- **`Button 1 Light`–`Button 8 Light` (`light` entity):** Controls each button's On/Off state and RGB color. Brightness is fixed in firmware at **`10%`** (`PWM 26`) to stay within the XIAO ESP32C6 `3V3` power budget.

See [REQUIREMENTS.md](REQUIREMENTS.md) for the full specification and power calculations.

---

## Hardware & Wiring

Connect 1 to 8 button modules in parallel on the 3.3V I2C bus (`100 kHz`; onboard pull-ups are built into each button module, so no external pull-ups or level shifters are needed).

| Signal | XIAO ESP32C6    | Button Module (`0x23`–`0x2A`) | Gravity Wire |
|--------|-----------------|-------------------------------|--------------|
| SDA    | `D4` (`GPIO22`) | `D` (SDA)                     | Green        |
| SCL    | `D5` (`GPIO23`) | `C` (SCL)                     | Blue         |
| 3.3V   | `3V3`           | `+` (VCC)                     | Red          |
| GND    | `GND`           | `-` (GND)                     | Black        |

### DIP Switch Addresses
Set each button to a unique I2C address before powering on:

| Button | Address | DIP (`A2 A1 A0`) | Button | Address | DIP (`A2 A1 A0`) |
|--------|---------|------------------|--------|---------|------------------|
| **1**  | `0x23`  | `0 0 0`          | **5**  | `0x27`  | `1 0 0`          |
| **2**  | `0x24`  | `0 0 1`          | **6**  | `0x28`  | `1 0 1`          |
| **3**  | `0x25`  | `0 1 0`          | **7**  | `0x29`  | `1 1 0`          |
| **4**  | `0x26`  | `0 1 1`          | **8**  | `0x2A`  | `1 1 1`          |

---

## Build & Flash (Arduino IDE)

1. **ESP32 Core (v3.x+):** In **Tools → Board → Boards Manager**, install **`esp32` by Espressif Systems** (`v3.x` or newer).
2. **Library:** In **Tools → Manage Libraries**, install **`DFRobot_RGBButton`**.
3. **Board Settings:** Open [`Milieu-Panel.ino`](Milieu-Panel.ino) and set:
   - **Board:** `XIAO_ESP32C6`
   - **USB CDC On Boot:** `"Enabled"`
   - **Partition Scheme:** `"Huge APP (3MB No OTA/1MB SPIFFS)"`
4. **Upload:** Click **Upload** and open **Serial Monitor** at **`115200` baud**.

---

## Commissioning & Operation

1. **Pair with Home Assistant:** On an uncommissioned boot, connected buttons blink **Blue (`10%`)**:
   - **Via Temporary Wi-Fi Portal (no USB Serial needed):** Connect a phone or laptop to the **`Milieu-Panel-Setup`** Wi-Fi network and open **`http://192.168.4.1`** (or use the captive portal popup) to view the scannable **Matter QR Code**, one-tap **Manual Pairing Code**, and discovered buttons. The SoftAP shuts down automatically as soon as commissioning completes.
   - **Via USB Serial Monitor (`115200` baud):** Prints the **Manual Pairing Code**, **QR Payload (`MT:...`)**, and **QR Code URL**.
   - In the Home Assistant Companion App, go to **Settings → Devices & services → Matter → Add device** to pair.
2. **LED Status Reference:**
   - **Blinking Blue (`10%`):** Uncommissioned / ready to pair
   - **Blinking Red (`10%`):** Wi-Fi or Matter disconnected
   - **Solid White (`10%`):** Online, waiting for initial color sync from Home Assistant (reports `Off` to Matter on boot so HA automations can trigger on reconnect)
   - **Solid RGB (`10%`):** Online and synced with Home Assistant
3. **Add/Remove Buttons (Factory Reset):** Hold the XIAO's onboard **`BOOT` button (`GPIO9`) for 5 seconds** until all buttons flash **Purple 3x** (or remove the device in Home Assistant and reboot). The board will clear its saved button mask and re-scan the I2C bus.
