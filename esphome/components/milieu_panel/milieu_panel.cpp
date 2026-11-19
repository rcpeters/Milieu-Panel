#include "milieu_panel.h"

#include <algorithm>
#include <cmath>

#include "driver/gpio.h"
#include "esp_wifi.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#ifdef USE_API
#include "esphome/components/api/api_server.h"
#endif

#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif

namespace esphome {
namespace milieu_panel {

static const char *const TAG = "milieu_panel";
static constexpr uint32_t PREF_KEY_BTN_MASK = 0x4D494C01UL;  // "MIL\x01"

static float clamp_unit(float v) {
  return std::max(0.0f, std::min(1.0f, v));
}

void MilieuButtonLight::write_state(light::LightState *state) {
  if (this->parent_ == nullptr || state == nullptr) {
    return;
  }
  if (!this->parent_->is_api_connected()) {
    return;
  }
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  state->current_values_as_rgb(&r, &g, &b);
  this->parent_->on_light_state(this->slot_, state->current_values.is_on(), r, g, b);
}

void MilieuPanel::set_max_brightness(float max_brightness) {
  float clamped = std::max(0.01f, std::min(0.48f, max_brightness));
  this->max_pwm_ = static_cast<uint8_t>(std::lroundf(clamped * 255.0f));
}

void MilieuPanel::register_button_event(uint8_t idx, MilieuButtonEvent *ev) {
  if (idx < MAX_BUTTONS) {
    this->slots_[idx].event_entity = ev;
  }
}

void MilieuPanel::register_button_light(uint8_t idx, MilieuButtonLight *light) {
  if (idx < MAX_BUTTONS) {
    this->slots_[idx].light_output = light;
  }
}

bool MilieuPanel::is_api_connected() const {
#ifdef USE_API
  return api::global_api_server != nullptr && api::global_api_server->is_connected();
#else
  return false;
#endif
}

void MilieuPanel::on_light_state(uint8_t idx, bool is_on, float r, float g, float b) {
  if (idx >= MAX_BUTTONS) {
    return;
  }
  ButtonSlot &slot = this->slots_[idx];
  slot.ha_synced = true;
  slot.is_on = is_on;
  if (is_on) {
    slot.r = static_cast<uint8_t>(std::lroundf(clamp_unit(r) * static_cast<float>(this->max_pwm_)));
    slot.g = static_cast<uint8_t>(std::lroundf(clamp_unit(g) * static_cast<float>(this->max_pwm_)));
    slot.b = static_cast<uint8_t>(std::lroundf(clamp_unit(b) * static_cast<float>(this->max_pwm_)));
  } else {
    slot.r = 0;
    slot.g = 0;
    slot.b = 0;
  }
}

void MilieuPanel::set_ota_active(bool active) {
  this->ota_active_ = active;
  if (active) {
    // Write Amber synchronously before OTA flash writes block loop()
    constexpr uint8_t amber_r = LED_PWM_STATUS;
    constexpr uint8_t amber_g = LED_PWM_STATUS / 2;
    for (uint8_t i = 0; i < this->button_count_; ++i) {
      ButtonSlot &slot = this->slots_[i];
      if (slot.active && slot.connected) {
        this->set_button_rgb(slot.dev, amber_r, amber_g, 0);
        slot.has_color = false;
      }
    }
  }
}

bool MilieuPanel::probe_button(uint8_t addr) {
  uint8_t buf[2] = {0, 0};
  this->probe_dev_.set_i2c_address(addr);
  if (this->probe_dev_.read_register(REG_PID_MSB, buf, sizeof(buf)) != i2c::ERROR_OK) {
    return false;
  }
  uint16_t pid = (static_cast<uint16_t>(buf[0]) << 8) | buf[1];
  return (pid != 0x0000 && pid != 0xFFFF);
}

uint8_t MilieuPanel::scan_buttons() {
  uint8_t mask = 0;
  for (uint8_t i = 0; i < MAX_BUTTONS; ++i) {
    if (this->probe_button(BUTTON_ADDR_MIN + i)) {
      mask |= static_cast<uint8_t>(1U << i);
    }
    delay(5);
  }
  return mask;
}

bool MilieuPanel::set_button_rgb(i2c::I2CDevice &dev, uint8_t r, uint8_t g, uint8_t b) {
  uint8_t rgb[3] = {r, g, b};
  return dev.write_register(REG_RED, rgb, sizeof(rgb)) == i2c::ERROR_OK;
}

bool MilieuPanel::read_button_pressed(i2c::I2CDevice &dev, bool &pressed) {
  uint8_t val = 0;
  if (dev.read_register(REG_BUTTON_SIGNAL, &val, 1) != i2c::ERROR_OK) {
    return false;
  }
  pressed = (val != 0);
  return true;
}

void MilieuPanel::disconnect_slot(ButtonSlot &slot, uint32_t now) {
  if (slot.connected) {
    slot.connected = false;
    slot.last_retry_ms = now;
    slot.raw_pressed = false;
    slot.pressed = false;
    slot.long_fired = false;
    slot.has_color = false;
    ESP_LOGW(TAG, "Button %u (0x%02X) disconnected; probing every %lu ms",
             slot.number, slot.addr, static_cast<unsigned long>(this->retry_interval_ms_));
  }
}

void MilieuPanel::apply_button_mask(uint8_t mask, uint32_t now) {
  this->button_mask_ = mask;
  this->button_count_ = 0;

  for (uint8_t i = 0; i < MAX_BUTTONS; ++i) {
    this->slots_[i].active = false;
    this->slots_[i].connected = false;
    this->slots_[i].number = 0;
    this->slots_[i].addr = 0;
    this->slots_[i].has_color = false;
  }

  for (uint8_t bit = 0; bit < MAX_BUTTONS; ++bit) {
    if ((mask & (1U << bit)) == 0) {
      continue;
    }
    uint8_t idx = this->button_count_++;
    ButtonSlot &slot = this->slots_[idx];
    slot.number = idx + 1;
    slot.addr = BUTTON_ADDR_MIN + bit;
    slot.active = true;
    slot.dev.set_i2c_bus(this->bus_);
    slot.dev.set_i2c_address(slot.addr);
    slot.connected = this->probe_button(slot.addr);
    slot.last_retry_ms = now;
    ESP_LOGI(TAG, "Slot Button %u mapped to I2C 0x%02X (%s)",
             slot.number, slot.addr, slot.connected ? "OK" : "MISSING");
  }
}

void MilieuPanel::setup() {
  // Configure RF antenna switch before Wi-Fi initializes
  auto rf_pin = static_cast<gpio_num_t>(this->rf_switch_pin_);
  gpio_reset_pin(rf_pin);
  gpio_set_direction(rf_pin, GPIO_MODE_OUTPUT);
  gpio_set_level(rf_pin, 0);  // LOW enables RF switch

  auto ant_pin = static_cast<gpio_num_t>(this->antenna_select_pin_);
  gpio_reset_pin(ant_pin);
  gpio_set_direction(ant_pin, GPIO_MODE_OUTPUT);
  gpio_set_level(ant_pin, this->external_antenna_ ? 1 : 0);

  // Configure BOOT pin (active-LOW) for 5 s factory reset
  auto boot_pin = static_cast<gpio_num_t>(this->boot_pin_);
  gpio_reset_pin(boot_pin);
  gpio_set_direction(boot_pin, GPIO_MODE_INPUT);
  gpio_set_pull_mode(boot_pin, GPIO_PULLUP_ONLY);

  this->probe_dev_.set_i2c_bus(this->bus_);
  this->mask_pref_ = global_preferences->make_preference<uint8_t>(PREF_KEY_BTN_MASK);

  uint8_t saved_mask = 0;
  if (this->mask_pref_.load(&saved_mask) && saved_mask != 0) {
    ESP_LOGI(TAG, "Loaded saved btn_mask 0x%02X from NVS", saved_mask);
    this->apply_button_mask(saved_mask, millis());
    return;
  }

  ESP_LOGI(TAG, "Scanning I2C bus (0x23-0x2A) for DFR0991 buttons...");
  uint8_t scanned = this->scan_buttons();
  if (scanned != 0) {
    this->mask_pref_.save(&scanned);
    global_preferences->sync();
    ESP_LOGI(TAG, "Discovered btn_mask 0x%02X and saved to NVS", scanned);
    this->apply_button_mask(scanned, millis());
  } else {
    ESP_LOGW(TAG, "No buttons found at boot; retrying I2C scan every %lu ms",
             static_cast<unsigned long>(this->retry_interval_ms_));
    this->scanning_for_buttons_ = true;
    this->last_scan_ms_ = millis();
  }
}

void MilieuPanel::dump_config() {
  ESP_LOGCONFIG(TAG, "Milieu Panel:");
  ESP_LOGCONFIG(TAG, "  Antenna: %s (RF_SW=GPIO%u, ANT_SEL=GPIO%u)",
                this->external_antenna_ ? "External U.FL" : "Internal Ceramic",
                this->rf_switch_pin_, this->antenna_select_pin_);
  ESP_LOGCONFIG(TAG, "  BOOT Reset Pin: GPIO%u (hold %lu ms)",
                this->boot_pin_, static_cast<unsigned long>(this->factory_reset_hold_ms_));
  ESP_LOGCONFIG(TAG, "  Max PWM Cap: %u/255 (%.0f%%)",
                this->max_pwm_, (static_cast<float>(this->max_pwm_) / 255.0f) * 100.0f);
  ESP_LOGCONFIG(TAG, "  Active Buttons: %u (mask 0x%02X)", this->button_count_, this->button_mask_);
}

NodeState MilieuPanel::get_node_state() const {
  if (this->ota_active_) {
    return NodeState::OTA;
  }
#ifdef USE_WIFI
  if (wifi::global_wifi_component != nullptr) {
    if (!wifi::global_wifi_component->is_connected()) {
      return wifi::global_wifi_component->has_sta() ? NodeState::OFFLINE : NodeState::UNPROVISIONED;
    }
  }
#endif
  if (!this->is_api_connected()) {
    return NodeState::OFFLINE;
  }
  return NodeState::ONLINE;
}

void MilieuPanel::check_factory_reset(uint32_t now) {
  auto boot_pin = static_cast<gpio_num_t>(this->boot_pin_);
  if (gpio_get_level(boot_pin) == 0) {
    if (!this->boot_held_) {
      this->boot_held_ = true;
      this->boot_hold_start_ms_ = now;
    } else if ((now - this->boot_hold_start_ms_) >= this->factory_reset_hold_ms_) {
      ESP_LOGW(TAG, "Factory reset triggered! Flashing Purple 3x and clearing NVS + Wi-Fi...");
      uint8_t purple[3] = {LED_PWM_STATUS, 0, LED_PWM_STATUS};
      uint8_t off[3] = {0, 0, 0};
      for (uint8_t c = 0; c < FACTORY_RESET_BLINK_COUNT; ++c) {
        for (uint8_t a = BUTTON_ADDR_MIN; a <= BUTTON_ADDR_MAX; ++a) {
          this->probe_dev_.set_i2c_address(a);
          this->probe_dev_.write_register(REG_RED, purple, sizeof(purple));
        }
        delay(FACTORY_RESET_BLINK_MS);
        for (uint8_t a = BUTTON_ADDR_MIN; a <= BUTTON_ADDR_MAX; ++a) {
          this->probe_dev_.set_i2c_address(a);
          this->probe_dev_.write_register(REG_RED, off, sizeof(off));
        }
        delay(FACTORY_RESET_BLINK_MS);
      }

      uint8_t zero = 0;
      this->mask_pref_.save(&zero);
      global_preferences->sync();
      global_preferences->reset();
      esp_wifi_restore();
      delay(50);
      App.safe_reboot();
    }
  } else {
    this->boot_held_ = false;
  }
}

void MilieuPanel::update_buttons(NodeState state, uint32_t now) {
  for (uint8_t i = 0; i < this->button_count_; ++i) {
    ButtonSlot &slot = this->slots_[i];
    if (slot.active && !slot.connected && (now - slot.last_retry_ms) >= this->retry_interval_ms_) {
      slot.last_retry_ms = now;
      if (this->probe_button(slot.addr)) {
        slot.connected = true;
        slot.has_color = false;
        ESP_LOGI(TAG, "Button %u (0x%02X) reconnected", slot.number, slot.addr);
      }
    }
  }

  if ((now - this->last_poll_ms_) < this->poll_interval_ms_) {
    return;
  }
  this->last_poll_ms_ = now;

  if (state != NodeState::ONLINE) {
    for (uint8_t i = 0; i < this->button_count_; ++i) {
      this->slots_[i].raw_pressed = false;
      this->slots_[i].pressed = false;
      this->slots_[i].long_fired = false;
    }
    return;
  }

  for (uint8_t i = 0; i < this->button_count_; ++i) {
    ButtonSlot &slot = this->slots_[i];
    if (!slot.active || !slot.connected) {
      continue;
    }

    bool raw = false;
    if (!this->read_button_pressed(slot.dev, raw)) {
      this->disconnect_slot(slot, now);
      continue;
    }

    if (raw != slot.raw_pressed) {
      slot.raw_pressed = raw;
      slot.debounce_ms = now;
    }

    if ((now - slot.debounce_ms) >= this->debounce_ms_ && slot.pressed != slot.raw_pressed) {
      slot.pressed = slot.raw_pressed;
      if (slot.pressed) {
        slot.press_start_ms = now;
        slot.long_fired = false;
        if (slot.event_entity != nullptr) {
          slot.event_entity->fire("initial_press");
        }
        ESP_LOGD(TAG, "Button %u (0x%02X) -> initial_press", slot.number, slot.addr);
      } else if (slot.long_fired || (now - slot.press_start_ms) >= this->long_press_ms_) {
        if (!slot.long_fired && slot.event_entity != nullptr) {
          slot.event_entity->fire("long_press");
        }
        if (slot.event_entity != nullptr) {
          slot.event_entity->fire("long_release");
        }
        ESP_LOGD(TAG, "Button %u (0x%02X) -> long_release (%lu ms)",
                 slot.number, slot.addr, static_cast<unsigned long>(now - slot.press_start_ms));
      } else {
        if (slot.event_entity != nullptr) {
          slot.event_entity->fire("short_release");
        }
        ESP_LOGD(TAG, "Button %u (0x%02X) -> short_release (%lu ms)",
                 slot.number, slot.addr, static_cast<unsigned long>(now - slot.press_start_ms));
      }
    }

    if (slot.pressed && !slot.long_fired && (now - slot.press_start_ms) >= this->long_press_ms_) {
      slot.long_fired = true;
      if (slot.event_entity != nullptr) {
        slot.event_entity->fire("long_press");
      }
      ESP_LOGD(TAG, "Button %u (0x%02X) -> long_press", slot.number, slot.addr);
    }
  }
}

void MilieuPanel::update_leds(NodeState state, uint32_t now) {
  if (state == NodeState::OTA) {
    return;
  }

  bool blink_on = ((now / STATUS_BLINK_INTERVAL_MS) % 2) == 0;

  for (uint8_t i = 0; i < this->button_count_; ++i) {
    ButtonSlot &slot = this->slots_[i];
    if (!slot.active || !slot.connected) {
      continue;
    }

    uint8_t r = LED_PWM_OFF;
    uint8_t g = LED_PWM_OFF;
    uint8_t b = LED_PWM_OFF;

    if (state == NodeState::UNPROVISIONED) {
      b = blink_on ? LED_PWM_STATUS : LED_PWM_OFF;
    } else if (state == NodeState::OFFLINE) {
      r = blink_on ? LED_PWM_STATUS : LED_PWM_OFF;
    } else {
      if (!slot.ha_synced) {
        r = LED_PWM_STATUS;
        g = LED_PWM_STATUS;
        b = LED_PWM_STATUS;
      } else if (slot.is_on) {
        r = slot.r;
        g = slot.g;
        b = slot.b;
      }
    }

    if (!slot.has_color || slot.last_r != r || slot.last_g != g || slot.last_b != b) {
      if (this->set_button_rgb(slot.dev, r, g, b)) {
        slot.last_r = r;
        slot.last_g = g;
        slot.last_b = b;
        slot.has_color = true;
      } else {
        this->disconnect_slot(slot, now);
      }
    }
  }
}

void MilieuPanel::loop() {
  uint32_t now = millis();
  this->check_factory_reset(now);

  if (this->scanning_for_buttons_) {
    if ((now - this->last_scan_ms_) >= this->retry_interval_ms_) {
      this->last_scan_ms_ = now;
      uint8_t scanned = this->scan_buttons();
      if (scanned != 0) {
        this->scanning_for_buttons_ = false;
        this->mask_pref_.save(&scanned);
        global_preferences->sync();
        ESP_LOGI(TAG, "Discovered btn_mask 0x%02X and saved to NVS", scanned);
        this->apply_button_mask(scanned, now);
      }
    }
    return;
  }

  NodeState state = this->get_node_state();
  this->update_buttons(state, now);
  this->update_leds(state, now);
}

}  // namespace milieu_panel
}  // namespace esphome
