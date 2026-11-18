# esp-wakepsx-on-bt

This directory contains the ESPHome firmware for the ESP32 gateway used by the **Wake PSX on Bluetooth** Home Assistant integration. The ESP32 acts as a Bluetooth Classic bridge: when Home Assistant sends a wake command, the ESP32 impersonates your PlayStation controller over Bluetooth and wakes the console.

---

## Before You Start

### Compatible Hardware

**You need an ESP32 with Classic Bluetooth support.** Not all ESP32 variants support this.

| Module | Classic BT | Compatible |
|---|---|---|
| ESP32 (WROOM-32, WROVER) | ✅ | ✅ **Use this** |
| ESP32-S2 | ❌ | ❌ No Bluetooth at all |
| ESP32-S3 | BLE only | ❌ BLE only, no Classic BT |
| ESP32-C3 | BLE only | ❌ BLE only, no Classic BT |
| ESP32-C6 | BLE only | ❌ BLE only, no Classic BT |
| ESP32-H2 | BLE only | ❌ BLE only, no Classic BT |

> **In short: only the original ESP32 (WROOM-32 or WROVER) works.**
> When buying a board, look for "ESP32" without any suffix, or explicitly "Classic Bluetooth" in the specs.

### What You Also Need

- **Home Assistant** with the **Wake PSX on Bluetooth** integration installed
- **ESPHome** (add-on or standalone CLI) to compile and flash the firmware
- Your **PlayStation controller paired to the console at least once** (the HA integration extracts the MAC addresses via USB — see the integration README)
- The ESP32 placed **within Bluetooth range of your console** (typically 5–10 metres, line of sight preferred)

### Supported Consoles

The wake protocol is identical across all PlayStation generations:

| Console | Controller | Tested |
|---|---|---|
| PlayStation 3 | DualShock 3 | ✅ |
| PlayStation 4 | DualShock 4 | ✅ |
| PlayStation 5 | DualSense / DualSense Edge | ✅ |

---

## Installation

Nothing to download: the firmware is an [ESPHome external component](https://esphome.io/components/external_components.html) fetched from GitHub at compile time.

### Option A: the package (recommended)

Add the package to your node config. It sets everything the Home Assistant integration expects:

```yaml
esphome:
  name: esp-wakepsx-on-bt
  friendly_name: Wake PSX on Bluetooth

packages:
  wakepsx: github://victorigualada/wakepsx-on-bt/esphome/wakepsx-on-bt.yaml@main

# plus your usual logger / api / ota / wifi
```

A complete node file with secrets is in [esp-wakepsx-on-bt.yaml](esp-wakepsx-on-bt.yaml).

`@main` follows the latest code. For reproducible builds pin a [release](https://github.com/victorigualada/wakepsx-on-bt/releases) instead; the component it pulls is pinned to the same tag:

```yaml
packages:
  wakepsx: github://victorigualada/wakepsx-on-bt/esphome/wakepsx-on-bt.yaml@v0.2.1
```

(v0.2.0 is the exception: it still pulls the component from `main` unless you also set `substitutions: wakepsx_ref: v0.2.0`.)

The package provides:

| Piece | Why |
|---|---|
| `esphome.project.name: victorigualada.esp-wakepsx-on-bt` | The integration finds the node by device model `esp-wakepsx-on-bt` |
| `esp32: board: esp32dev`, ESP-IDF framework | Bluetooth Classic is only on the original ESP32 |
| `external_components` + `wakepsx_on_bt:` | The wake firmware and its sdkconfig options |
| `PSX Wake Status` text sensor | Status shown in HA (entity id ends in `psx_wake_status`) |
| `api: actions: wake_psx` | Called by the integration as `esphome.<node>_wake_psx` |

To use another classic ESP32 board, override it in your file (`esp32: board: ...`).

### Option B: the component on its own

If you want to wire it yourself (for example to wake from an ESPHome button or automation):

```yaml
esphome:
  project:
    name: "victorigualada.esp-wakepsx-on-bt"   # keep it if you use the HA integration
    version: "0.2.1"

esp32:
  board: esp32dev
  framework:
    type: esp-idf

external_components:
  - source: github://victorigualada/wakepsx-on-bt@main
    components: [wakepsx_on_bt]

wakepsx_on_bt:
  id: wakepsx
  status:                       # optional, any text_sensor option
    name: "PSX Wake Status"

button:
  - platform: template
    name: "Wake PS5"
    on_press:
      - wakepsx_on_bt.wake:
          dsx_mac: "AA:BB:CC:DD:EE:FF"   # controller paired with the console
          psx_mac: "11:22:33:44:55:66"   # console
```

`wakepsx_on_bt.wake` accepts templates (`!lambda`) for both MACs. The component sets the Bluetooth sdkconfig options itself and refuses to build alongside `esp32_ble`, `esp32_ble_tracker` or `bluetooth_proxy`, which need the BLE host stack it disables.

### Flash and add to Home Assistant

```bash
esphome run esp-wakepsx-on-bt.yaml
```

Or use the ESPHome dashboard. Once the ESP32 is online, Home Assistant discovers it through the ESPHome integration. Add it, then configure the **Wake PSX on Bluetooth** integration and select this node as your wake gateway.

---

## How It Works (Simple Version)

1. You press the **Wake** button in Home Assistant.
2. HA calls the `wake_psx` service on the ESP32, passing two MAC addresses: the console's and the controller's.
3. The ESP32 **changes its Bluetooth address** to match your controller (spoofing).
4. The ESP32 sends a standard Bluetooth connection request to the console.
5. The console sees a known controller trying to connect and wakes up.
6. The ESP32 shuts down the Bluetooth controller and reports the result back to HA. The spoofed address is re-applied at the start of the next wake call.

The whole sequence takes 1–6 seconds depending on how quickly the console responds.

---

## Status Sensor

The firmware exposes a `PSX Wake Status` sensor in Home Assistant that shows the current state:

| Value | Meaning |
|---|---|
| `Ready` | Idle, waiting for a wake command |
| `Connecting...` | Wake sequence in progress |
| `Success` | Console responded and is waking up |
| `Page Timeout` | Console did not respond within 6 seconds (off, out of range, or already on) |
| `No Response` | No HCI event received — possible firmware issue |
| `Busy` | A wake command is already running |
| `Spoof Error` | Failed to change the Bluetooth address |
| `Init Error` / `Enable Error` | BT controller initialisation failed |
| `VHCI Error` | Virtual HCI interface error |
| `TX Unavailable` | Controller not ready to send — try again |
| `Invalid MAC` | Malformed MAC address received from HA |
| `Task Error` | FreeRTOS task could not be created (out of memory) |

The status resets to `Ready` 5 seconds after the result is displayed.

---

## Technical Details

### Firmware Architecture

The firmware disables the full Bluetooth host stack (Bluedroid and NimBLE) and uses the **Virtual HCI (VHCI)** interface directly. This frees RAM and gives direct access to the HCI transport layer without a protocol stack in the way.

```
wakepsx_on_bt.wake action
        │
        ▼
WakePsxOnBt::wake()           ← main loop: MAC parsing, busy guard, "Connecting..."
        │
        ▼
FreeRTOS task: wake_task_()   ← 4 KB stack; posts results back to loop() for publishing
        │
        ├─ esp_bt_controller_deinit()     BT controller reset to clean state
        ├─ esp_iface_mac_addr_set()       BD_ADDR spoof (must happen before init)
        ├─ esp_bt_controller_init()       Classic BT only (ESP_BT_MODE_CLASSIC_BT)
        ├─ esp_bt_controller_enable()
        ├─ esp_vhci_host_register_callback()
        ├─ esp_vhci_host_send_packet()    Raw HCI Create Connection injection
        └─ poll connection_status         Updated by notify_host_recv() callback
```

### HCI Packet

The wake packet is a raw **HCI Create Connection** command (opcode `0x0405`), identical to what the Python library sends on Linux:

```
01 05 04 0D             HCI Command, opcode 0x0405, 13 param bytes
XX XX XX XX XX XX       Target console MAC (little-endian)
18 CC                   Packet type: DM1|DH1|DM3|DH3|DM5|DH5 (0xCC18)
02                      Page Scan Repetition Mode R2
00                      Reserved
00 00                   Clock Offset (not valid, ignored)
01                      Allow Role Switch
```

Ref: Bluetooth Core Spec Vol 4 Part E §7.1.5.

### Concurrency

An atomic busy flag prevents concurrent wake attempts. A second call while a wake is in progress returns `Busy` immediately. The flag is cleared 5 seconds after the result is published (while the status is displayed), so rapid retries within that window are also blocked.

The wake task never touches ESPHome entities: it hands each status to the component, and `loop()` publishes it from the main loop, because ESPHome entities and the API are not thread-safe.

### Timing Constants

| Constant | Value | Reason |
|---|---|---|
| `BT_CONTROLLER_SETTLE_MS` | 100 ms | Hardware settling after controller deinit (empirical) |
| `VHCI_WARMUP_MS` | 200 ms | Controller internal init after enable — no "ready" event in VHCI mode |
| `VHCI_TX_POLL_MAX_CYCLES × INTERVAL` | 50 × 10 ms = 500 ms | Safety poll if warmup is not enough |
| `WAKE_TIMEOUT_CYCLES × INTERVAL` | 60 × 100 ms = 6 s | Page timeout margin (BT default = 5.12 s) |
| `WAKE_RESULT_DISPLAY_MS` | 5 s | UX display time before reset to Ready |

### sdkconfig Options

The component sets these itself; you do not need them in your YAML.

| Option | Value | Reason |
|---|---|---|
| `CONFIG_BT_ENABLED` | y | Enable BT subsystem |
| `CONFIG_BT_CONTROLLER_ONLY` | y | No host stack — VHCI only |
| `CONFIG_BT_BLUEDROID_ENABLED` | n | Disabled — not needed, saves RAM |
| `CONFIG_BT_NIMBLE_ENABLED` | n | Disabled — not needed, saves RAM |
| `CONFIG_BTDM_CTRL_HCI_MODE_VHCI` | y | Route HCI over Virtual HCI interface |
| `CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY` | y | Classic BT only — disables BLE hardware to save RAM |
