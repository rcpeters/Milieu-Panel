#pragma once

#include <Arduino.h>
#include <DFRobot_RGBButton.h>
#include <Matter.h>

// Hardware Pins (Seeed Studio XIAO ESP32C6)
constexpr uint8_t PIN_I2C_SDA = 22;        // D4 (GPIO22)
constexpr uint8_t PIN_I2C_SCL = 23;        // D5 (GPIO23)
constexpr uint32_t I2C_FREQ_HZ = 100000UL; // 100 kHz I2C

constexpr uint8_t PIN_RF_SWITCH_EN = 3;    // GPIO3 (LOW enables FM8625H RF switch)
constexpr uint8_t PIN_RF_ANT_SELECT = 14;  // GPIO14 (LOW = internal ceramic, HIGH = external U.FL)
constexpr uint8_t PIN_BOOT_BUTTON = 9;     // GPIO9 (active-LOW BOOT button)

#ifndef USE_EXTERNAL_ANTENNA
#define USE_EXTERNAL_ANTENNA 0
#endif

// I2C Button Address Range (DFR0991 DIP Switch A2..A0 = 000..111)
constexpr uint8_t BUTTON_ADDR_MIN = 0x23;
constexpr uint8_t BUTTON_ADDR_MAX = 0x2A;
constexpr uint8_t MAX_BUTTONS = BUTTON_ADDR_MAX - BUTTON_ADDR_MIN + 1; // 8

// Power Rail & Compile-Time Max Brightness Cap (PWM 0..255)
// Set POWER_RAIL_5V to 1 ONLY when buttons are powered at 5V through an I2C level shifter (see 5V_UPGRADE_README.md).
#ifndef POWER_RAIL_5V
#define POWER_RAIL_5V 0
#endif

#ifndef MAX_BRIGHTNESS_PCT
#define MAX_BRIGHTNESS_PCT 10
#endif

constexpr uint8_t MAX_BRIGHTNESS_LIMIT_PCT = POWER_RAIL_5V ? 100 : 48;
static_assert(MAX_BRIGHTNESS_PCT >= 1 && MAX_BRIGHTNESS_PCT <= 100,
              "MAX_BRIGHTNESS_PCT must be between 1 and 100");
static_assert(MAX_BRIGHTNESS_PCT <= MAX_BRIGHTNESS_LIMIT_PCT,
              "MAX_BRIGHTNESS_PCT exceeds 48% 3.3V power budget; set POWER_RAIL_5V 1 only after 5V upgrade");

constexpr uint8_t LED_PWM_OFF = 0;
constexpr uint8_t LED_PWM_MAX =
    (uint8_t)(((uint16_t)MAX_BRIGHTNESS_PCT * 255U + 50U) / 100U); // 10% -> 26, 48% -> 122, 100% -> 255
constexpr uint8_t LED_PWM_STATUS =
    (LED_PWM_MAX < 26U) ? LED_PWM_MAX : 26U; // 10% (PWM 26), or lower if MAX_BRIGHTNESS_PCT < 10%

// Timing Constants (ms)
constexpr uint32_t BUTTON_POLL_INTERVAL_MS = 20;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 50;
constexpr uint32_t BUTTON_LONG_PRESS_MS = 1000;
constexpr uint32_t I2C_RETRY_INTERVAL_MS = 5000;
constexpr uint32_t STATUS_BLINK_INTERVAL_MS = 500;
constexpr uint32_t FACTORY_RESET_HOLD_MS = 5000;
constexpr uint32_t FACTORY_RESET_BLINK_MS = 150;
constexpr uint8_t FACTORY_RESET_BLINK_COUNT = 3;

// NVS Keys
constexpr const char *NVS_NAMESPACE = "milieu";
constexpr const char *NVS_KEY_BTN_MASK = "btn_mask";
constexpr const char *NVS_KEY_COMMISSIONED = "commissioned";

// Temporary Commissioning SoftAP & Captive Portal (active only while uncommissioned)
constexpr const char *SETUP_AP_SSID = "Milieu-Panel-Setup";
constexpr uint8_t SETUP_DNS_PORT = 53;
constexpr uint16_t SETUP_HTTP_PORT = 80;

enum class NodeState : uint8_t { UNCOMMISSIONED, OFFLINE, ONLINE };

struct ButtonSlot {
  uint8_t number = 0;  // 1..N
  uint8_t addr = 0;    // 0x23..0x2A
  MatterGenericSwitch sw;
  MatterColorLight light;

  bool connected = false;
  uint32_t lastRetryMs = 0;

  bool rawPressed = false;
  bool pressed = false;
  bool longFired = false;
  uint32_t debounceMs = 0;
  uint32_t pressStartMs = 0;

  bool haSynced = false;
  bool isOn = false;
  espRgbColor_t rgb = {255, 255, 255};
  uint8_t brightness = 254; // Matter LevelControl 0..254

  uint8_t lastR = 255, lastG = 255, lastB = 255;
  bool hasColor = false;
};
