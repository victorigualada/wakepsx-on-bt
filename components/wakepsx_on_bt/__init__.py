"""ESPHome external component: wake a PlayStation (PS3/PS4/PS5) over Bluetooth Classic.

The ESP32 spoofs the Bluetooth address of a controller already paired with the
console and sends a raw HCI Create Connection through the controller's Virtual
HCI interface. No Bluetooth host stack (Bluedroid/NimBLE) is used.
"""

import esphome.codegen as cg
from esphome import automation
from esphome.components import esp32, text_sensor
from esphome.components.esp32 import add_idf_sdkconfig_option
from esphome.components.esp32.const import VARIANT_ESP32
import esphome.config_validation as cv
from esphome.const import CONF_ID, Framework
from esphome.core import CORE

CODEOWNERS = ["@FreeTHX", "@victorigualada"]
DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["text_sensor"]
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

wakepsx_on_bt_ns = cg.esphome_ns.namespace("wakepsx_on_bt")
WakePsxOnBt = wakepsx_on_bt_ns.class_("WakePsxOnBt", cg.Component)
WakeAction = wakepsx_on_bt_ns.class_("WakeAction", automation.Action)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(WakePsxOnBt),
            cv.Optional(CONF_STATUS, default={"name": "PSX Wake Status"}): text_sensor.text_sensor_schema(
                icon="mdi:sony-playstation",
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    esp32.only_on_variant(
        supported=[VARIANT_ESP32],
        msg_prefix="wakepsx_on_bt needs Bluetooth Classic, which",
    ),
    cv.only_with_framework(Framework.ESP_IDF),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    sens = await text_sensor.new_text_sensor(config[CONF_STATUS])
    cg.add(var.set_status_sensor(sens))

    _request_bt_controller()

    # Bring up only the Classic BT controller, reached through Virtual HCI.
    # Dropping Bluedroid/NimBLE frees RAM and leaves the HCI transport to us.
    add_idf_sdkconfig_option("CONFIG_BT_ENABLED", True)
    add_idf_sdkconfig_option("CONFIG_BT_CONTROLLER_ONLY", True)
    add_idf_sdkconfig_option("CONFIG_BT_BLUEDROID_ENABLED", False)
    add_idf_sdkconfig_option("CONFIG_BT_NIMBLE_ENABLED", False)
    add_idf_sdkconfig_option("CONFIG_BTDM_CTRL_HCI_MODE_VHCI", True)
    add_idf_sdkconfig_option("CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY", True)


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
