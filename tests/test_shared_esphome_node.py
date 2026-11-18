"""Consoles sharing one ESP32 only mirror the status of their own wake."""

from homeassistant.core import HomeAssistant, ServiceCall
from homeassistant.helpers import entity_registry as er
from pytest_homeassistant_custom_component.common import MockConfigEntry

from custom_components.wakepsx_on_bt.const import (
    CONF_CONSOLE_NAME,
    CONF_DSX_MAC,
    CONF_ESP_ENTITY,
    CONF_ESP_SERVICE,
    CONF_PSX_MAC,
    CONF_WAKE_METHOD,
    DOMAIN,
    WAKE_METHOD_ESPHOME,
)

SENSOR = "sensor.esp_wakepsx_on_bt_psx_wake_status"
SERVICE = "esp_wakepsx_on_bt_wake_psx"


async def _add_console(hass: HomeAssistant, name: str, psx_mac: str) -> str:
    entry = MockConfigEntry(
        domain=DOMAIN,
        unique_id=psx_mac.lower(),
        data={
            CONF_PSX_MAC: psx_mac,
            CONF_DSX_MAC: "C0:C0:C0:C0:C0:01",
            CONF_CONSOLE_NAME: name,
            CONF_WAKE_METHOD: WAKE_METHOD_ESPHOME,
            CONF_ESP_ENTITY: SENSOR,
            CONF_ESP_SERVICE: SERVICE,
        },
    )
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    await hass.async_block_till_done()
    unique_id = f"{DOMAIN}_{psx_mac.replace(':', '').lower()}_wake"
    entity_id = er.async_get(hass).async_get_entity_id("button", DOMAIN, unique_id)
    assert entity_id
    return entity_id


def _text(hass: HomeAssistant, entity_id: str) -> str:
    return hass.states.get(entity_id).attributes["last_wake_status_text"]


async def _press(hass: HomeAssistant, entity_id: str) -> None:
    await hass.services.async_call(
        "button", "press", {"entity_id": entity_id}, blocking=True
    )


async def _sensor(hass: HomeAssistant, state: str) -> None:
    hass.states.async_set(SENSOR, state)
    await hass.async_block_till_done()


async def test_status_only_reaches_the_console_that_pressed(
    hass: HomeAssistant,
) -> None:
    calls: list[ServiceCall] = []
    hass.services.async_register("esphome", SERVICE, calls.append)
    await _sensor(hass, "Ready")
    ps5 = await _add_console(hass, "PS5", "AA:AA:AA:AA:AA:01")
    ps4 = await _add_console(hass, "PS4", "BB:BB:BB:BB:BB:02")

    await _press(hass, ps5)
    assert calls[0].data["psx_mac"] == "AA:AA:AA:AA:AA:01"
    await _sensor(hass, "Connecting...")
    await _sensor(hass, "Success")

    assert _text(hass, ps5) == "Success"
    assert _text(hass, ps4) == "Ready"

    # The PS4 button is pressed while the ESP32 still shows the PS5 result.
    await _press(hass, ps4)
    assert len(calls) == 1, "must not call the ESP32 while it is busy"
    assert _text(hass, ps4) == "Busy"

    await _sensor(hass, "Ready")
    assert _text(hass, ps5) == "Ready"

    # Once the ESP32 is free the PS4 wakes normally, and the PS5 is not touched.
    await _press(hass, ps4)
    assert calls[1].data["psx_mac"] == "BB:BB:BB:BB:BB:02"
    await _sensor(hass, "Connecting...")
    await _sensor(hass, "Page Timeout")
    assert _text(hass, ps4) == "Page Timeout"
    assert _text(hass, ps5) == "Ready"


async def test_busy_from_firmware_ends_the_window(hass: HomeAssistant) -> None:
    hass.services.async_register("esphome", SERVICE, lambda call: None)
    await _sensor(hass, "Ready")
    ps5 = await _add_console(hass, "PS5", "AA:AA:AA:AA:AA:01")

    await _press(hass, ps5)
    await _sensor(hass, "Busy")
    assert _text(hass, ps5) == "Busy"

    # A result after Busy belongs to whatever the ESP32 was already doing.
    await _sensor(hass, "Success")
    assert _text(hass, ps5) == "Busy"
