#pragma once

#include <cstdint>

#include "esphome/components/event/event.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/components/light/light_output.h"
#include "esphome/components/light/light_state.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/preferences.h"

namespace esphome {
namespace milieu_panel {

constexpr uint8_t BUTTON_ADDR_MIN = 0x23;
constexpr uint8_t BUTTON_ADDR_MAX = 0x2A;
constexpr uint8_t MAX_BUTTONS = BUTTON_ADDR_MAX - BUTTON_ADDR_MIN + 1;

constexpr uint8_t REG_RED = 0x01;
constexpr uint8_t REG_BUTTON_SIGNAL = 0x04;
constexpr uint8_t REG_PID_MSB = 0x09;

constexpr uint8_t LED_PWM_OFF = 0;
constexpr uint8_t LED_PWM_STATUS = 26;  // 10% fixed status brightness

constexpr uint32_t STATUS_BLINK_INTERVAL_MS = 500;
constexpr uint32_t FACTORY_RESET_BLINK_MS = 150;
constexpr uint8_t FACTORY_RESET_BLINK_COUNT = 3;

enum class NodeState : uint8_t { UNPROVISIONED, OFFLINE, ONLINE, OTA };

class MilieuPanel;

class MilieuButtonEvent : public event::Event {
 public:
  void fire(const std::string &event_type) { this->trigger(event_type); }
};

class MilieuButtonLight : public light::LightOutput {
 public:
  light::LightTraits get_traits() override {
    auto traits = light::LightTraits();
    traits.set_supported_color_modes({light::ColorMode::RGB});
    return traits;
  }

  void setup_state(light::LightState *state) override { this->state_ = state; }
  void write_state(light::LightState *state) override;

  void set_parent(MilieuPanel *parent, uint8_t slot) {
    this->parent_ = parent;
    this->slot_ = slot;
  }

 protected:
  MilieuPanel *parent_{nullptr};
  light::LightState *state_{nullptr};
  uint8_t slot_{0};
};

struct ButtonSlot {
  uint8_t number{0};  // 1..N (0 when unused)
  uint8_t addr{0};    // 0x23..0x2A
  bool active{false};
  bool connected{false};
  uint32_t last_retry_ms{0};

  i2c::I2CDevice dev;
  MilieuButtonEvent *event_entity{nullptr};
  MilieuButtonLight *light_output{nullptr};

  bool raw_pressed{false};
  bool pressed{false};
  bool long_fired{false};
  uint32_t debounce_ms{0};
  uint32_t press_start_ms{0};

  bool ha_synced{false};
  bool is_on{false};
  uint8_t r{0};
  uint8_t g{0};
  uint8_t b{0};

  uint8_t last_r{255};
  uint8_t last_g{255};
  uint8_t last_b{255};
  bool has_color{false};
};

class MilieuPanel : public Component {
 public:
  float get_setup_priority() const override { return setup_priority::HARDWARE; }
  void setup() override;
  void loop() override;
  void dump_config() override;

  void set_i2c_bus(i2c::I2CBus *bus) { this->bus_ = bus; }
  void set_max_brightness(float max_brightness);
  void set_external_antenna(bool external) { this->external_antenna_ = external; }
  void set_boot_pin(uint8_t pin) { this->boot_pin_ = pin; }
  void set_rf_switch_pin(uint8_t pin) { this->rf_switch_pin_ = pin; }
  void set_antenna_select_pin(uint8_t pin) { this->antenna_select_pin_ = pin; }
  void set_poll_interval_ms(uint32_t ms) { this->poll_interval_ms_ = ms; }
  void set_debounce_ms(uint32_t ms) { this->debounce_ms_ = ms; }
  void set_long_press_ms(uint32_t ms) { this->long_press_ms_ = ms; }
  void set_retry_interval_ms(uint32_t ms) { this->retry_interval_ms_ = ms; }
  void set_factory_reset_hold_ms(uint32_t ms) { this->factory_reset_hold_ms_ = ms; }

  void register_button_event(uint8_t idx, MilieuButtonEvent *ev);
  void register_button_light(uint8_t idx, MilieuButtonLight *light);

  void on_light_state(uint8_t idx, bool is_on, float r, float g, float b);
  void set_ota_active(bool active);
  bool is_api_connected() const;

 protected:
  bool probe_button(uint8_t addr);
  uint8_t scan_buttons();
  void apply_button_mask(uint8_t mask, uint32_t now);
  bool set_button_rgb(i2c::I2CDevice &dev, uint8_t r, uint8_t g, uint8_t b);
  bool read_button_pressed(i2c::I2CDevice &dev, bool &pressed);
  void disconnect_slot(ButtonSlot &slot, uint32_t now);

  NodeState get_node_state() const;
  void check_factory_reset(uint32_t now);
  void update_buttons(NodeState state, uint32_t now);
  void update_leds(NodeState state, uint32_t now);

  i2c::I2CBus *bus_{nullptr};
  i2c::I2CDevice probe_dev_;
  ESPPreferenceObject mask_pref_;

  ButtonSlot slots_[MAX_BUTTONS];
  uint8_t button_count_{0};
  uint8_t button_mask_{0};
  bool scanning_for_buttons_{false};
  uint32_t last_scan_ms_{0};

  uint8_t max_pwm_{LED_PWM_STATUS};
  bool external_antenna_{false};
  uint8_t boot_pin_{9};
  uint8_t rf_switch_pin_{3};
  uint8_t antenna_select_pin_{14};

  uint32_t poll_interval_ms_{20};
  uint32_t debounce_ms_{50};
  uint32_t long_press_ms_{1000};
  uint32_t retry_interval_ms_{5000};
  uint32_t factory_reset_hold_ms_{5000};

  uint32_t last_poll_ms_{0};
  uint32_t boot_hold_start_ms_{0};
  bool boot_held_{false};
  bool ota_active_{false};
};

}  // namespace milieu_panel
}  // namespace esphome
