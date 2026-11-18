# wakepsx-on-bt

[![HACS Custom](https://img.shields.io/badge/HACS-Custom-41BDF5?style=for-the-badge&logo=homeassistantcommunitystore)](https://hacs.xyz/)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg?style=for-the-badge)](https://opensource.org/licenses/MIT)
[![ESPHome](https://img.shields.io/badge/ESPHome-External%20Component-blue?style=for-the-badge&logo=esphome)](esphome/)

Wake up your PlayStation (PS3, PS4, PS5) from Home Assistant over Bluetooth.

> Fork of [FreeTHX/wakepsx-on-bt](https://github.com/FreeTHX/wakepsx-on-bt), packaged so nothing has to be downloaded or copied by hand:
> the Home Assistant integration installs through **HACS** and the ESP32 firmware is an **ESPHome external component** pulled straight from GitHub.

| Component | Description | Documentation |
|---|---|---|
| **HA integration** (HACS) | Button entity, config flow, wake logic | [custom_components/wakepsx_on_bt/](custom_components/wakepsx_on_bt/README.md) |
| **ESPHome external component** | ESP32 gateway for reliable Bluetooth wake (recommended method) | [esphome/](esphome/README.md) |

The integration relies on **[pywakepsx-on-bt](https://github.com/FreeTHX/pywakepsx-on-bt)**, a Python library for HCI-level Bluetooth operations and MAC extraction from USB controllers. Home Assistant installs it automatically.

## 1. Install the integration with HACS

[![Open your Home Assistant instance and open this repository inside HACS.](https://my.home-assistant.io/badges/hacs_repository.svg)](https://my.home-assistant.io/redirect/hacs_repository/?owner=victorigualada&repository=wakepsx-on-bt&category=integration)

Or by hand: **HACS → ⋮ → Custom repositories**, add `https://github.com/victorigualada/wakepsx-on-bt` with type **Integration**, then download **Wake PSX on Bluetooth** and restart Home Assistant.

## 2. Flash the ESP32 (ESPHome method)

Use a **classic ESP32** (WROOM-32/WROVER, `esp32dev`). S2/S3/C3/C6/H2 have no Bluetooth Classic. In your ESPHome node config, add one package:

```yaml
esphome:
  name: esp-wakepsx-on-bt
  friendly_name: Wake PSX on Bluetooth

packages:
  wakepsx: github://victorigualada/wakepsx-on-bt/esphome/wakepsx-on-bt.yaml@main

logger:
api:
  encryption:
    key: !secret api_encryption_key
ota:
  - platform: esphome
wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
```

The package brings the external component, the `esp32dev` board with ESP-IDF, the Bluetooth sdkconfig options, the `PSX Wake Status` sensor and the `wake_psx` action that the integration calls. A ready-to-use node file is in [esphome/esp-wakepsx-on-bt.yaml](esphome/esp-wakepsx-on-bt.yaml). Details and the standalone component reference: [esphome/README.md](esphome/README.md).

## 3. Add the integration

**Settings → Devices & Services → Add Integration → Wake PSX on Bluetooth**, then follow the config flow (connect your controller via USB when prompted and pick the ESPHome node).

## License

MIT License
