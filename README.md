# Milieu Panel

> **Smart buttons that don't lie to your face.** 🎨🔘
>
> Most buttons blindly flip an LED when you press them and hope for the best. **Milieu Panel** decouples the color from the switch event so your buttons show what your smart home is *actually* doing in real time:
>
> 🔴 **Off** → *click!* → 🔵 **Working on it…** → 🟢 **Nailed it!** *(or 🔴 **Plot twist: it failed**)*
>
> Bonus superpower: if another Home Assistant automation, voice assistant, or rogue family member toggles a device from across the house, the button colors automatically update to match.

[<img src="docs/img/two_buttons_sample.jpg" alt="Alt text" width="270" height="480">](https://www.youtube.com/shorts/2WVCOeiRdfo)

## Tech Details:

Matter-over-Wi-Fi multi-button RGB controller for Home Assistant, built on the [Seeed Studio XIAO ESP32C6](https://www.seeedstudio.com/Seeed-Studio-XIAO-ESP32C6-p-5884.html) and supporting **1 to 8 [DFRobot Gravity I2C RGB LED Buttons (DFR0991)](https://www.dfrobot.com/product-2634.html)**.

Connected buttons are auto-discovered over I2C (`0x23`–`0x2A`) and exposed to Home Assistant as two labeled Matter endpoints per button:
- **`Button 1`–`Button 8` (`event` entity):** Emits `initial_press`, `short_release`, `long_press`, and `long_release` events.
- **`Button 1 Light`–`Button 8 Light` (`light` entity):** Controls each button's On/Off state and RGB color. Brightness is fixed in firmware at **`10%`** (`PWM 26`) to stay within the XIAO ESP32C6 `3V3` power budget.

See [HARDWARE_SPECIFICATION.md](HARDWARE_SPECIFICATION.md) for wiring, I2C bus details, and 3.3V power calculations, and [MATTER_REQUIREMENTS.md](MATTER_REQUIREMENTS.md) for Matter endpoints, commissioning, and Home Assistant behavior.

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
3. **Add/Remove Buttons (Factory Reset):** Hold the XIAO's onboard **`BOOT` button (`GPIO9`)**—or short the **`BOOT` (`GPIO9`) and `GND` pads/pins underneath the board**—for **5 seconds** until all buttons flash **Purple 3x** (or remove the device in Home Assistant and reboot). The board will clear its saved button mask and re-scan the I2C bus.
