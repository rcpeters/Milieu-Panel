#pragma once

#include <Arduino.h>
#include <DFRobot_RGBButton.h>
#include <Matter.h>

// -----------------------------------------------------------------------------
// Hardware Pin Definitions (Seeed Studio XIAO ESP32C6)
// -----------------------------------------------------------------------------
// Direct 3.3V I2C bus connected in parallel to 1..8 DFR0991 RGB buttons.
constexpr uint8_t PIN_I2C_SDA = 22;        // D4 (GPIO22)
constexpr uint8_t PIN_I2C_SCL = 23;        // D5 (GPIO23)
constexpr uint32_t I2C_FREQ_HZ = 100000UL; // 100 kHz standard mode (no external pull-ups)

// Onboard FM8625H RF antenna switch control pins.
// GPIO3 = LOW enables the RF switch; GPIO14 = LOW selects the built-in ceramic
// antenna (or HIGH for an external U.FL antenna when USE_EXTERNAL_ANTENNA=1).
constexpr uint8_t PIN_RF_SWITCH_EN = 3;   // GPIO3
constexpr uint8_t PIN_RF_ANT_SELECT = 14; // GPIO14

// Onboard BOOT button used to trigger a 5-second factory reset.
constexpr uint8_t PIN_BOOT_BUTTON = 9; // GPIO9 (active-LOW)

// Set to 1 if using an external U.FL antenna on the XIAO ESP32C6; 0 for built-in ceramic antenna.
#ifndef USE_EXTERNAL_ANTENNA
#define USE_EXTERNAL_ANTENNA 0
#endif

// -----------------------------------------------------------------------------
// I2C Button Address Range (DFRobot DFR0991 DIP Switch A2..A0 = 000..111)
// -----------------------------------------------------------------------------
constexpr uint8_t BUTTON_ADDR_MIN = 0x23;
constexpr uint8_t BUTTON_ADDR_MAX = 0x2A;
constexpr uint8_t MAX_BUTTONS = (BUTTON_ADDR_MAX - BUTTON_ADDR_MIN + 1); // 8 buttons

// -----------------------------------------------------------------------------
// 3.3V Rail Power-Safe Brightness Levels (PWM 0..255)
// -----------------------------------------------------------------------------
// Idle (10% = PWM 26): <= 64 mA worst-case (44 mA typical) for 8 white buttons.
// Pressed (40% = PWM 102): <= 208 mA worst-case (128 mA typical) for 8 white buttons.
constexpr uint8_t LED_PWM_OFF = 0;
constexpr uint8_t LED_PWM_IDLE = 26;     // 10% brightness
constexpr uint8_t LED_PWM_PRESSED = 102; // 40% brightness

// -----------------------------------------------------------------------------
// Timing Constants (Milliseconds)
// -----------------------------------------------------------------------------
constexpr uint32_t BUTTON_POLL_INTERVAL_MS = 20;    // Poll active buttons every 20 ms
constexpr uint32_t BUTTON_DEBOUNCE_MS = 50;         // 50 ms push/release debounce
constexpr uint32_t BUTTON_LONG_PRESS_MS = 1000;     // 1000 ms long-press threshold
constexpr uint32_t I2C_RETRY_INTERVAL_MS = 5000;    // 5 s scan / reconnect retry interval
constexpr uint32_t STATUS_BLINK_INTERVAL_MS = 500;  // 1 Hz blink (500 ms on / 500 ms off)
constexpr uint32_t FACTORY_RESET_HOLD_MS = 5000;    // 5 s BOOT hold to factory reset
constexpr uint32_t FACTORY_RESET_BLINK_MS = 150;    // 150 ms on / 150 ms off purple flash
constexpr uint8_t FACTORY_RESET_BLINK_COUNT = 3;    // Flash purple 3 times on reset

// -----------------------------------------------------------------------------
// NVS Persistence Keys
// -----------------------------------------------------------------------------
constexpr const char *NVS_NAMESPACE = "milieu";
constexpr const char *NVS_KEY_BTN_MASK = "btn_mask";
constexpr const char *NVS_KEY_COMMISSIONED = "commissioned";

// -----------------------------------------------------------------------------
// Firmware State Types (Defined in header for Arduino .ino prototype generation)
// -----------------------------------------------------------------------------
enum class NodeSystemState : uint8_t {
  UNCOMMISSIONED,
  OFFLINE,
  ONLINE,
};

struct ButtonSlot {
  uint8_t buttonNumber = 0; // 1..N (ordered by I2C address 0x23..0x2A)
  uint8_t i2cAddr = 0;      // 0x23..0x2A
  DFRobot_RGBButton *driver = nullptr;

  MatterGenericSwitch switchEndpoint; // Endpoint 2i - 1
  MatterColorLight lightEndpoint;     // Endpoint 2i

  // I2C connectivity & reconnection state
  bool connected = false;
  uint32_t lastReconnectAttemptMs = 0;

  // Button polling, 50 ms debounce & 1000 ms long-press state
  bool rawPressed = false;
  bool debouncedPressed = false;
  bool longPressFired = false;
  uint32_t rawChangeTimeMs = 0;
  uint32_t pressStartTimeMs = 0;

  // MatterColorLight state (guarded by g_stateMux across Matter/loop tasks)
  bool haSynced = false;                   // False until first HA MatterColorLight update
  bool isOn = false;                       // Commanded On/Off state from HA
  espRgbColor_t baseRgb = {255, 255, 255}; // Full-scale [0..255] RGB from HA Hue/Sat
  bool ledDirty = true;

  // Cached hardware LED state to avoid redundant I2C writes
  bool hasAppliedColor = false;
  uint8_t appliedR = 0;
  uint8_t appliedG = 0;
  uint8_t appliedB = 0;
};
