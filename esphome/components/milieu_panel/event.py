import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import event
from esphome.const import CONF_ID

from . import CONF_MILIEU_PANEL_ID, CONF_SLOT, MilieuPanel, milieu_panel_ns

DEPENDENCIES = ["milieu_panel"]

BUTTON_EVENT_TYPES = [
    "initial_press",
    "short_release",
    "long_press",
    "long_release",
]

MilieuButtonEvent = milieu_panel_ns.class_("MilieuButtonEvent", event.Event)

CONFIG_SCHEMA = event.event_schema(MilieuButtonEvent).extend(
    {
        cv.GenerateID(CONF_MILIEU_PANEL_ID): cv.use_id(MilieuPanel),
        cv.Required(CONF_SLOT): cv.int_range(min=1, max=8),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_MILIEU_PANEL_ID])
    var = cg.new_Pvariable(config[CONF_ID])
    await event.register_event(var, config, event_types=BUTTON_EVENT_TYPES)
    cg.add(hub.register_button_event(config[CONF_SLOT] - 1, var))
