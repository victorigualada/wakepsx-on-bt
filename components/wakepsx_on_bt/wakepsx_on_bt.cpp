// Low-level Bluetooth HCI injection for PlayStation Wake-on-BT.
// Original firmware by FreeTHX (https://github.com/FreeTHX/wakepsx-on-bt).

#include "wakepsx_on_bt.h"

#include "esphome/core/log.h"

#include "esp_bt.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cctype>
#include <cstring>

namespace esphome::wakepsx_on_bt {

static const char *const TAG = "wakepsx_on_bt";

// Wake task stack, in bytes (ESP-IDF's FreeRTOS counts stack depth in bytes).
static constexpr uint32_t WAKE_TASK_STACK_BYTES = 4096;

// Priority 5: deliberately low. The BT controller task runs at 23, so the wake task yields to
// any BT driver work while still pre-empting idle.
static constexpr UBaseType_t WAKE_TASK_PRIORITY = 5;

// Settling time after esp_bt_controller_deinit() before calling esp_iface_mac_addr_set().
// ESP-IDF does not specify a minimum; empirically sufficient.
static constexpr uint32_t BT_CONTROLLER_SETTLE_MS = 100;

// VHCI exposes no "controller ready" event; this fixed delay covers the controller's internal
// initialisation after enable. Based on ESP-IDF's bt_hci_test example and empirical validation.
static constexpr uint32_t VHCI_WARMUP_MS = 200;

// Safety net if VHCI_WARMUP_MS is not enough: poll esp_vhci_host_check_send_available()
// for up to 50 × 10 ms = 500 ms.
static constexpr uint32_t VHCI_TX_POLL_INTERVAL_MS = 10;
static constexpr int VHCI_TX_POLL_MAX_CYCLES = 50;

// Wait for HCI Connection Complete. Default BR/EDR page timeout = 5.12 s (HCI spec Vol 4 §7.3.17),
// so 60 × 100 ms = 6 s leaves ~1 s of margin.
static constexpr uint32_t WAKE_POLL_INTERVAL_MS = 100;
static constexpr int WAKE_TIMEOUT_CYCLES = 60;

// How long the result stays on the sensor before it goes back to "Ready". Also blocks retries.
static constexpr uint32_t WAKE_RESULT_DISPLAY_MS = 5000;

// HCI Create Connection packet type: all BR packet sizes (DM1|DH1|DM3|DH3|DM5|DH5).
// EDR variants are implicitly allowed (their "shall not use" bits are 0).
// Ref: Bluetooth Core Spec Vol 4 Part E §7.1.5.
static constexpr uint16_t HCI_BR_PACKET_TYPE = 0xCC18;
static constexpr uint16_t HCI_OP_CREATE_CONNECTION = 0x0405;

// Written from the VHCI callback (BT controller task) and read by the wake task.
static std::atomic<int> connection_status{-1};
// Target console address, little-endian, used to filter Connection Complete events from other
// devices. Written by the wake task before the packet is sent, read by the VHCI callback after.
static uint8_t target_psx_le[6] = {0};

const char *wake_status_to_string(WakeStatus status) {
  switch (status) {
    case WakeStatus::READY:
      return "Ready";
    case WakeStatus::CONNECTING:
      return "Connecting...";
    case WakeStatus::SUCCESS:
      return "Success";
    case WakeStatus::PAGE_TIMEOUT:
      return "Page Timeout";
    case WakeStatus::NO_RESPONSE:
      return "No Response";
    case WakeStatus::BT_ERROR:
      return "BT Error";
    case WakeStatus::BUSY:
      return "Busy";
    case WakeStatus::SPOOF_ERROR:
      return "Spoof Error";
    case WakeStatus::INIT_ERROR:
      return "Init Error";
    case WakeStatus::ENABLE_ERROR:
      return "Enable Error";
    case WakeStatus::VHCI_ERROR:
      return "VHCI Error";
    case WakeStatus::TX_UNAVAILABLE:
      return "TX Unavailable";
    case WakeStatus::INVALID_MAC:
      return "Invalid MAC";
    case WakeStatus::TASK_ERROR:
      return "Task Error";
    default:
      return "Unknown";
  }
}

// HCI event from the controller. Two events matter:
//   0x03 Connection Complete — terminal result of the page attempt.
//   0x0F Command Status      — the controller rejected Create Connection outright (status != 0);
//                              without this a rejection would spin for the full 6 s timeout.
static int notify_host_recv(uint8_t *data, uint16_t len) {
  if (data == nullptr || len < 2 || data[0] != 0x04)
    return 0;

  if (data[1] == 0x03 && len >= 13) {
    // data[3] = status, data[6..11] = BD_ADDR (little-endian). Ignore other devices.
    if (memcmp(&data[6], target_psx_le, 6) != 0)
      return 0;
    connection_status = data[3];
  } else if (data[1] == 0x0F && len >= 7) {
    // data[3] = status, data[5..6] = opcode (little-endian).
    const uint16_t opcode = static_cast<uint16_t>(data[5]) | (static_cast<uint16_t>(data[6]) << 8);
    if (opcode == HCI_OP_CREATE_CONNECTION && data[3] != 0x00)
      connection_status = data[3];
  }
  return 0;
}

static void notify_host_send_available() {}

static const esp_vhci_host_callback_t VHCI_CALLBACKS = {
    .notify_host_send_available = notify_host_send_available,
    .notify_host_recv = notify_host_recv,
};

static uint8_t hex_nibble(char c) {
  if (c >= '0' && c <= '9')
    return static_cast<uint8_t>(c - '0');
  if (c >= 'a' && c <= 'f')
    return static_cast<uint8_t>(c - 'a' + 10);
  return static_cast<uint8_t>(c - 'A' + 10);
}

// Parses "AA:BB:CC:DD:EE:FF". Rejects all-zeros, broadcast and multicast addresses, which are
// never valid BR/EDR unicast targets, before any BT hardware is touched.
static bool parse_mac(const std::string &mac, uint8_t out[6]) {
  if (mac.size() != 17)
    return false;

  for (int i = 0; i < 6; i++) {
    const size_t pos = static_cast<size_t>(i) * 3;
    const unsigned char hi = static_cast<unsigned char>(mac[pos]);
    const unsigned char lo = static_cast<unsigned char>(mac[pos + 1]);
    if (!std::isxdigit(hi) || !std::isxdigit(lo))
      return false;
    if (i < 5 && mac[pos + 2] != ':')
      return false;
    out[i] = static_cast<uint8_t>((hex_nibble(static_cast<char>(hi)) << 4) | hex_nibble(static_cast<char>(lo)));
  }

  const bool all_zeros = (out[0] | out[1] | out[2] | out[3] | out[4] | out[5]) == 0x00;
  const bool broadcast = (out[0] & out[1] & out[2] & out[3] & out[4] & out[5]) == 0xFF;
  const bool multicast = (out[0] & 0x01) != 0;  // IEEE 802 group bit
  return !(all_zeros || broadcast || multicast);
}

static void cleanup_bt_controller() {
  esp_err_t err = esp_bt_controller_disable();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    ESP_LOGW(TAG, "BT controller disable failed: %s", esp_err_to_name(err));

  err = esp_bt_controller_deinit();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    ESP_LOGW(TAG, "BT controller deinit failed: %s", esp_err_to_name(err));
}

struct WakePsxOnBt::TaskParams {
  WakePsxOnBt *parent;
  uint8_t dsx[6];
  uint8_t psx[6];
};

void WakePsxOnBt::setup() { this->publish_(WakeStatus::READY); }

void WakePsxOnBt::loop() {
  const auto pending = static_cast<WakeStatus>(this->pending_status_.exchange(static_cast<uint8_t>(WakeStatus::NONE)));
  if (pending != WakeStatus::NONE)
    this->publish_(pending);
}

void WakePsxOnBt::dump_config() {
  ESP_LOGCONFIG(TAG, "Wake PSX on Bluetooth:");
  LOG_TEXT_SENSOR("  ", "Status", this->status_sensor_);
}

void WakePsxOnBt::publish_(WakeStatus status) {
  if (this->status_sensor_ != nullptr)
    this->status_sensor_->publish_state(wake_status_to_string(status));
}

void WakePsxOnBt::wake(const std::string &dsx_mac, const std::string &psx_mac) {
  if (this->busy_.exchange(true)) {
    ESP_LOGW(TAG, "Wake already in progress");
    this->publish_(WakeStatus::BUSY);
    return;
  }
  // Drop a stale "Ready" the previous task may have posted after clearing busy_,
  // so it cannot overwrite the "Connecting..." published below.
  this->pending_status_.store(static_cast<uint8_t>(WakeStatus::NONE));

  auto *params = new TaskParams{this, {}, {}};
  if (!parse_mac(dsx_mac, params->dsx) || !parse_mac(psx_mac, params->psx)) {
    ESP_LOGW(TAG, "Invalid MAC address. dsx_mac=%s psx_mac=%s", dsx_mac.c_str(), psx_mac.c_str());
    delete params;
    this->publish_(WakeStatus::INVALID_MAC);
    this->set_timeout("ready", WAKE_RESULT_DISPLAY_MS, [this]() {
      this->publish_(WakeStatus::READY);
      this->busy_ = false;
    });
    return;
  }

  ESP_LOGI(TAG, "Waking %s as controller %s", psx_mac.c_str(), dsx_mac.c_str());
  this->publish_(WakeStatus::CONNECTING);

  if (xTaskCreate(wake_task_, "wakepsx_on_bt", WAKE_TASK_STACK_BYTES, params, WAKE_TASK_PRIORITY, nullptr) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create wake task");
    delete params;
    this->publish_(WakeStatus::TASK_ERROR);
    this->busy_ = false;
  }
}

void WakePsxOnBt::wake_task_(void *arg) {
  auto *params = static_cast<TaskParams *>(arg);
  WakePsxOnBt *self = params->parent;

  // Every path ends here: publish the result, power the controller down, keep the result on
  // screen for a while, then go back to Ready and accept the next wake.
  auto finish = [params, self](WakeStatus result) {
    self->post_status_(result);
    cleanup_bt_controller();
    vTaskDelay(pdMS_TO_TICKS(WAKE_RESULT_DISPLAY_MS));
    self->post_status_(WakeStatus::READY);
    self->busy_ = false;
    delete params;
    vTaskDelete(nullptr);
  };

  // 1. Start from a clean controller.
  cleanup_bt_controller();
  vTaskDelay(pdMS_TO_TICKS(BT_CONTROLLER_SETTLE_MS));

  // 2. Spoof the controller's address. Must happen before esp_bt_controller_init().
  esp_err_t err = esp_iface_mac_addr_set(params->dsx, ESP_MAC_BT);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to spoof BT MAC address: %s", esp_err_to_name(err));
    finish(WakeStatus::SPOOF_ERROR);
    return;
  }

  // 3. Bring the controller up, Classic BT only.
  esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  bt_cfg.mode = ESP_BT_MODE_CLASSIC_BT;
  err = esp_bt_controller_init(&bt_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialise BT controller: %s", esp_err_to_name(err));
    finish(WakeStatus::INIT_ERROR);
    return;
  }
  err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to enable BT controller: %s", esp_err_to_name(err));
    finish(WakeStatus::ENABLE_ERROR);
    return;
  }

  // 4. Attach to Virtual HCI and let the controller warm up.
  err = esp_vhci_host_register_callback(&VHCI_CALLBACKS);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register VHCI callback: %s", esp_err_to_name(err));
    finish(WakeStatus::VHCI_ERROR);
    return;
  }
  vTaskDelay(pdMS_TO_TICKS(VHCI_WARMUP_MS));

  // 5. HCI Create Connection (Bluetooth Core Spec Vol 4 Part E §7.1.5):
  //   01        HCI command packet
  //   05 04     opcode 0x0405, little-endian
  //   0D        13 parameter bytes
  //   BD_ADDR   console address, little-endian
  //   18 CC     packet type (HCI_BR_PACKET_TYPE)
  //   02        page scan repetition mode R2
  //   00        reserved
  //   00 00     clock offset (not valid)
  //   01        allow role switch
  const uint8_t *psx = params->psx;
  uint8_t hci_packet[17] = {
      0x01,
      static_cast<uint8_t>(HCI_OP_CREATE_CONNECTION & 0xFF),
      static_cast<uint8_t>(HCI_OP_CREATE_CONNECTION >> 8),
      0x0D,
      psx[5],
      psx[4],
      psx[3],
      psx[2],
      psx[1],
      psx[0],
      static_cast<uint8_t>(HCI_BR_PACKET_TYPE & 0xFF),
      static_cast<uint8_t>(HCI_BR_PACKET_TYPE >> 8),
      0x02,
      0x00,
      0x00,
      0x00,
      0x01,
  };

  for (int i = 0; i < VHCI_TX_POLL_MAX_CYCLES && !esp_vhci_host_check_send_available(); i++)
    vTaskDelay(pdMS_TO_TICKS(VHCI_TX_POLL_INTERVAL_MS));
  if (!esp_vhci_host_check_send_available()) {
    ESP_LOGE(TAG, "VHCI host not ready to send");
    finish(WakeStatus::TX_UNAVAILABLE);
    return;
  }

  for (int i = 0; i < 6; i++)
    target_psx_le[i] = psx[5 - i];
  connection_status = -1;
  esp_vhci_host_send_packet(hci_packet, sizeof(hci_packet));

  // 6. Wait for the console to answer the page.
  for (int i = 0; i < WAKE_TIMEOUT_CYCLES && connection_status == -1; i++)
    vTaskDelay(pdMS_TO_TICKS(WAKE_POLL_INTERVAL_MS));

  // 7. Map the HCI status to a result.
  const int status = connection_status.load();
  WakeStatus result = WakeStatus::BT_ERROR;
  if (status == 0x00) {
    result = WakeStatus::SUCCESS;
  } else if (status == 0x04) {
    result = WakeStatus::PAGE_TIMEOUT;
  } else if (status == -1) {
    result = WakeStatus::NO_RESPONSE;
  }
  if (result == WakeStatus::BT_ERROR) {
    ESP_LOGW(TAG, "Wake finished with HCI status 0x%02X", status);
  } else {
    ESP_LOGI(TAG, "Wake finished: %s", wake_status_to_string(result));
  }
  finish(result);
}

}  // namespace esphome::wakepsx_on_bt
