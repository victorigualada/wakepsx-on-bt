# esp-wakepsx-on-bt

This directory contains the ESPHome firmware that wakes a PlayStation from an ESP32. The ESP32 impersonates your PlayStation controller over Bluetooth Classic and the console wakes up. It works on its own, with a **Wake console** button on the device; the **Wake PSX on Bluetooth** Home Assistant integration is optional.

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

- **ESPHome** (add-on or standalone CLI) to compile and flash the firmware
- Your **PlayStation controller paired to the console at least once**, and a USB data cable to read its MACs once (see [Setting the MACs](#setting-the-macs))
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

Add the package to your node config. It brings the button, the MAC fields and the status sensor, plus everything the optional Home Assistant integration expects:

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
  wakepsx: github://victorigualada/wakepsx-on-bt/esphome/wakepsx-on-bt.yaml@v0.6.0
```

(v0.2.0 is the exception: it still pulls the component from `main` unless you also set `substitutions: wakepsx_ref: v0.2.0`.)

The package provides:

| Piece | Why |
|---|---|
| `esphome.project.name: victorigualada.esp-wakepsx-on-bt` | The integration finds the node by device model `esp-wakepsx-on-bt` |
| `esp32: board: esp32dev`, ESP-IDF framework | Bluetooth Classic is only on the original ESP32 |
| `external_components` + `wakepsx_on_bt:` | The wake firmware and its sdkconfig options |
| `PSX Wake Status` text sensor | Status shown in HA (entity id ends in `psx_wake_status`) |
| **Wake console** button | Wakes the console with the two MACs below |
| **Console IP** text field | Where the state sensors ask the console, saved on the ESP32 |
| **Console state** / **Console power** (+ **Console name**, **Console firmware**) | Whether the console is on, in rest mode or off, asked over the local network (see [Console state](#console-state)) |
| **Remote Play IPs** text field, **Remote Play request** / **Remote Play device**, **Auto wake on Remote Play** | Wake the console when you open Remote Play on another device (see [Remote Play request](#remote-play-request)) |
| **Controller MAC** / **Console MAC** text fields | The MACs the button uses, editable from HA and saved on the ESP32 |
| `api: actions: wake_psx` | Called by the integration as `esphome.<node>_wake_psx`; also stores the MACs it receives in the two fields |

### Setting the MACs

The ESP32 needs the MAC of a controller paired with the console and the MAC of the console. Both are stored in the controller:

1. Connect the controller to a computer with a USB data cable.
2. Open the [MAC reader](https://victorigualada.github.io/wakepsx-on-bt/) in Chrome or Edge and press **Read controller**.
3. Paste the two values into **Controller MAC** and **Console MAC** on the ESP32's device page in Home Assistant (Configuration section). They are saved on the ESP32 and survive reboots and updates.

Or set them in the node's YAML; they are used on the first boot only, after that the values saved on the ESP32 win:

```yaml
substitutions:
  controller_mac: "AA:BB:CC:DD:EE:FF"
  console_mac: "11:22:33:44:55:66"
```

If you use the integration, you don't need any of this: each wake it sends through the ESP32 fills both fields. With several consoles on one ESP32 the fields hold the last console woken, so the button wakes that one; add a [template button](#option-b-the-component-on-its-own) per extra console.

To use another classic ESP32 board, override it in your file (`esp32: board: ...`).

### Option B: the component on its own

If you want to wire it yourself (for example to wake from an ESPHome button or automation):

```yaml
esphome:
  project:
    name: "victorigualada.esp-wakepsx-on-bt"   # keep it if you use the HA integration
    version: "0.6.0"

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

Or use the ESPHome dashboard. Once the ESP32 is online, Home Assistant discovers it through the ESPHome integration. Add it and [set the MACs](#setting-the-macs); the **Wake console** button is ready. If you also use the **Wake PSX on Bluetooth** integration, configure it and select this node as your wake gateway.

---

## Console State

The package also asks the console every 10 seconds whether it is on, over the local network, the same way the PS Remote Play app finds consoles (DDP: a UDP message to port 9302 on a PS5, 987 on a PS4). The console answers right away, even in rest mode; no PSN account or registration is involved, and the Bluetooth side is never touched, so checking never wakes the console.

Set the console's IP in **Console IP** on the ESP32's device page in Home Assistant (Configuration section), and give the console a fixed IP on your router (DHCP reservation). It is saved on the ESP32. Or set it in the node's YAML, used on the first boot only:

```yaml
substitutions:
  console_ip: "192.168.1.105"
```

| Entity | Values |
|---|---|
| **Console state** | `On`, `Rest` (rest mode), `Off` (no reply for two checks in a row: off, unplugged or not on the network) or `No IP` (Console IP empty or invalid) |
| **Console power** | `on` only when the state is `On` |
| **Console name** | The console's name, e.g. `PS5-338` (diagnostic) |
| **Console firmware** | System software version, e.g. `14.23` (diagnostic) |

The ESP32 always asks the console at its IP, never by broadcast: with Bluetooth on, WiFi stays in power-save mode, and the console's replies to a broadcast never reach the ESP32. The console does not report the game it is running over DDP; for that use Home Assistant's [PlayStation Network](https://www.home-assistant.io/integrations/playstation_network/) integration.

With the standalone component, add a `console:` block. `address` takes an IP or a lambda (read at every check):

```yaml
wakepsx_on_bt:
  console:
    address: 192.168.1.105   # or: !lambda return id(my_text).state;
    update_interval: 10s
    state:
      name: "Console state"
    power:
      name: "Console power"
    host_name:
      name: "Console name"
    system_version:
      name: "Console firmware"
```

---

## Remote Play Request

When you open PS Remote Play, or a client such as Asobi on an Apple TV, while the console sleeps, the device it runs on keeps asking the network "who has the console's IP?" (ARP) every few seconds, and nobody answers. Those requests are broadcast, so they reach the ESP32 over WiFi too. The ESP32 watches for them and wakes the console itself, with no Home Assistant automation involved.

1. Give the devices you play from fixed IPs on your router (DHCP reservations).
2. Set them in **Remote Play IPs** on the ESP32's device page (Configuration section). **Console IP** must be set too.
3. Open Remote Play with the console asleep. **Remote Play request** turns on after two requests from one device and, with **Auto wake on Remote Play** on (the default), the ESP32 presses **Wake console**.

**Remote Play IPs** takes up to 8 entries separated by commas, each one of:

| Entry | Example | Matches |
|---|---|---|
| An IP | `192.168.1.102` | That device |
| A range | `192.168.1.200-192.168.1.210` or `192.168.1.200-210` | Every IP from the first to the last, both included |
| A subnet | `192.168.1.192/28` | `192.168.1.192` to `192.168.1.207` (prefix 8 to 32) |

For example `192.168.1.102, 192.168.1.200-210`. Invalid entries are skipped and logged as a warning; the others still work.

| Entity | What it does |
|---|---|
| **Remote Play IPs** | The devices to watch. Empty: nothing is watched. Saved on the ESP32 (or `substitutions: remote_play_ip:` for the first boot) |
| **Remote Play request** | `on` while one of those devices keeps asking for the console without an answer; `off` 15 s after the last one stops |
| **Remote Play device** | IP of the device that turned the request on last, published just before it turns on, so automations can tell your devices apart |
| **Auto wake on Remote Play** | On: wake the console when the request starts, if **Console power** is off. Off: only report it, so a Home Assistant automation can decide (trigger on **Remote Play request** turning on) |

Each device counts on its own: one request from the Apple TV and one from a phone are not two in a row. While there is something to watch, the ESP32 keeps WiFi power save off (ESPHome turns it on by default on the ESP32): with it on, the radio sleeps between beacons and misses most of the requests. It goes back on for the few seconds of each Bluetooth wake, as ESP-IDF requires next to WiFi. Only requests for **Console IP** count, so the ESP32's own state checks are ignored.

Do not list devices that talk to the console for other reasons, above all your router: when traffic from the internet arrives for a sleeping console, the router asks for it a few times in a row too, and that would wake it. A range or subnet that covers the router or Home Assistant has the same problem. Once the console is awake it answers the device and the requests stop. If your router isolates WiFi clients or turns broadcasts into unicast, the requests never reach the ESP32; the log shows `… is looking for the console` at debug level when they do.

With the standalone component, add `remote_play:` inside `console:`:

```yaml
wakepsx_on_bt:
  console:
    address: 192.168.1.105
    remote_play:
      address: 192.168.1.102, 192.168.1.200-210   # or: !lambda return id(my_text).state;
      min_requests: 2          # requests in a row from one device before it turns on
      window: 15s              # longest gap between them; also how long it stays on
      request:
        name: "Remote Play request"
        on_press:
          - button.press: my_wake_button
      device:                  # optional
        name: "Remote Play device"
```

---

## How It Works (Simple Version)

1. You press **Wake console** on the ESP32's device (or the integration's **Wake** button, which calls the ESP32's `wake_psx` action).
2. The ESP32 takes two MAC addresses: the console's and the controller's.
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
| `Invalid MAC` | Missing or malformed MAC (set **Controller MAC** / **Console MAC**) |
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
