// Milieu Panel - Matter-over-Wi-Fi Multi-Button RGB Controller (1-8 Buttons)
// Target Board: Seeed Studio XIAO ESP32C6 (Arduino-ESP32 v3.x+)
// Partition Scheme: Huge APP (3MB No OTA/1MB SPIFFS)

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
static bool g_nvsCommissioned = false;

static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t g_lastPollMs = 0;
static uint32_t g_bootHoldStartMs = 0;
static bool g_bootHeld = false;

// -----------------------------------------------------------------------------
// I2C Helpers (Direct register access to DFR0991 at 100 kHz)
// -----------------------------------------------------------------------------

static bool probeButton(uint8_t addr) {
  Wire.beginTransmission(addr);
  Wire.write(RGBBUTTON_PID_MSB_REG);
  if (Wire.endTransmission() != 0 || Wire.requestFrom(addr, (size_t)2) != 2) {
    return false;
  }
  uint16_t pid = ((uint16_t)Wire.read() << 8) | Wire.read();
  return (pid != 0x0000 && pid != 0xFFFF);
}

static bool setButtonRgb(uint8_t addr, uint8_t r, uint8_t g, uint8_t b) {
  Wire.beginTransmission(addr);
  Wire.write(RGBBUTTON_RED_REG);
  Wire.write(r);
  Wire.write(g);
  Wire.write(b);
  return (Wire.endTransmission() == 0);
}

static bool readButtonPressed(uint8_t addr, bool &pressed) {
  Wire.beginTransmission(addr);
  Wire.write(RGBBUTTON_BUTTON_SIGNAL_REG);
  if (Wire.endTransmission() != 0 || Wire.requestFrom(addr, (size_t)1) != 1) {
    return false;
  }
  pressed = (Wire.read() != 0);
  return true;
}

static uint8_t scanButtons() {
  uint8_t mask = 0;
  for (uint8_t i = 0; i < MAX_BUTTONS; ++i) {
    if (probeButton(BUTTON_ADDR_MIN + i)) {
      mask |= (1U << i);
    }
    delay(5);
  }
  return mask;
}

static uint8_t scalePwm(uint8_t val, uint8_t maxPwm) {
  return (uint8_t)(((uint16_t)val * maxPwm + 127U) / 255U);
}

// -----------------------------------------------------------------------------
// NVS & Factory Reset (5 s BOOT Button Hold)
// -----------------------------------------------------------------------------

static void saveNvs(uint8_t mask, bool commissioned) {
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    prefs.putUChar(NVS_KEY_BTN_MASK, mask);
    prefs.putBool(NVS_KEY_COMMISSIONED, commissioned);
    prefs.end();
  }
  g_nvsCommissioned = commissioned;
}

static void checkFactoryReset(uint32_t now) {
  if (digitalRead(PIN_BOOT_BUTTON) == LOW) {
    if (!g_bootHeld) {
      g_bootHeld = true;
      g_bootHoldStartMs = now;
    } else if ((now - g_bootHoldStartMs) >= FACTORY_RESET_HOLD_MS) {
      Serial.println("\n[RESET] Factory reset triggered -> Flashing Purple 3x...");
      for (uint8_t c = 0; c < FACTORY_RESET_BLINK_COUNT; ++c) {
        for (uint8_t a = BUTTON_ADDR_MIN; a <= BUTTON_ADDR_MAX; ++a) {
          setButtonRgb(a, LED_PWM_ON, 0, LED_PWM_ON);
        }
        delay(FACTORY_RESET_BLINK_MS);
        for (uint8_t a = BUTTON_ADDR_MIN; a <= BUTTON_ADDR_MAX; ++a) {
          setButtonRgb(a, 0, 0, 0);
        }
        delay(FACTORY_RESET_BLINK_MS);
      }
      Preferences prefs;
      if (prefs.begin(NVS_NAMESPACE, false)) {
        prefs.clear();
        prefs.end();
      }
      Matter.decommission();
      ESP.restart();
    }
  } else {
    g_bootHeld = false;
  }
}

// -----------------------------------------------------------------------------
// MatterColorLight Callbacks
// -----------------------------------------------------------------------------

static bool onLightChange(uint8_t idx, bool state, espHsvColor_t hsv) {
  if (idx >= g_buttonCount) {
    return true;
  }
  espRgbColor_t rgb = espHsvColorToRgbColor({hsv.h, hsv.s, 254});
  portENTER_CRITICAL(&g_mux);
  g_buttons[idx].haSynced = true;
  g_buttons[idx].isOn = state;
  g_buttons[idx].rgb = rgb;
  portEXIT_CRITICAL(&g_mux);
  return true;
}

using LightCb = bool (*)(bool, espHsvColor_t);
static const LightCb kLightCbs[MAX_BUTTONS] = {
    [](bool s, espHsvColor_t c) { return onLightChange(0, s, c); },
    [](bool s, espHsvColor_t c) { return onLightChange(1, s, c); },
    [](bool s, espHsvColor_t c) { return onLightChange(2, s, c); },
    [](bool s, espHsvColor_t c) { return onLightChange(3, s, c); },
    [](bool s, espHsvColor_t c) { return onLightChange(4, s, c); },
    [](bool s, espHsvColor_t c) { return onLightChange(5, s, c); },
    [](bool s, espHsvColor_t c) { return onLightChange(6, s, c); },
    [](bool s, espHsvColor_t c) { return onLightChange(7, s, c); },
};

static constexpr const char *kButtonLabels[MAX_BUTTONS] = {
    "Button 1", "Button 2", "Button 3", "Button 4",
    "Button 5", "Button 6", "Button 7", "Button 8",
};

static constexpr const char *kLightLabels[MAX_BUTTONS] = {
    "Button 1 Light", "Button 2 Light", "Button 3 Light", "Button 4 Light",
    "Button 5 Light", "Button 6 Light", "Button 7 Light", "Button 8 Light",
};

static void setEndpointLabel(MatterEndPoint &ep, const char *label) {
  esp_matter::endpoint_t *endpoint = esp_matter::endpoint::get(esp_matter::node::get(), ep.getEndPointId());
  if (!endpoint) {
    return;
  }
  esp_matter::endpoint::bridged_node::config_t cfg;
  if (esp_matter::endpoint::bridged_node::add(endpoint, &cfg) == ESP_OK) {
    esp_matter::cluster_t *cluster =
        esp_matter::cluster::get(endpoint, chip::app::Clusters::BridgedDeviceBasicInformation::Id);
    if (cluster) {
      char buf[32];
      strncpy(buf, label, sizeof(buf) - 1);
      buf[sizeof(buf) - 1] = '\0';
      esp_matter::cluster::bridged_device_basic_information::attribute::create_node_label(cluster, buf, strlen(buf));
    }
  }
}

// -----------------------------------------------------------------------------
// Button Polling & LED State Machine
// -----------------------------------------------------------------------------

static NodeState getNodeState() {
  if (!Matter.isDeviceCommissioned()) {
    return NodeState::UNCOMMISSIONED;
  }
  if (!Matter.isWiFiConnected() || !Matter.isDeviceConnected()) {
    return NodeState::OFFLINE;
  }
  return NodeState::ONLINE;
}

static void disconnectSlot(ButtonSlot &b, uint32_t now) {
  if (b.connected) {
    b.connected = false;
    b.lastRetryMs = now;
    b.rawPressed = b.pressed = b.longFired = b.hasColor = false;
    Serial.printf("[I2C] Button %u (0x%02X) disconnected; retrying every 5 s.\n", b.number, b.addr);
  }
}

static void updateButtons(NodeState state, uint32_t now) {
  for (uint8_t i = 0; i < g_buttonCount; ++i) {
    ButtonSlot &b = g_buttons[i];
    if (!b.connected && (now - b.lastRetryMs) >= I2C_RETRY_INTERVAL_MS) {
      b.lastRetryMs = now;
      if (probeButton(b.addr)) {
        b.connected = true;
        b.hasColor = false;
        Serial.printf("[I2C] Button %u (0x%02X) reconnected.\n", b.number, b.addr);
      }
    }
  }

  if ((now - g_lastPollMs) < BUTTON_POLL_INTERVAL_MS) {
    return;
  }
  g_lastPollMs = now;

  if (state != NodeState::ONLINE) {
    for (uint8_t i = 0; i < g_buttonCount; ++i) {
      g_buttons[i].rawPressed = g_buttons[i].pressed = g_buttons[i].longFired = false;
    }
    return;
  }

  for (uint8_t i = 0; i < g_buttonCount; ++i) {
    ButtonSlot &b = g_buttons[i];
    if (!b.connected) {
      continue;
    }

    bool raw = false;
    if (!readButtonPressed(b.addr, raw)) {
      disconnectSlot(b, now);
      continue;
    }

    if (raw != b.rawPressed) {
      b.rawPressed = raw;
      b.debounceMs = now;
    }

    if ((now - b.debounceMs) >= BUTTON_DEBOUNCE_MS && b.pressed != b.rawPressed) {
      b.pressed = b.rawPressed;
      if (b.pressed) {
        b.pressStartMs = now;
        b.longFired = false;
        b.sw.press();
        Serial.printf("[BTN] Button %u (0x%02X) -> InitialPress\n", b.number, b.addr);
      } else if (b.longFired || (now - b.pressStartMs) >= BUTTON_LONG_PRESS_MS) {
        if (!b.longFired) {
          b.sw.longPress();
        }
        b.sw.longRelease();
        Serial.printf("[BTN] Button %u (0x%02X) -> LongRelease (%lu ms)\n",
                      b.number, b.addr, (unsigned long)(now - b.pressStartMs));
      } else {
        b.sw.release();
        Serial.printf("[BTN] Button %u (0x%02X) -> ShortRelease (%lu ms)\n",
                      b.number, b.addr, (unsigned long)(now - b.pressStartMs));
      }
    }

    if (b.pressed && !b.longFired && (now - b.pressStartMs) >= BUTTON_LONG_PRESS_MS) {
      b.longFired = true;
      b.sw.longPress();
      Serial.printf("[BTN] Button %u (0x%02X) -> LongPress\n", b.number, b.addr);
    }
  }
}

static void updateLeds(NodeState state, uint32_t now) {
  bool blinkOn = ((now / STATUS_BLINK_INTERVAL_MS) % 2) == 0;

  for (uint8_t i = 0; i < g_buttonCount; ++i) {
    ButtonSlot &b = g_buttons[i];
    if (!b.connected) {
      continue;
    }

    uint8_t r = 0, g = 0, bl = 0;
    if (state == NodeState::UNCOMMISSIONED) {
      bl = blinkOn ? LED_PWM_ON : LED_PWM_OFF;
    } else if (state == NodeState::OFFLINE) {
      r = blinkOn ? LED_PWM_ON : LED_PWM_OFF;
    } else {
      portENTER_CRITICAL(&g_mux);
      bool synced = b.haSynced;
      bool on = b.isOn;
      espRgbColor_t c = b.rgb;
      portEXIT_CRITICAL(&g_mux);

      if (synced && on) {
        r = scalePwm(c.r, LED_PWM_ON);
        g = scalePwm(c.g, LED_PWM_ON);
        bl = scalePwm(c.b, LED_PWM_ON);
      } else if (!synced) {
        r = g = bl = LED_PWM_ON; // White (10% waiting for HA sync)
      }
    }

    if (!b.hasColor || b.lastR != r || b.lastG != g || b.lastB != bl) {
      if (setButtonRgb(b.addr, r, g, bl)) {
        b.lastR = r;
        b.lastG = g;
        b.lastB = bl;
        b.hasColor = true;
      } else {
        disconnectSlot(b, now);
      }
    }
  }
}

// -----------------------------------------------------------------------------
// Setup & Loop
// -----------------------------------------------------------------------------

void setup() {
  pinMode(PIN_RF_SWITCH_EN, OUTPUT);
  digitalWrite(PIN_RF_SWITCH_EN, LOW);
  pinMode(PIN_RF_ANT_SELECT, OUTPUT);
  digitalWrite(PIN_RF_ANT_SELECT, USE_EXTERNAL_ANTENNA ? HIGH : LOW);
  pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);

  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 2000) {
    delay(10);
  }

  delay(150);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);
  Wire.setTimeOut(50);
  delay(50);

  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, true)) {
    g_buttonMask = prefs.getUChar(NVS_KEY_BTN_MASK, 0);
    g_nvsCommissioned = prefs.getBool(NVS_KEY_COMMISSIONED, false);
    prefs.end();
  }

  if (!g_nvsCommissioned || g_buttonMask == 0) {
    Serial.println("[BOOT] Scanning I2C bus (0x23-0x2A) for RGB buttons...");
    while ((g_buttonMask = scanButtons()) == 0) {
      Serial.println("[I2C] No buttons found; retrying in 5 s...");
      uint32_t waitStart = millis();
      while ((millis() - waitStart) < I2C_RETRY_INTERVAL_MS) {
        checkFactoryReset(millis());
        delay(20);
      }
    }
    saveNvs(g_buttonMask, false);
  }

  uint32_t switchFlags = MatterGenericSwitch::FEATURE_MOMENTARY |
                         MatterGenericSwitch::FEATURE_RELEASE |
                         MatterGenericSwitch::FEATURE_LONG_PRESS;

  // Pass 1: Register all Switches first so their Endpoint IDs are 1..N
  for (uint8_t bit = 0; bit < MAX_BUTTONS; ++bit) {
    if (!(g_buttonMask & (1U << bit))) {
      continue;
    }
    uint8_t idx = g_buttonCount++;
    ButtonSlot &b = g_buttons[idx];
    b.number = idx + 1;
    b.addr = BUTTON_ADDR_MIN + bit;
    b.connected = probeButton(b.addr);
    b.lastRetryMs = millis();

    b.sw.begin(switchFlags);
    b.sw.setTagList({{MatterTags::Number::NS, b.number},
                     MatterTags::Switches::createCustomTag(kButtonLabels[idx])});
    setEndpointLabel(b.sw, kButtonLabels[idx]);
  }

  // Pass 2: Register all Lights second so their Endpoint IDs are (N+1)..2N
  for (uint8_t idx = 0; idx < g_buttonCount; ++idx) {
    ButtonSlot &b = g_buttons[idx];
    b.light.begin(false, {0, 0, 254});
    b.light.setTagList({{MatterTags::Number::NS, b.number},
                        MatterTags::Switches::createCustomTag(kLightLabels[idx])});
    b.light.onChange(kLightCbs[idx]);
    setEndpointLabel(b.light, kLightLabels[idx]);

    Serial.printf("[BOOT] Button %u @ 0x%02X (%s) -> Switch EP %u, Light EP %u\n",
                  b.number, b.addr, b.connected ? "OK" : "MISSING",
                  b.sw.getEndPointId(), b.light.getEndPointId());
  }

  Matter.setVendorName("Make Rob Make");
  Matter.setProductName("Milieu Panel");
  Matter.setDeviceName("Milieu Panel");
  Matter.begin();

  if (g_nvsCommissioned && !Matter.isDeviceCommissioned()) {
    saveNvs(0, false);
    ESP.restart();
  }

  if (!Matter.isDeviceCommissioned()) {
    Serial.printf("\n=== Pairing Mode (Buttons: %u, Mask: 0x%02X) ===\n", g_buttonCount, g_buttonMask);
    Serial.printf("Manual Code: %s\n", Matter.getManualPairingCode().c_str());
    Serial.printf("QR Code URL: %s\n\n", Matter.getOnboardingQRCodeUrl().c_str());
  }
}

void loop() {
  uint32_t now = millis();
  checkFactoryReset(now);

  if (!g_nvsCommissioned && Matter.isDeviceCommissioned()) {
    saveNvs(g_buttonMask, true);
    Serial.printf("[MATTER] Commissioned! Locked button mask 0x%02X in NVS.\n", g_buttonMask);
  }

  NodeState state = getNodeState();
  updateButtons(state, now);
  updateLeds(state, now);
  delay(2);
}
