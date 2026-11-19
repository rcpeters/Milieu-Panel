import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import i2c
from esphome.const import CONF_ID

CODEOWNERS = ["@rcpeters"]
DEPENDENCIES = ["i2c", "preferences", "esp32"]
AUTO_LOAD = ["event", "light"]

CONF_I2C_ID = "i2c_id"
CONF_POWER_RAIL_5V = "power_rail_5v"
CONF_MAX_BRIGHTNESS = "max_brightness"
CONF_EXTERNAL_ANTENNA = "external_antenna"
CONF_BOOT_PIN = "boot_pin"
CONF_RF_SWITCH_PIN = "rf_switch_pin"
CONF_ANTENNA_SELECT_PIN = "antenna_select_pin"
CONF_POLL_INTERVAL = "poll_interval"
CONF_DEBOUNCE_TIME = "debounce_time"
CONF_LONG_PRESS_TIME = "long_press_time"
CONF_RETRY_INTERVAL = "retry_interval"
CONF_FACTORY_RESET_HOLD_TIME = "factory_reset_hold_time"
CONF_MILIEU_PANEL_ID = "milieu_panel_id"
CONF_SLOT = "slot"

milieu_panel_ns = cg.esphome_ns.namespace("milieu_panel")
MilieuPanel = milieu_panel_ns.class_("MilieuPanel", cg.Component)

def _validate_brightness_budget(config):
    if not config[CONF_POWER_RAIL_5V] and config[CONF_MAX_BRIGHTNESS] > 0.48:
        raise cv.Invalid(
            "max_brightness above 48% exceeds the XIAO ESP32-C6 3.3V power budget. "
            "Set power_rail_5v: true ONLY when buttons are powered at 5V through an "
            "I2C level shifter (see 5V_UPGRADE_README.md)."
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(MilieuPanel),
            cv.Required(CONF_I2C_ID): cv.use_id(i2c.I2CBus),
            cv.Optional(CONF_POWER_RAIL_5V, default=False): cv.boolean,
            cv.Optional(CONF_MAX_BRIGHTNESS, default="10%"): cv.All(
                cv.percentage, cv.Range(min=0.01, max=1.0)
            ),
            cv.Optional(CONF_EXTERNAL_ANTENNA, default=False): cv.boolean,
            cv.Optional(CONF_BOOT_PIN, default=9): cv.int_range(min=0, max=30),
            cv.Optional(CONF_RF_SWITCH_PIN, default=3): cv.int_range(min=0, max=30),
            cv.Optional(CONF_ANTENNA_SELECT_PIN, default=14): cv.int_range(min=0, max=30),
            cv.Optional(
                CONF_POLL_INTERVAL, default="20ms"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_DEBOUNCE_TIME, default="50ms"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_LONG_PRESS_TIME, default="1000ms"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_RETRY_INTERVAL, default="5000ms"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_FACTORY_RESET_HOLD_TIME, default="5000ms"
            ): cv.positive_time_period_milliseconds,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_brightness_budget,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    bus = await cg.get_variable(config[CONF_I2C_ID])
    cg.add(var.set_i2c_bus(bus))
    cg.add(var.set_power_rail_5v(config[CONF_POWER_RAIL_5V]))
    cg.add(var.set_max_brightness(config[CONF_MAX_BRIGHTNESS]))
    cg.add(var.set_external_antenna(config[CONF_EXTERNAL_ANTENNA]))
    cg.add(var.set_boot_pin(config[CONF_BOOT_PIN]))
    cg.add(var.set_rf_switch_pin(config[CONF_RF_SWITCH_PIN]))
    cg.add(var.set_antenna_select_pin(config[CONF_ANTENNA_SELECT_PIN]))
    cg.add(var.set_poll_interval_ms(config[CONF_POLL_INTERVAL]))
    cg.add(var.set_debounce_ms(config[CONF_DEBOUNCE_TIME]))
    cg.add(var.set_long_press_ms(config[CONF_LONG_PRESS_TIME]))
    cg.add(var.set_retry_interval_ms(config[CONF_RETRY_INTERVAL]))
    cg.add(var.set_factory_reset_hold_ms(config[CONF_FACTORY_RESET_HOLD_TIME]))
