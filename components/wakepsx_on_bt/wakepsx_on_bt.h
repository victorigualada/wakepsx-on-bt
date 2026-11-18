#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace esphome::wakepsx_on_bt {

// Values published on the status text sensor. The Home Assistant integration
// reads these strings, so they are part of the public contract.
enum class WakeStatus : uint8_t {
  NONE = 0,  // nothing pending (internal)
  READY,
  CONNECTING,
  SUCCESS,
  PAGE_TIMEOUT,
  NO_RESPONSE,
  BT_ERROR,
  BUSY,
  SPOOF_ERROR,
  INIT_ERROR,
  ENABLE_ERROR,
  VHCI_ERROR,
  TX_UNAVAILABLE,
  INVALID_MAC,
  TASK_ERROR,
};

const char *wake_status_to_string(WakeStatus status);

class WakePsxOnBt : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  // Publish "Ready" before the API comes up so Home Assistant never sees "unknown".
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_status_sensor(text_sensor::TextSensor *sensor) { this->status_sensor_ = sensor; }

  /// Start a wake sequence. Must be called from the main loop (actions, lambdas, API actions).
  /// Returns immediately; the result is published on the status sensor.
  void wake(const std::string &dsx_mac, const std::string &psx_mac);

 protected:
  struct TaskParams;
  static void wake_task_(void *arg);

  void publish_(WakeStatus status);
  // Called from the wake task: hand a status over to the main loop, which publishes it.
  void post_status_(WakeStatus status) { this->pending_status_.store(static_cast<uint8_t>(status)); }

  text_sensor::TextSensor *status_sensor_{nullptr};
  // Held from wake() until the result has been displayed; a second call meanwhile gets "Busy".
  // An atomic flag instead of a FreeRTOS mutex: it is set on the main loop and cleared by the
  // wake task, and a mutex must be released by the task that took it.
  std::atomic<bool> busy_{false};
  // Status posted by the wake task, published from loop(). ESPHome entities are not
  // thread-safe, so the task never calls publish_state() itself.
  std::atomic<uint8_t> pending_status_{static_cast<uint8_t>(WakeStatus::NONE)};
};

template<typename... Ts> class WakeAction : public Action<Ts...>, public Parented<WakePsxOnBt> {
 public:
  TEMPLATABLE_VALUE(std::string, dsx_mac)
  TEMPLATABLE_VALUE(std::string, psx_mac)

  void play(const Ts &...x) override { this->parent_->wake(this->dsx_mac_.value(x...), this->psx_mac_.value(x...)); }
};

}  // namespace esphome::wakepsx_on_bt
