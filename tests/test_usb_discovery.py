"""USB discovery offers one flow per console, also when a console is already set up."""

from unittest.mock import patch

from homeassistant.config_entries import SOURCE_IGNORE, SOURCE_USB
from homeassistant.core import HomeAssistant
from homeassistant.data_entry_flow import FlowResultType
from homeassistant.helpers.service_info.usb import UsbServiceInfo
from pytest_homeassistant_custom_component.common import MockConfigEntry

from custom_components.wakepsx_on_bt.const import DOMAIN

PS5_A = "AA:AA:AA:AA:AA:01"
PS5_B = "BB:BB:BB:BB:BB:02"
USB_INFO = UsbServiceInfo(
    device="/dev/hidraw0",
    vid="054C",
    pid="0CE6",
    serial_number=None,
    manufacturer="Sony Interactive Entertainment",
    description="DualSense Wireless Controller",
)


def _controller(psx_mac: str) -> dict[str, str]:
    return {
        "psx_mac": psx_mac,
        "dsx_mac": "C0:C0:C0:C0:C0:01",
        "controller_type": "dualsense",
    }


def _extract(*macs: str):
    return patch(
        "custom_components.wakepsx_on_bt.config_flow.extract_psx_bt_macs",
        return_value=[_controller(m) for m in macs],
    )


async def _start(hass: HomeAssistant):
    return await hass.config_entries.flow.async_init(
        DOMAIN, context={"source": SOURCE_USB}, data=USB_INFO
    )


async def test_second_console_is_discovered(hass: HomeAssistant) -> None:
    MockConfigEntry(domain=DOMAIN, unique_id=PS5_A.lower(), data={}).add_to_hass(hass)

    with _extract(PS5_B):
        result = await _start(hass)

    assert result["type"] is FlowResultType.FORM
    assert result["step_id"] == "usb_confirm"
    flow = hass.config_entries.flow.async_get(result["flow_id"])
    assert flow["context"]["unique_id"] == PS5_B.lower()


async def test_configured_console_is_not_rediscovered(hass: HomeAssistant) -> None:
    MockConfigEntry(domain=DOMAIN, unique_id=PS5_A.lower(), data={}).add_to_hass(hass)

    with _extract(PS5_A):
        result = await _start(hass)

    assert result["type"] is FlowResultType.ABORT
    assert result["reason"] == "already_configured"


async def test_ignored_console_stays_ignored(hass: HomeAssistant) -> None:
    MockConfigEntry(
        domain=DOMAIN, unique_id=PS5_B.lower(), source=SOURCE_IGNORE, data={}
    ).add_to_hass(hass)

    with _extract(PS5_B):
        result = await _start(hass)

    assert result["type"] is FlowResultType.ABORT


async def test_new_controller_is_picked_when_several_are_plugged(
    hass: HomeAssistant,
) -> None:
    MockConfigEntry(domain=DOMAIN, unique_id=PS5_A.lower(), data={}).add_to_hass(hass)

    with _extract(PS5_A, PS5_B):
        result = await _start(hass)

    flow = hass.config_entries.flow.async_get(result["flow_id"])
    assert flow["context"]["unique_id"] == PS5_B.lower()


async def test_unreadable_controller_aborts(hass: HomeAssistant) -> None:
    with patch(
        "custom_components.wakepsx_on_bt.config_flow.extract_psx_bt_macs",
        side_effect=OSError("permission denied"),
    ):
        result = await _start(hass)

    assert result["type"] is FlowResultType.ABORT
    assert result["reason"] == "extraction_error"
