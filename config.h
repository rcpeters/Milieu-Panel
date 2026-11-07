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

// 3.3V Rail Power-Safe Brightness Levels (PWM 0..255)
constexpr uint8_t LED_PWM_OFF = 0;
constexpr uint8_t LED_PWM_IDLE = 26;     // 10% brightness
constexpr uint8_t LED_PWM_PRESSED = 102; // 40% brightness

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

  uint8_t lastR = 255, lastG = 255, lastB = 255;
  bool hasColor = false;
};
