"""Centurion (MyCentsys Pro) BLE gate controller - Vantage / V-Series.

Reverse-engineered protocol, see RECON.md. Hub component: handles auth,
keep-alive, status overview stream, RAM info reads and triggers.
"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import ble_client, sensor, text_sensor, binary_sensor
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_VOLTAGE,
    DEVICE_CLASS_DISTANCE,
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_PROBLEM,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_SAFETY,
    DEVICE_CLASS_LOCK,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_VOLT,
    UNIT_MILLIMETER,
)

DEPENDENCIES = ["ble_client"]
AUTO_LOAD = ["sensor", "text_sensor", "binary_sensor"]
CODEOWNERS = ["@henk"]

centurion_ns = cg.esphome_ns.namespace("centurion_gate")
CenturionGate = centurion_ns.class_(
    "CenturionGate", cg.Component, ble_client.BLEClientNode
)

CONF_PHONE = "phone_number"
CONF_KEY = "cipher_key"
CONF_KEEPALIVE = "keepalive_interval"

SENSORS = {
    "battery_voltage": sensor.sensor_schema(
        unit_of_measurement=UNIT_VOLT, accuracy_decimals=2,
        device_class=DEVICE_CLASS_VOLTAGE, state_class=STATE_CLASS_MEASUREMENT),
    "master_position": sensor.sensor_schema(
        unit_of_measurement=UNIT_MILLIMETER, accuracy_decimals=0,
        device_class=DEVICE_CLASS_DISTANCE, state_class=STATE_CLASS_MEASUREMENT,
        icon="mdi:gate-arrow-right"),
    "slave_position": sensor.sensor_schema(
        unit_of_measurement=UNIT_MILLIMETER, accuracy_decimals=0,
        device_class=DEVICE_CLASS_DISTANCE, state_class=STATE_CLASS_MEASUREMENT,
        icon="mdi:gate-arrow-left"),
    "master_encoder": sensor.sensor_schema(
        accuracy_decimals=0, state_class=STATE_CLASS_MEASUREMENT,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:counter"),
    "slave_encoder": sensor.sensor_schema(
        accuracy_decimals=0, state_class=STATE_CLASS_MEASUREMENT,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:counter"),
    "trigger_counter": sensor.sensor_schema(
        accuracy_decimals=0, state_class=STATE_CLASS_TOTAL_INCREASING,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:counter"),
    "last_trigger": sensor.sensor_schema(
        accuracy_decimals=0, entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        icon="mdi:gesture-tap"),
}
TEXT_SENSORS = {
    "gate_status": text_sensor.text_sensor_schema(icon="mdi:gate"),
    "master_status": text_sensor.text_sensor_schema(icon="mdi:gate"),
    "slave_status": text_sensor.text_sensor_schema(icon="mdi:gate"),
    "comms_version": text_sensor.text_sensor_schema(
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:bluetooth"),
    "core_version": text_sensor.text_sensor_schema(
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:chip"),
    "serial_number": text_sensor.text_sensor_schema(
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:identifier"),
    "health_general": text_sensor.text_sensor_schema(
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:heart-pulse"),
    "health_controller": text_sensor.text_sensor_schema(
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:chip"),
    "health_battery": text_sensor.text_sensor_schema(
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:car-battery"),
    "health_power_supply": text_sensor.text_sensor_schema(
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:power-plug"),
    "health_motors": text_sensor.text_sensor_schema(
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:engine"),
}
BINARY_SENSORS = {
    "holiday_lock": binary_sensor.binary_sensor_schema(icon="mdi:lock"),
    "frx": binary_sensor.binary_sensor_schema(icon="mdi:exit-run"),
    "keep_open": binary_sensor.binary_sensor_schema(icon="mdi:gate-open"),
    "mains_fail": binary_sensor.binary_sensor_schema(device_class=DEVICE_CLASS_PROBLEM),
    "power_low": binary_sensor.binary_sensor_schema(device_class=DEVICE_CLASS_PROBLEM),
    "irb_armed": binary_sensor.binary_sensor_schema(
        device_class=DEVICE_CLASS_SAFETY, entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
    "beam_open": binary_sensor.binary_sensor_schema(device_class=DEVICE_CLASS_SAFETY),
    "gate_lock": binary_sensor.binary_sensor_schema(icon="mdi:lock-outline"),
    "fault": binary_sensor.binary_sensor_schema(device_class=DEVICE_CLASS_PROBLEM),
    "connected": binary_sensor.binary_sensor_schema(
        device_class=DEVICE_CLASS_CONNECTIVITY, entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
}


def _hex_key(value):
    value = cv.string_strict(value).replace(":", "").replace(" ", "").lower()
    if len(value) != 8:
        raise cv.Invalid("cipher_key must be 4 bytes (8 hex chars), e.g. e9617009")
    try:
        bytes.fromhex(value)
    except ValueError as err:
        raise cv.Invalid("cipher_key must be hex") from err
    return value


schema = {
    cv.GenerateID(): cv.declare_id(CenturionGate),
    cv.Required(CONF_PHONE): cv.string_strict,
    cv.Optional(CONF_KEY): _hex_key,
    cv.Optional(CONF_KEEPALIVE, default="8s"): cv.positive_time_period_milliseconds,
}
for k, s in SENSORS.items():
    schema[cv.Optional(k)] = s
for k, s in TEXT_SENSORS.items():
    schema[cv.Optional(k)] = s
for k, s in BINARY_SENSORS.items():
    schema[cv.Optional(k)] = s

CONFIG_SCHEMA = (
    cv.Schema(schema)
    .extend(cv.COMPONENT_SCHEMA)
    .extend(ble_client.BLE_CLIENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)
    cg.add(var.set_phone_number(config[CONF_PHONE]))
    if CONF_KEY in config:
        k = bytes.fromhex(config[CONF_KEY])
        cg.add(var.set_cipher_key(k[0], k[1], k[2], k[3]))
    cg.add(var.set_keepalive_interval(config[CONF_KEEPALIVE]))
    for k in SENSORS:
        if k in config:
            s = await sensor.new_sensor(config[k])
            cg.add(getattr(var, f"set_{k}_sensor")(s))
    for k in TEXT_SENSORS:
        if k in config:
            s = await text_sensor.new_text_sensor(config[k])
            cg.add(getattr(var, f"set_{k}_text_sensor")(s))
    for k in BINARY_SENSORS:
        if k in config:
            s = await binary_sensor.new_binary_sensor(config[k])
            cg.add(getattr(var, f"set_{k}_binary_sensor")(s))
