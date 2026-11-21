# wakepsx-on-bt

[![HACS Custom](https://img.shields.io/badge/HACS-Custom-41BDF5?style=for-the-badge&logo=homeassistantcommunitystore)](https://hacs.xyz/)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg?style=for-the-badge)](https://opensource.org/licenses/MIT)
[![ESPHome](https://img.shields.io/badge/ESPHome-External%20Component-blue?style=for-the-badge&logo=esphome)](esphome/)

Wake up your PlayStation (PS3, PS4, PS5) from Home Assistant over Bluetooth.

> Fork of [FreeTHX/wakepsx-on-bt](https://github.com/FreeTHX/wakepsx-on-bt), packaged so nothing has to be downloaded or copied by hand:
> the Home Assistant integration installs through **HACS** and the ESP32 firmware is an **ESPHome external component** pulled straight from GitHub.

| Component | Description | Documentation |
|---|---|---|
| **ESPHome external component** | ESP32 that wakes the console and reports whether it is on, in rest mode or off. Brings its own **Wake console** button, MAC fields and sensors; works on its own. | [esphome/](esphome/README.md) |
| **MAC reader** | Web page that reads the controller and console MACs from a controller plugged in over USB | [victorigualada.github.io/wakepsx-on-bt](https://victorigualada.github.io/wakepsx-on-bt/) |
| **HA integration** (HACS, optional) | Reads the MACs from a controller plugged into the HA host, and can wake from the host's own Bluetooth adapter when there is no ESP32 | [custom_components/wakepsx_on_bt/](custom_components/wakepsx_on_bt/README.md) |

## With an ESP32 only (recommended)

1. **Flash the ESP32** with the package below (classic ESP32 only: WROOM-32/WROVER, `esp32dev`).
2. **Read the MACs**: plug the controller paired with the console into a computer over USB and open the [MAC reader](https://victorigualada.github.io/wakepsx-on-bt/) in Chrome or Edge.
3. **Paste them** into **Controller MAC** and **Console MAC** on the ESP32's device page in Home Assistant (or set the `controller_mac` / `console_mac` substitutions in the node's YAML).
4. Press **Wake console**. It is a normal button entity: use it in dashboards, scripts and automations.

Set **Console IP** on the same page (give the console a fixed IP on your router) and the node also shows **Console state** (`On` / `Rest` / `Off`) and **Console power**, asked over the local network ([details](esphome/README.md#console-state)). Set **Remote Play IPs** to the devices you play from (an Apple TV with Asobi, a phone with PS Remote Play; IPs or ranges) and the ESP32 wakes the console as soon as you open Remote Play there ([details](esphome/README.md#remote-play-request)).

No integration to install. The rest of this page covers the optional integration.

## 1. Install the integration with HACS (optional)

The integration relies on **[pywakepsx-on-bt](https://github.com/FreeTHX/pywakepsx-on-bt)**, a Python library for HCI-level Bluetooth operations and MAC extraction from USB controllers. Home Assistant installs it automatically.

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

The package brings the external component, the `esp32dev` board with ESP-IDF, the Bluetooth sdkconfig options, the **Wake console** button, the **Controller MAC** / **Console MAC** fields, the **Console state** sensors, the **Remote Play request** sensor and its switch, the `PSX Wake Status` sensor and the `wake_psx` action that the integration calls. A ready-to-use node file is in [esphome/esp-wakepsx-on-bt.yaml](esphome/esp-wakepsx-on-bt.yaml). Details and the standalone component reference: [esphome/README.md](esphome/README.md).

## 3. Add the integration

**Settings → Devices & Services → Add Integration → Wake PSX on Bluetooth**, then follow the config flow (connect your controller via USB when prompted and pick the ESPHome node). Every wake the integration sends through the ESP32 also stores the MACs in the node's **Controller MAC** / **Console MAC** fields, so the node's own **Wake console** button works too, and keeps working if you remove the integration later.

## Releasing

Publish a release on GitHub from the latest commit of `main`, with a tag like `v1.2.3`, and write the notes as usual. The [Release workflow](.github/workflows/release.yml) then runs on its own: it bumps the version on `main` and moves the tag to a commit where the ESPHome package pulls the component from that same tag, so pinning `…/wakepsx-on-bt.yaml@v0.6.0` pins everything.

## License

MIT License
