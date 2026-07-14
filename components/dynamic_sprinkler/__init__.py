import gzip
from pathlib import Path

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button, number, select, sensor, sun, switch, text_sensor, time
from esphome.const import CONF_ENTITY_ID, CONF_ID, CONF_NAME

AUTO_LOAD = ["button", "json", "number", "sensor", "web_server_base"]
DEPENDENCIES = ["api", "web_server", "time"]

CONF_TIME_ID = "time_id"
CONF_SUN_ID = "sun_id"
CONF_ZONES = "zones"
CONF_ZONE_ID = "zone_id"
CONF_RELAY = "relay"
CONF_BUTTON = "button"
CONF_WEATHER = "weather"
CONF_WIND = "wind"
CONF_GUST = "gust"
CONF_RAIN_24H = "rain_24h"
CONF_RAIN_RATE = "rain_rate"
CONF_TEMPERATURE = "temperature"
CONF_STATUS_TEXT = "status_text"
CONF_DECISION_TEXT = "decision_text"
CONF_REMAINING_SENSOR = "remaining_sensor"
CONF_TIMEZONE_SELECT = "timezone_select"
CONF_SELECTED_ENABLED_SWITCH = "selected_enabled_switch"
CONF_MANUAL_DURATION = "manual_duration"

dynamic_ns = cg.esphome_ns.namespace("dynamic_sprinkler")
DynamicSprinkler = dynamic_ns.class_("DynamicSprinkler", cg.Component)
DynamicZoneButton = dynamic_ns.class_("DynamicZoneButton", button.Button)
DynamicWeatherSensor = dynamic_ns.class_("DynamicWeatherSensor", sensor.Sensor, cg.Component)

WEB_ASSET_DIR = Path(__file__).resolve().parents[2] / "web"


def add_web_resource(resource_name: str, filename: str) -> None:
    """Embed a repository asset from the external-component checkout."""
    content = (WEB_ASSET_DIR / filename).read_bytes()
    compressed = gzip.compress(content, mtime=0)
    values = ", ".join(str(value) for value in compressed)
    cg.add_global(
        cg.RawExpression(
            f"constexpr uint8_t ESPHOME_WEBSERVER_{resource_name}[{len(compressed)}] "
            f"PROGMEM = {{{values}}}"
        )
    )
    cg.add_global(
        cg.RawExpression(
            f"constexpr size_t ESPHOME_WEBSERVER_{resource_name}_SIZE = {len(compressed)}"
        )
    )


ZONE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_RELAY): cv.use_id(switch.Switch),
        cv.Optional(CONF_ZONE_ID): cv.int_range(min=1, max=32),
        cv.Optional(CONF_NAME): cv.string_strict,
        cv.Required(CONF_BUTTON): button.button_schema(
            DynamicZoneButton, icon="mdi:timer-play"
        ),
    }
)


def validate_zones(zones):
    ids = [zone.get(CONF_ZONE_ID, index + 1) for index, zone in enumerate(zones)]
    if len(ids) != len(set(ids)):
        raise cv.Invalid("zone_id values must be unique")
    return zones

WEATHER_SOURCE_SCHEMA = sensor.sensor_schema(
    DynamicWeatherSensor, accuracy_decimals=1
).extend({cv.Required(CONF_ENTITY_ID): cv.entity_id})

WEATHER_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_WIND): WEATHER_SOURCE_SCHEMA,
        cv.Optional(CONF_GUST): WEATHER_SOURCE_SCHEMA,
        cv.Optional(CONF_RAIN_24H): WEATHER_SOURCE_SCHEMA,
        cv.Optional(CONF_RAIN_RATE): WEATHER_SOURCE_SCHEMA,
        cv.Optional(CONF_TEMPERATURE): WEATHER_SOURCE_SCHEMA,
    }
)


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(DynamicSprinkler),
        cv.Required(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
        cv.Optional(CONF_SUN_ID): cv.use_id(sun.Sun),
        cv.Required(CONF_ZONES): cv.All(
            cv.ensure_list(ZONE_SCHEMA), cv.Length(min=1, max=32), validate_zones
        ),
        cv.Optional(CONF_WEATHER, default={}): WEATHER_SCHEMA,
        cv.Required(CONF_STATUS_TEXT): cv.use_id(text_sensor.TextSensor),
        cv.Required(CONF_DECISION_TEXT): cv.use_id(text_sensor.TextSensor),
        cv.Required(CONF_REMAINING_SENSOR): cv.use_id(sensor.Sensor),
        cv.Required(CONF_TIMEZONE_SELECT): cv.use_id(select.Select),
        cv.Required(CONF_SELECTED_ENABLED_SWITCH): cv.use_id(switch.Switch),
        cv.Required(CONF_MANUAL_DURATION): cv.use_id(number.Number),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    # web_server's normal *_include paths are resolved beside the user's main
    # YAML, which prevents a remote package from carrying its own assets. Read
    # them from this component's Git checkout and publish the same supported
    # PROGMEM symbols instead.
    cg.add_define("USE_WEBSERVER_CSS_INCLUDE")
    cg.add_define("USE_WEBSERVER_JS_INCLUDE")
    add_web_resource("CSS_INCLUDE", "style.css")
    add_web_resource("JS_INCLUDE", "dynamic_app.js")

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_time(await cg.get_variable(config[CONF_TIME_ID])))
    if CONF_SUN_ID in config:
        cg.add(var.set_sun(await cg.get_variable(config[CONF_SUN_ID])))
    for index, zone in enumerate(config[CONF_ZONES]):
        zone_id = zone.get(CONF_ZONE_ID, index + 1)
        default_name = f"Zone {index + 1}"
        zone_name = zone.get(CONF_NAME, default_name)
        button_config = zone[CONF_BUTTON]
        if CONF_NAME not in button_config:
            button_config[CONF_NAME] = f"{zone_name} Manual Run"
        cg.add(var.add_zone(await cg.get_variable(zone[CONF_RELAY]), zone_name, zone_id))
        zone_button = await button.new_button(button_config, var, zone_id)
        cg.add(var.set_zone_button(index, zone_button))
    weather = config[CONF_WEATHER]
    setters = {
        CONF_WIND: var.set_wind_speed,
        CONF_GUST: var.set_wind_gust,
        CONF_RAIN_24H: var.set_rain_24h,
        CONF_RAIN_RATE: var.set_rain_rate,
        CONF_TEMPERATURE: var.set_daily_temp,
    }
    for key, setter in setters.items():
        if key not in weather:
            continue
        source_config = weather[key]
        source = await sensor.new_sensor(source_config)
        await cg.register_component(source, source_config)
        cg.add(source.set_entity_id(source_config[CONF_ENTITY_ID]))
        cg.add(setter(source))
    cg.add_define("USE_API_HOMEASSISTANT_STATES")
    cg.add(var.set_status_text(await cg.get_variable(config[CONF_STATUS_TEXT])))
    cg.add(var.set_decision_text(await cg.get_variable(config[CONF_DECISION_TEXT])))
    cg.add(var.set_remaining_sensor(await cg.get_variable(config[CONF_REMAINING_SENSOR])))
    cg.add(var.set_timezone_select(await cg.get_variable(config[CONF_TIMEZONE_SELECT])))
    cg.add(var.set_selected_enabled_switch(await cg.get_variable(config[CONF_SELECTED_ENABLED_SWITCH])))
    cg.add(var.set_manual_duration(await cg.get_variable(config[CONF_MANUAL_DURATION])))
