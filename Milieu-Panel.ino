// Milieu Panel - Matter-over-Wi-Fi Multi-Button RGB Controller (1-8 Buttons)
// Target Board: Seeed Studio XIAO ESP32C6 (Arduino-ESP32 v3.x+)
// Required Partition Scheme: Huge APP (3MB No OTA/1MB SPIFFS)

#include <Arduino.h>
#include <DFRobot_RGBButton.h>
#include <Matter.h>
#include <Preferences.h>
#include <WiFi.h>
#include <Wire.h>

#include "config.h"

static ButtonSlot g_buttons[MAX_BUTTONS];
static uint8_t g_buttonCount = 0;
static uint8_t g_buttonMask = 0;

static portMUX_TYPE g_stateMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool g_setupComplete = false;
static bool g_matterStarted = false;
static bool g_nvsCommissioned = false;

static NodeSystemState g_lastSystemState = NodeSystemState::UNCOMMISSIONED;
static uint32_t g_lastPollMs = 0;
static uint32_t g_lastBlinkToggleMs = 0;
static bool g_blinkPhaseOn = true;

// BOOT button (GPIO9) 5-second factory reset state
static bool g_bootButtonPressed = false;
static uint32_t g_bootPressStartMs = 0;

// -----------------------------------------------------------------------------
// Hardware & I2C Helpers
// -----------------------------------------------------------------------------

static void initRfSwitch() {
  pinMode(PIN_RF_SWITCH_EN, OUTPUT);
  digitalWrite(PIN_RF_SWITCH_EN, LOW); // Enable onboard FM8625H RF switch

  pinMode(PIN_RF_ANT_SELECT, OUTPUT);
#if USE_EXTERNAL_ANTENNA
  digitalWrite(PIN_RF_ANT_SELECT, HIGH); // Select external U.FL antenna
#else
  digitalWrite(PIN_RF_ANT_SELECT, LOW);  // Select built-in ceramic antenna (default)
#endif
}

static void initI2CBus() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(I2C_FREQ_HZ);
}

static bool pingI2CAddress(uint8_t addr) {
  Wire.beginTransmission(addr);
  return (Wire.endTransmission() == 0);
}

static bool initButtonDriver(ButtonSlot &slot) {
  if (!pingI2CAddress(slot.i2cAddr)) {
    return false;
  }
  bool ok = slot.driver->begin();
  // DFRobot_RGBButton::begin() invokes _pWire->begin() internally, so restore 100 kHz clock.
  Wire.setClock(I2C_FREQ_HZ);
  return ok;
}

static uint8_t scanI2CButtonMask() {
  uint8_t mask = 0;
  for (uint8_t idx = 0; idx < MAX_BUTTONS; ++idx) {
    uint8_t addr = BUTTON_ADDR_MIN + idx;
    if (pingI2CAddress(addr)) {
      DFRobot_RGBButton tempBtn(&Wire, addr);
      bool ok = tempBtn.begin();
      Wire.setClock(I2C_FREQ_HZ);
      if (ok) {
        mask |= static_cast<uint8_t>(1U << idx);
      }
    }
  }
  return mask;
}

static uint8_t scaleChannel(uint8_t channelValue, uint8_t maxPwm) {
  return static_cast<uint8_t>((static_cast<uint16_t>(channelValue) * static_cast<uint16_t>(maxPwm) + 127U) / 255U);
}

// -----------------------------------------------------------------------------
// NVS Persistence Helpers
// -----------------------------------------------------------------------------

static void loadNvsConfig(uint8_t &outMask, bool &outCommissioned) {
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, true)) {
    outMask = prefs.getUChar(NVS_KEY_BTN_MASK, 0);
    outCommissioned = prefs.getBool(NVS_KEY_COMMISSIONED, false);
    prefs.end();
  } else {
    outMask = 0;
    outCommissioned = false;
  }
}

static void saveNvsButtonMask(uint8_t mask) {
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    prefs.putUChar(NVS_KEY_BTN_MASK, mask);
    prefs.end();
  }
}

static void saveNvsCommissioned(bool commissioned) {
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    prefs.putBool(NVS_KEY_COMMISSIONED, commissioned);
    prefs.end();
  }
  g_nvsCommissioned = commissioned;
}

static void clearNvsConfig() {
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    prefs.clear();
    prefs.end();
  }
  g_nvsCommissioned = false;
}

// -----------------------------------------------------------------------------
// Factory Reset (5 s BOOT Button Hold)
// -----------------------------------------------------------------------------

static void executeFactoryReset() {
  Serial.println("\n[RESET] BOOT held for 5 s -> Triggering Factory Reset!");
  Serial.println("[RESET] Flashing all connected buttons Purple 3x (40% brightness)...");

  for (uint8_t cycle = 0; cycle < FACTORY_RESET_BLINK_COUNT; ++cycle) {
    for (uint8_t addr = BUTTON_ADDR_MIN; addr <= BUTTON_ADDR_MAX; ++addr) {
      if (pingI2CAddress(addr)) {
        DFRobot_RGBButton btn(&Wire, addr);
        btn.setRGBColor(LED_PWM_PRESSED, 0, LED_PWM_PRESSED); // 40% Purple (PWM 102, 0, 102)
      }
    }
    delay(FACTORY_RESET_BLINK_MS);

    for (uint8_t addr = BUTTON_ADDR_MIN; addr <= BUTTON_ADDR_MAX; ++addr) {
      if (pingI2CAddress(addr)) {
        DFRobot_RGBButton btn(&Wire, addr);
        btn.setRGBColor(LED_PWM_OFF, LED_PWM_OFF, LED_PWM_OFF);
      }
    }
    delay(FACTORY_RESET_BLINK_MS);
  }

  Serial.println("[RESET] Clearing saved button mask in NVS and decommissioning Matter node...");
  clearNvsConfig();

  if (g_matterStarted) {
    Matter.decommission();
  }
  ESP.restart();
}

static void checkBootFactoryReset() {
  bool pressed = (digitalRead(PIN_BOOT_BUTTON) == LOW);
  uint32_t now = millis();

  if (pressed) {
    if (!g_bootButtonPressed) {
      g_bootButtonPressed = true;
      g_bootPressStartMs = now;
      Serial.println("[BOOT] BOOT button pressed; hold for 5 s to factory reset...");
    } else if ((now - g_bootPressStartMs) >= FACTORY_RESET_HOLD_MS) {
      executeFactoryReset();
    }
  } else {
    if (g_bootButtonPressed) {
      g_bootButtonPressed = false;
      Serial.println("[BOOT] BOOT button released before 5 s.");
    }
  }
}

// -----------------------------------------------------------------------------
// MatterColorLight Callbacks (Thread-Safe)
// -----------------------------------------------------------------------------

static bool handleLightChange(uint8_t slotIndex, bool newState, espHsvColor_t newColor) {
  if (!g_setupComplete || slotIndex >= g_buttonCount) {
    return true;
  }

  // Normalize Value (brightness) to max (254) so Matter brightness slider is ignored
  // and brightness is controlled strictly by firmware (10% idle / 40% pressed).
  espHsvColor_t fullScaleHsv = {newColor.h, newColor.s, 254};
  espRgbColor_t rgb = espHsvColorToRgbColor(fullScaleHsv);

  ButtonSlot &slot = g_buttons[slotIndex];
  portENTER_CRITICAL(&g_stateMux);
  slot.haSynced = true;
  slot.isOn = newState;
  slot.baseRgb = rgb;
  slot.ledDirty = true;
  portEXIT_CRITICAL(&g_stateMux);

  Serial.printf("[MATTER] Button %u (0x%02X) Light -> %s | RGB(%u, %u, %u) (HSV h=%u s=%u v=%u)\n",
                slot.buttonNumber,
                slot.i2cAddr,
                newState ? "ON" : "OFF",
                rgb.r,
                rgb.g,
                rgb.b,
                newColor.h,
                newColor.s,
                newColor.v);
  return true;
}

using LightCallbackFn = bool (*)(bool, espHsvColor_t);
static const LightCallbackFn kLightCallbacks[MAX_BUTTONS] = {
    [](bool state, espHsvColor_t color) -> bool { return handleLightChange(0, state, color); },
    [](bool state, espHsvColor_t color) -> bool { return handleLightChange(1, state, color); },
    [](bool state, espHsvColor_t color) -> bool { return handleLightChange(2, state, color); },
    [](bool state, espHsvColor_t color) -> bool { return handleLightChange(3, state, color); },
    [](bool state, espHsvColor_t color) -> bool { return handleLightChange(4, state, color); },
    [](bool state, espHsvColor_t color) -> bool { return handleLightChange(5, state, color); },
    [](bool state, espHsvColor_t color) -> bool { return handleLightChange(6, state, color); },
    [](bool state, espHsvColor_t color) -> bool { return handleLightChange(7, state, color); },
};

// -----------------------------------------------------------------------------
// Connectivity & LED State Machine
// -----------------------------------------------------------------------------

static NodeSystemState getCurrentSystemState() {
  if (!Matter.isDeviceCommissioned()) {
    return NodeSystemState::UNCOMMISSIONED;
  }
  if (!Matter.isWiFiConnected() || !Matter.isDeviceConnected()) {
    return NodeSystemState::OFFLINE;
  }
  return NodeSystemState::ONLINE;
}

static void handleButtonDisconnect(ButtonSlot &slot, uint32_t now) {
  if (slot.connected) {
    slot.connected = false;
    slot.lastReconnectAttemptMs = now;
    slot.rawPressed = false;
    slot.debouncedPressed = false;
    slot.longPressFired = false;
    slot.hasAppliedColor = false;
    Serial.printf("[I2C] WARNING: Button %u (0x%02X) disconnected from I2C bus; retrying every 5 s.\n",
                  slot.buttonNumber,
                  slot.i2cAddr);
  }
}

static void applyButtonLedColor(ButtonSlot &slot, uint8_t r, uint8_t g, uint8_t b, uint32_t now) {
  if (!slot.connected) {
    return;
  }
  if (slot.hasAppliedColor && slot.appliedR == r && slot.appliedG == g && slot.appliedB == b) {
    return;
  }
  if (!pingI2CAddress(slot.i2cAddr)) {
    handleButtonDisconnect(slot, now);
    return;
  }

  slot.driver->setRGBColor(r, g, b);
  slot.appliedR = r;
  slot.appliedG = g;
  slot.appliedB = b;
  slot.hasAppliedColor = true;
}

static void computeTargetLedColor(const ButtonSlot &slot,
                                  NodeSystemState sysState,
                                  bool blinkOn,
                                  uint8_t &outR,
                                  uint8_t &outG,
                                  uint8_t &outB) {
  if (sysState == NodeSystemState::UNCOMMISSIONED) {
    // Uncommissioned (pairing mode): Blink Blue at 10% (PWM 26) at 1 Hz
    outR = 0;
    outG = 0;
    outB = blinkOn ? LED_PWM_IDLE : LED_PWM_OFF;
    return;
  }

  if (sysState == NodeSystemState::OFFLINE) {
    // Commissioned, Wi-Fi or Matter offline: Blink Red at 10% (PWM 26) at 1 Hz
    outR = blinkOn ? LED_PWM_IDLE : LED_PWM_OFF;
    outG = 0;
    outB = 0;
    return;
  }

  // NodeSystemState::ONLINE
  bool haSynced = false;
  bool isOn = false;
  espRgbColor_t baseRgb = {255, 255, 255};

  portENTER_CRITICAL(&g_stateMux);
  haSynced = slot.haSynced;
  isOn = slot.isOn;
  baseRgb = slot.baseRgb;
  portEXIT_CRITICAL(&g_stateMux);

  if (slot.debouncedPressed) {
    // Online, button held down (InitialPress to release):
    // Boost to 40% brightness (PWM 102) of active RGB color, or 40% White if Off / uninitialized.
    if (haSynced && isOn) {
      outR = scaleChannel(baseRgb.r, LED_PWM_PRESSED);
      outG = scaleChannel(baseRgb.g, LED_PWM_PRESSED);
      outB = scaleChannel(baseRgb.b, LED_PWM_PRESSED);
    } else {
      outR = LED_PWM_PRESSED;
      outG = LED_PWM_PRESSED;
      outB = LED_PWM_PRESSED;
    }
    return;
  }

  // Online, idle:
  if (!haSynced) {
    // Waiting for HA sync after boot: Solid White at 10% brightness (PWM 26)
    outR = LED_PWM_IDLE;
    outG = LED_PWM_IDLE;
    outB = LED_PWM_IDLE;
  } else if (isOn) {
    // MatterColorLight On: Solid HA-selected RGB color at 10% brightness (PWM 26)
    outR = scaleChannel(baseRgb.r, LED_PWM_IDLE);
    outG = scaleChannel(baseRgb.g, LED_PWM_IDLE);
    outB = scaleChannel(baseRgb.b, LED_PWM_IDLE);
  } else {
    // MatterColorLight Off: Off (PWM 0)
    outR = LED_PWM_OFF;
    outG = LED_PWM_OFF;
    outB = LED_PWM_OFF;
  }
}

static void updateAllButtonLeds(NodeSystemState sysState, uint32_t now) {
  bool forceUpdate = false;

  if (sysState != g_lastSystemState) {
    g_lastSystemState = sysState;
    g_lastBlinkToggleMs = now;
    g_blinkPhaseOn = true;
    forceUpdate = true;

    switch (sysState) {
      case NodeSystemState::UNCOMMISSIONED:
        Serial.println("[STATE] Uncommissioned (pairing mode) -> Blinking Blue (10%) at 1 Hz.");
        break;
      case NodeSystemState::OFFLINE:
        Serial.println("[STATE] Commissioned, Wi-Fi/Matter offline -> Blinking Red (10%) at 1 Hz.");
        break;
      case NodeSystemState::ONLINE:
        Serial.println("[STATE] Online -> Restoring active button LED states.");
        break;
    }
  }

  if (sysState == NodeSystemState::UNCOMMISSIONED || sysState == NodeSystemState::OFFLINE) {
    if ((now - g_lastBlinkToggleMs) >= STATUS_BLINK_INTERVAL_MS) {
      g_lastBlinkToggleMs = now;
      g_blinkPhaseOn = !g_blinkPhaseOn;
      forceUpdate = true;
    }
  }

  for (uint8_t idx = 0; idx < g_buttonCount; ++idx) {
    ButtonSlot &slot = g_buttons[idx];
    if (!slot.connected) {
      continue;
    }

    bool dirty = false;
    portENTER_CRITICAL(&g_stateMux);
    dirty = slot.ledDirty;
    slot.ledDirty = false;
    portEXIT_CRITICAL(&g_stateMux);

    if (forceUpdate || dirty || !slot.hasAppliedColor) {
      uint8_t r = 0, g = 0, b = 0;
      computeTargetLedColor(slot, sysState, g_blinkPhaseOn, r, g, b);
      applyButtonLedColor(slot, r, g, b, now);
    }
  }
}

// -----------------------------------------------------------------------------
// Button Polling, Debounce (50 ms), Long Press (1000 ms) & I2C Reconnection
// -----------------------------------------------------------------------------

static void pollButtonsAndReconnect(NodeSystemState sysState, uint32_t now) {
  // 1. Check for disconnected buttons and retry every 5 s without blocking active buttons
  for (uint8_t idx = 0; idx < g_buttonCount; ++idx) {
    ButtonSlot &slot = g_buttons[idx];
    if (!slot.connected && (now - slot.lastReconnectAttemptMs) >= I2C_RETRY_INTERVAL_MS) {
      slot.lastReconnectAttemptMs = now;
      if (initButtonDriver(slot)) {
        slot.connected = true;
        slot.rawPressed = false;
        slot.debouncedPressed = false;
        slot.longPressFired = false;
        slot.hasAppliedColor = false;
        portENTER_CRITICAL(&g_stateMux);
        slot.ledDirty = true;
        portEXIT_CRITICAL(&g_stateMux);
        Serial.printf("[I2C] Button %u (0x%02X) reconnected on I2C bus!\n",
                      slot.buttonNumber,
                      slot.i2cAddr);
      } else {
        Serial.printf("[I2C] Button %u (0x%02X) still missing on I2C bus; next retry in 5 s.\n",
                      slot.buttonNumber,
                      slot.i2cAddr);
      }
    }
  }

  // 2. Poll active buttons every 20 ms
  if ((now - g_lastPollMs) < BUTTON_POLL_INTERVAL_MS) {
    return;
  }
  g_lastPollMs = now;

  // Button events are ignored while uncommissioned or offline
  if (sysState != NodeSystemState::ONLINE) {
    for (uint8_t idx = 0; idx < g_buttonCount; ++idx) {
      ButtonSlot &slot = g_buttons[idx];
      if (slot.debouncedPressed || slot.rawPressed) {
        slot.rawPressed = false;
        slot.debouncedPressed = false;
        slot.longPressFired = false;
        portENTER_CRITICAL(&g_stateMux);
        slot.ledDirty = true;
        portEXIT_CRITICAL(&g_stateMux);
      }
    }
    return;
  }

  for (uint8_t idx = 0; idx < g_buttonCount; ++idx) {
    ButtonSlot &slot = g_buttons[idx];
    if (!slot.connected) {
      continue;
    }

    if (!pingI2CAddress(slot.i2cAddr)) {
      handleButtonDisconnect(slot, now);
      continue;
    }

    bool currentRaw = slot.driver->getButtonStatus();
    if (currentRaw != slot.rawPressed) {
      slot.rawPressed = currentRaw;
      slot.rawChangeTimeMs = now;
    }

    if ((now - slot.rawChangeTimeMs) >= BUTTON_DEBOUNCE_MS &&
        slot.debouncedPressed != slot.rawPressed) {
      slot.debouncedPressed = slot.rawPressed;

      if (slot.debouncedPressed) {
        // Push down (at 50 ms debounce): emit InitialPress and boost LED to 40%
        slot.pressStartTimeMs = now;
        slot.longPressFired = false;
        portENTER_CRITICAL(&g_stateMux);
        slot.ledDirty = true;
        portEXIT_CRITICAL(&g_stateMux);

        slot.switchEndpoint.press();
        Serial.printf("[BUTTON] Button %u (0x%02X) -> InitialPress\n",
                      slot.buttonNumber,
                      slot.i2cAddr);
      } else {
        // Released (after 50 ms debounce): return LED to 10% (or Off) and emit release event
        uint32_t holdDurationMs = now - slot.pressStartTimeMs;
        portENTER_CRITICAL(&g_stateMux);
        slot.ledDirty = true;
        portEXIT_CRITICAL(&g_stateMux);

        if (slot.longPressFired || holdDurationMs >= BUTTON_LONG_PRESS_MS) {
          if (!slot.longPressFired) {
            slot.switchEndpoint.longPress();
            slot.longPressFired = true;
          }
          slot.switchEndpoint.longRelease();
          Serial.printf("[BUTTON] Button %u (0x%02X) -> LongRelease (held %lu ms)\n",
                        slot.buttonNumber,
                        slot.i2cAddr,
                        static_cast<unsigned long>(holdDurationMs));
        } else {
          slot.switchEndpoint.release();
          Serial.printf("[BUTTON] Button %u (0x%02X) -> ShortRelease (held %lu ms)\n",
                        slot.buttonNumber,
                        slot.i2cAddr,
                        static_cast<unsigned long>(holdDurationMs));
        }
      }
    }

    // Held >= 1000 ms: emit LongPress immediately at 1000 ms
    if (slot.debouncedPressed && !slot.longPressFired &&
        (now - slot.pressStartTimeMs) >= BUTTON_LONG_PRESS_MS) {
      slot.longPressFired = true;
      slot.switchEndpoint.longPress();
      Serial.printf("[BUTTON] Button %u (0x%02X) -> LongPress (1000 ms reached)\n",
                    slot.buttonNumber,
                    slot.i2cAddr);
    }
  }
}

// -----------------------------------------------------------------------------
// Discovery & Endpoint Registration
// -----------------------------------------------------------------------------

static void waitWithBootCheck(uint32_t durationMs) {
  uint32_t start = millis();
  while ((millis() - start) < durationMs) {
    checkBootFactoryReset();
    delay(20);
  }
}

static void setupButtonsAndEndpoints() {
  uint8_t savedMask = 0;
  bool commissionedInNvs = false;
  loadNvsConfig(savedMask, commissionedInNvs);
  g_nvsCommissioned = commissionedInNvs;

  if (commissionedInNvs && savedMask != 0) {
    // Locked while commissioned: always load saved NVS button mask at boot
    g_buttonMask = savedMask;
    Serial.printf("[BOOT] Commissioned mode: loaded locked NVS button mask 0x%02X.\n", g_buttonMask);
  } else {
    // Uncommissioned I2C scan: scan 0x23..0x2A, retrying every 5 s if 0 buttons are found
    Serial.println("[BOOT] Uncommissioned mode: scanning I2C bus (0x23-0x2A) for RGB buttons...");
    while (true) {
      checkBootFactoryReset();
      uint8_t mask = scanI2CButtonMask();
      if (mask != 0) {
        g_buttonMask = mask;
        saveNvsButtonMask(g_buttonMask);
        Serial.printf("[I2C] Discovered button mask 0x%02X saved to NVS.\n", g_buttonMask);
        break;
      }
      Serial.println("[I2C] 0 buttons found on 0x23-0x2A; retrying scan in 5 s...");
      waitWithBootCheck(I2C_RETRY_INTERVAL_MS);
    }
  }

  // Instantiate discovered buttons (1..N ordered by I2C address 0x23..0x2A)
  uint32_t now = millis();
  g_buttonCount = 0;

  for (uint8_t bit = 0; bit < MAX_BUTTONS; ++bit) {
    if ((g_buttonMask & (1U << bit)) == 0) {
      continue;
    }

    uint8_t slotIdx = g_buttonCount++;
    ButtonSlot &slot = g_buttons[slotIdx];
    slot.buttonNumber = slotIdx + 1;
    slot.i2cAddr = BUTTON_ADDR_MIN + bit;
    slot.driver = new DFRobot_RGBButton(&Wire, slot.i2cAddr);
    slot.lastReconnectAttemptMs = now;
    slot.haSynced = false;
    slot.isOn = false;
    slot.baseRgb = {255, 255, 255};
    slot.ledDirty = true;

    slot.connected = initButtonDriver(slot);
    if (slot.connected) {
      Serial.printf("[I2C] Button %u ready at I2C address 0x%02X.\n",
                    slot.buttonNumber,
                    slot.i2cAddr);
    } else {
      Serial.printf("[I2C] WARNING: Paired Button %u (0x%02X) missing at boot; will retry every 5 s.\n",
                    slot.buttonNumber,
                    slot.i2cAddr);
    }

    // Endpoint 2i - 1: MatterGenericSwitch with Momentary + Release + LongPress features
    uint32_t switchFeatures = MatterGenericSwitch::FEATURE_MOMENTARY |
                              MatterGenericSwitch::FEATURE_RELEASE |
                              MatterGenericSwitch::FEATURE_LONG_PRESS;
    if (!slot.switchEndpoint.begin(switchFeatures)) {
      Serial.printf("[MATTER] ERROR: Failed to initialize MatterGenericSwitch for Button %u!\n",
                    slot.buttonNumber);
    }

    // Endpoint 2i: MatterColorLight initialized as Off with White [255, 255, 255] (HSV 0, 0, 254)
    espHsvColor_t initialWhiteHsv = {0, 0, 254};
    if (!slot.lightEndpoint.begin(false, initialWhiteHsv)) {
      Serial.printf("[MATTER] ERROR: Failed to initialize MatterColorLight for Button %u!\n",
                    slot.buttonNumber);
    }
    slot.lightEndpoint.onChange(kLightCallbacks[slotIdx]);

    Serial.printf("[MATTER] Registered Button %u (0x%02X) -> Switch EP %u, Light EP %u\n",
                  slot.buttonNumber,
                  slot.i2cAddr,
                  slot.switchEndpoint.getEndPointId(),
                  slot.lightEndpoint.getEndPointId());
  }
}

static void startMatterStack() {
  Matter.begin();
  g_matterStarted = true;

  // If NVS thought we were commissioned, but the node was removed from the Matter fabric
  // while powered off, clear our NVS commissioned flag and re-sync if button topology changed.
  bool commissionedNow = Matter.isDeviceCommissioned();
  if (g_nvsCommissioned && !commissionedNow) {
    Serial.println("[MATTER] Fabric no longer commissioned; unlocking button discovery in NVS.");
    clearNvsConfig();
    uint8_t liveMask = scanI2CButtonMask();
    if (liveMask != 0 && liveMask != g_buttonMask) {
      saveNvsButtonMask(liveMask);
      Serial.println("[MATTER] Button topology changed while uncommissioned; rebooting to re-register endpoints...");
      ESP.restart();
    }
    saveNvsButtonMask(g_buttonMask);
  } else if (!g_nvsCommissioned && commissionedNow) {
    saveNvsCommissioned(true);
  }

  g_setupComplete = true;

  if (!commissionedNow) {
    Serial.println("\n============================================================");
    Serial.println("  Milieu Panel - Matter Node Uncommissioned (Pairing Mode)");
    Serial.println("============================================================");
    Serial.printf("  Discovered Buttons : %u (Mask: 0x%02X)\n", g_buttonCount, g_buttonMask);
    Serial.printf("  Manual Pairing Code: %s\n", Matter.getManualPairingCode().c_str());
    Serial.printf("  QR Code URL        : %s\n", Matter.getOnboardingQRCodeUrl().c_str());
    Serial.println("============================================================\n");
  } else {
    Serial.println("\n============================================================");
    Serial.println("  Milieu Panel - Matter Node Commissioned");
    Serial.printf("  Locked Buttons     : %u (Mask: 0x%02X)\n", g_buttonCount, g_buttonMask);
    Serial.println("============================================================\n");
  }

  g_lastSystemState = getCurrentSystemState();
  g_lastBlinkToggleMs = millis();
  g_blinkPhaseOn = true;
  updateAllButtonLeds(g_lastSystemState, millis());
}

// -----------------------------------------------------------------------------
// Arduino Entry Points
// -----------------------------------------------------------------------------

void setup() {
  initRfSwitch();

  pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);

  Serial.begin(115200);
  uint32_t serialWaitStart = millis();
  while (!Serial && (millis() - serialWaitStart) < 2000) {
    delay(10);
  }

  Serial.println("\n[BOOT] Starting Milieu Panel (Seeed Studio XIAO ESP32C6)...");
  initI2CBus();
  setupButtonsAndEndpoints();
  startMatterStack();
}

void loop() {
  uint32_t now = millis();

  // Monitor onboard BOOT button (GPIO9) for 5-second factory reset hold
  checkBootFactoryReset();

  // Lock NVS button mask as soon as commissioning completes
  if (!g_nvsCommissioned && Matter.isDeviceCommissioned()) {
    saveNvsCommissioned(true);
    Serial.printf("[MATTER] Device commissioned! Button mask 0x%02X locked in NVS.\n",
                  g_buttonMask);
  }

  NodeSystemState sysState = getCurrentSystemState();
  pollButtonsAndReconnect(sysState, now);
  updateAllButtonLeds(sysState, now);

  delay(2);
}
