"""ESPHome external component: wake a PlayStation (PS3/PS4/PS5) over Bluetooth Classic.

The ESP32 spoofs the Bluetooth address of a controller already paired with the
console and sends a raw HCI Create Connection through the controller's Virtual
HCI interface. No Bluetooth host stack (Bluedroid/NimBLE) is used.

Optionally (`console:`), it also reports whether the console is on, in rest mode
or off, asking it at its IP over the local network with DDP.
"""

import esphome.codegen as cg
from esphome import automation
from esphome.components import binary_sensor, esp32, text_sensor
from esphome.components.esp32 import add_idf_sdkconfig_option
from esphome.components.esp32.const import VARIANT_ESP32
import esphome.config_validation as cv
from esphome.const import (
    CONF_ADDRESS,
    CONF_ID,
    DEVICE_CLASS_POWER,
    ENTITY_CATEGORY_DIAGNOSTIC,
    Framework,
)
from esphome.core import CORE

CODEOWNERS = ["@FreeTHX", "@victorigualada"]
DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["text_sensor", "binary_sensor", "network"]
# This component owns the Bluetooth controller (Classic-only, VHCI, no host stack),
# so it cannot coexist with anything that brings up BLE or a host stack.
CONFLICTS_WITH = [
    "esp32_ble",
    "esp32_ble_tracker",
    "esp32_ble_server",
    "esp32_ble_beacon",
    "esp32_ble_client",
    "bluetooth_proxy",
    "ble_client",
]

CONF_STATUS = "status"
CONF_DSX_MAC = "dsx_mac"
CONF_PSX_MAC = "psx_mac"
CONF_CONSOLE = "console"
CONF_HOST_ID = "host_id"
CONF_STATE = "state"
CONF_POWER = "power"
CONF_HOST_NAME = "host_name"
CONF_SYSTEM_VERSION = "system_version"

wakepsx_on_bt_ns = cg.esphome_ns.namespace("wakepsx_on_bt")
WakePsxOnBt = wakepsx_on_bt_ns.class_("WakePsxOnBt", cg.Component)
WakeAction = wakepsx_on_bt_ns.class_("WakeAction", automation.Action)
ConsoleMonitor = wakepsx_on_bt_ns.class_("ConsoleMonitor", cg.PollingComponent)


def _console_address(value):
    """An IPv4 address. Empty is accepted (the state then reads "No IP") so the package can
    default to "" and take the address from its Console IP text entity instead."""
    value = cv.string_strict(value).strip()
    if not value:
        return ""
    return str(cv.ipv4address(value))


def _host_id(value):
    """DDP host-id: 12 hex digits; ':' or '-' separators are accepted and dropped."""
    value = cv.string_strict(value).strip().replace(":", "").replace("-", "").upper()
    if not value:
        return ""
    if len(value) != 12 or any(c not in "0123456789ABCDEF" for c in value):
        raise cv.Invalid("host_id must be 12 hexadecimal digits, like D4F7D543E740")
    return value


CONSOLE_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ConsoleMonitor),
        cv.Optional(CONF_ADDRESS, default=""): cv.templatable(_console_address),
        cv.Optional(CONF_HOST_ID, default=""): _host_id,
        cv.Optional(CONF_STATE, default={"name": "Console state"}): text_sensor.text_sensor_schema(
            icon="mdi:sony-playstation",
        ),
        cv.Optional(CONF_POWER, default={"name": "Console power"}): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_POWER,
        ),
        cv.Optional(CONF_HOST_NAME): text_sensor.text_sensor_schema(
            icon="mdi:label-outline",
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_SYSTEM_VERSION): text_sensor.text_sensor_schema(
            icon="mdi:package-up",
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    }
).extend(cv.polling_component_schema("10s"))


def _consume_console_socket(config):
    """Reserve the UDP socket the console monitor keeps open (ESPHome sizes lwIP from this)."""
    if CONF_CONSOLE in config:
        try:
            from esphome.components import socket
        except ImportError:
            return config
        if hasattr(socket, "consume_sockets") and hasattr(socket, "SocketType"):
            socket.consume_sockets(1, "wakepsx_on_bt", socket.SocketType.UDP)(config)
    return config

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(WakePsxOnBt),
            cv.Optional(CONF_STATUS, default={"name": "PSX Wake Status"}): text_sensor.text_sensor_schema(
                icon="mdi:sony-playstation",
            ),
            cv.Optional(CONF_CONSOLE): CONSOLE_SCHEMA,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    esp32.only_on_variant(
        supported=[VARIANT_ESP32],
        msg_prefix="wakepsx_on_bt needs Bluetooth Classic, which",
    ),
    cv.only_with_framework(Framework.ESP_IDF),
    _consume_console_socket,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    sens = await text_sensor.new_text_sensor(config[CONF_STATUS])
    cg.add(var.set_status_sensor(sens))

    _request_bt_controller()

    if console := config.get(CONF_CONSOLE):
        await _console_to_code(console)

    # Bring up only the Classic BT controller, reached through Virtual HCI.
    # Dropping Bluedroid/NimBLE frees RAM and leaves the HCI transport to us.
    add_idf_sdkconfig_option("CONFIG_BT_ENABLED", True)
    add_idf_sdkconfig_option("CONFIG_BT_CONTROLLER_ONLY", True)
    add_idf_sdkconfig_option("CONFIG_BT_BLUEDROID_ENABLED", False)
    add_idf_sdkconfig_option("CONFIG_BT_NIMBLE_ENABLED", False)
    add_idf_sdkconfig_option("CONFIG_BTDM_CTRL_HCI_MODE_VHCI", True)
    add_idf_sdkconfig_option("CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY", True)


async def _console_to_code(config):
    mon = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(mon, config)
    address = await cg.templatable(config[CONF_ADDRESS], [], cg.std_string)
    cg.add(mon.set_address(address))
    if config[CONF_HOST_ID]:
        cg.add(mon.set_host_id(config[CONF_HOST_ID]))

    sens = await text_sensor.new_text_sensor(config[CONF_STATE])
    cg.add(mon.set_state_sensor(sens))
    sens = await binary_sensor.new_binary_sensor(config[CONF_POWER])
    cg.add(mon.set_power_sensor(sens))
    if conf := config.get(CONF_HOST_NAME):
        sens = await text_sensor.new_text_sensor(conf)
        cg.add(mon.set_host_name_sensor(sens))
    if conf := config.get(CONF_SYSTEM_VERSION):
        sens = await text_sensor.new_text_sensor(conf)
        cg.add(mon.set_system_version_sensor(sens))


def _request_bt_controller():
    """Make sure the ESP-IDF `bt` component is built (ESPHome trims it by default)."""
    if hasattr(esp32, "request_bluetooth"):  # ESPHome 2026.x
        esp32.request_bluetooth()
        # The controller shares the radio with WiFi; keep software coexistence on.
        if "wifi" in CORE.config and hasattr(esp32, "request_software_coexistence"):
            esp32.request_software_coexistence()
    elif hasattr(esp32, "include_builtin_idf_component"):
        esp32.include_builtin_idf_component("bt")


@automation.register_action(
    "wakepsx_on_bt.wake",
    WakeAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(WakePsxOnBt),
            cv.Required(CONF_DSX_MAC): cv.templatable(cv.string),
            cv.Required(CONF_PSX_MAC): cv.templatable(cv.string),
        }
    ),
    # play() copies both MACs and hands them to a background task before returning.
    synchronous=True,
)
async def wake_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    dsx = await cg.templatable(config[CONF_DSX_MAC], args, cg.std_string)
    psx = await cg.templatable(config[CONF_PSX_MAC], args, cg.std_string)
    cg.add(var.set_dsx_mac(dsx))
    cg.add(var.set_psx_mac(psx))
    return var
