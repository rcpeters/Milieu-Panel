# Milieu Panel

> **Smart buttons that don't lie to your face.** 🎨🔘
>
> Most buttons blindly flip an LED when you press them and hope for the best. **Milieu Panel** decouples the color from the switch event so that Home Assistant can automate button colors to show what your smart home is *actually* doing in real time. An example flow you can program into home assistant is:
>
> 🔴 **Off** → *click!* → 🔵 **Working on it…** → 🟢 **Nailed it!** *(or 🔴 **Plot twist: it failed**)*
>
> Bonus superpower: if another Home Assistant automation, voice assistant, or rogue family member toggles a device from across the house, the button colors automatically update to match.

[<img src="docs/img/two_buttons_sample.jpg" alt="Alt text" width="270" height="480">](https://www.youtube.com/shorts/2WVCOeiRdfo)
[View short video](https://www.youtube.com/shorts/2WVCOeiRdfo)

## Supported Events

Works with **ESPHome** or **Matter-over-Wi-Fi** in Home Assistant. Each button exposes a `Button i` (`event`) entity and a `Button i Light` (`light`, On/Off + RGB + brightness) entity.

| Event | Trigger |
|-------|---------|
| `initial_press` | Button pressed (`50 ms` debounce) |
| `short_release` | Released before `1 s` |
| `long_press` | Held for `≥ 1 s` |
| `long_release` | Released after a long press |

---

## Hardware

### BOM (Basic — 3.3V)
- **Microcontroller:** [Seeed Studio XIAO ESP32C6](https://www.seeedstudio.com/Seeed-Studio-XIAO-ESP32C6-p-5884.html)
- **Buttons (1–8):** [DFRobot Gravity I2C RGB LED Button (DFR0991)](https://www.dfrobot.com/product-2634.html)
- **Fasteners:** 4× M3×6 screws per button

### BOM (Optional 5V Upgrade)
- **I2C Level Shifter (1×):** [SparkFun Logic Level Converter - BSS138 (BOB-12009)](https://www.sparkfun.com/products/12009)
- **Decoupling Capacitor (1×):** `100 µF–470 µF` *(recommended)*
- **Schottky Blocking Diode (1×):** `1N5819` / `SS14` *(external 5V PSU only)*

### Wiring
- **3.3V (default, capped at 48% brightness):** [HARDWARE_SPECIFICATION.md](HARDWARE_SPECIFICATION.md)
- **5V Upgrade (up to 100% brightness):** [5V_UPGRADE_README.md](5V_UPGRADE_README.md)

---

## 3D Printing & Assembly

See [docs/enclosure/README.md](docs/enclosure/README.md) for the complete enclosure hardware list, printable CAD files, and step-by-step assembly instructions:

---

## Firmware Setup & Operation

See [FIRMWARE.md](FIRMWARE.md) for flashing, provisioning, LED status codes, and factory reset instructions for both supported firmware stacks.

---

## Firmware Development Requirments

Requirments used to build the firmware:

- [ESPHOME_REQUIREMENTS.md](ESPHOME_REQUIREMENTS.md) — ESPHome specification
- [MATTER_REQUIREMENTS.md](MATTER_REQUIREMENTS.md) — Matter specification

We'll want to make sure to keep the requirments updated and always synced with any software changes.