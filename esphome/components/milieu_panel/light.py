import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import light
from esphome.const import CONF_OUTPUT_ID

from . import CONF_MILIEU_PANEL_ID, CONF_SLOT, MilieuPanel, milieu_panel_ns

DEPENDENCIES = ["milieu_panel"]

MilieuButtonLight = milieu_panel_ns.class_("MilieuButtonLight", light.LightOutput)

CONFIG_SCHEMA = light.RGB_LIGHT_SCHEMA.extend(
    {
        cv.GenerateID(CONF_OUTPUT_ID): cv.declare_id(MilieuButtonLight),
        cv.GenerateID(CONF_MILIEU_PANEL_ID): cv.use_id(MilieuPanel),
        cv.Required(CONF_SLOT): cv.int_range(min=1, max=8),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_MILIEU_PANEL_ID])
    var = cg.new_Pvariable(config[CONF_OUTPUT_ID])
    await light.register_light(var, config)
    cg.add(var.set_parent(hub, config[CONF_SLOT] - 1))
    cg.add(hub.register_button_light(config[CONF_SLOT] - 1, var))
