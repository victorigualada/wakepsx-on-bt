#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include "arp_watch.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace esphome::wakepsx_on_bt {

// Console power state as reported by DDP (the PlayStation Device Discovery Protocol).
// The strings published for each value are part of the public contract.
enum class ConsoleState : uint8_t {
  UNKNOWN = 0,  // no verdict yet (internal)
  ON,           // "HTTP/1.1 200 Ok"
  REST,         // "HTTP/1.1 620 Server Standby"
  OFF,          // no reply: off, unplugged or not on the network
  NO_ADDRESS,   // no valid console IP configured: nothing is asked
};

const char *console_state_to_string(ConsoleState state);

// Asks the console over the local network whether it is on, in rest mode or off, the same way
// the PS Remote Play app finds consoles: a UDP "SRCH" to port 9302 (PS5) and 987 (PS4), which
// the console answers right away, even in rest mode. No account or registration needed.
//
// Always unicast to the console's IP. Broadcast is not used: Bluetooth forces WiFi power save on,
// and in practice the console's replies to broadcast probes never reach the ESP32, while replies
// to unicast ones do (the unicast probe's ARP request already teaches the console our address).
class ConsoleMonitor : public PollingComponent {
 public:
  void setup() override;
  void update() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  /// Console IPv4 address, constant or lambda (read at every update, so it can come from a text
  /// entity). Empty or invalid: the state reads "No IP" and nothing is sent.
  template<typename V> void set_address(V address) { this->address_ = address; }
  /// Only accept replies from this console (DDP host-id, 12 hex digits, no separators).
  void set_host_id(const std::string &host_id) { this->host_id_ = host_id; }

  void set_state_sensor(text_sensor::TextSensor *sensor) { this->state_sensor_ = sensor; }
  void set_power_sensor(binary_sensor::BinarySensor *sensor) { this->power_sensor_ = sensor; }
  void set_host_name_sensor(text_sensor::TextSensor *sensor) { this->host_name_sensor_ = sensor; }
  void set_system_version_sensor(text_sensor::TextSensor *sensor) { this->system_version_sensor_ = sensor; }

  /// Remote Play requests: the devices that look for the console (an Apple TV running Asobi, a
  /// phone with PS Remote Play...), as a list of IPs and ranges separated by commas
  /// ("192.168.1.102, 192.168.1.200-210, 192.168.1.64/28"), constant or lambda read at every
  /// update. When one of them keeps asking for the console's IP over ARP and nobody answers,
  /// the sensor turns on.
  template<typename V> void set_remote_play_address(V address) { this->remote_play_address_ = address; }
  /// Requests one device needs to turn the sensor on, each at most `window` ms after its previous one.
  void set_remote_play_min_requests(uint8_t count) { this->remote_play_min_requests_ = count; }
  /// Also how long the sensor stays on after the last request.
  void set_remote_play_window(uint32_t ms) { this->remote_play_window_ms_ = ms; }
  void set_remote_play_sensor(binary_sensor::BinarySensor *sensor) { this->remote_play_sensor_ = sensor; }
  /// IP of the device that turned the sensor on last.
  void set_remote_play_device_sensor(text_sensor::TextSensor *sensor) { this->remote_play_device_sensor_ = sensor; }

 protected:
  void handle_reply_(char *data, size_t len);
  void set_state_(ConsoleState state);
  static void publish_if_changed_(text_sensor::TextSensor *sensor, const std::string &value);

  bool resolve_address_();
  void update_remote_play_filter_();
  void handle_remote_play_(uint32_t now);
  void handle_remote_play_request_(uint32_t sender, uint32_t now);
  void reset_remote_play_();

  TemplatableValue<std::string> address_;
  std::string current_address_;  // last address read, to notice changes
  bool address_valid_{false};
  std::string host_id_;
  uint32_t dest_addr_{0};  // network byte order
  int fd_{-1};

  bool probe_pending_{false};  // a probe went out and nothing has answered it yet
  uint8_t misses_{0};          // consecutive unanswered probes
  ConsoleState state_{ConsoleState::UNKNOWN};

  text_sensor::TextSensor *state_sensor_{nullptr};
  binary_sensor::BinarySensor *power_sensor_{nullptr};
  text_sensor::TextSensor *host_name_sensor_{nullptr};
  text_sensor::TextSensor *system_version_sensor_{nullptr};

  // A device asking for the console, with its current burst of requests.
  struct RemotePlayDevice {
    uint32_t ip;       // host byte order, 0 = free slot
    uint32_t last_ms;  // when its last request was seen
    uint8_t burst;     // requests in a row, each within the window of the previous one
    bool counted;      // reached min_requests: keeps the sensor on
  };
  static constexpr size_t MAX_REMOTE_PLAY_DEVICES = 8;

  TemplatableValue<std::string> remote_play_address_;
  std::string remote_play_current_;  // last list read, to notice changes
  std::vector<arp_watch::Range> remote_play_ranges_;
  uint32_t remote_play_target_{0};   // console IP handed to arp_watch, host byte order
  uint8_t remote_play_min_requests_{2};
  uint32_t remote_play_window_ms_{15000};
  RemotePlayDevice remote_play_devices_[MAX_REMOTE_PLAY_DEVICES]{};
  bool remote_play_active_{false};
  bool remote_play_hooked_{false};
  binary_sensor::BinarySensor *remote_play_sensor_{nullptr};
  text_sensor::TextSensor *remote_play_device_sensor_{nullptr};
};

}  // namespace esphome::wakepsx_on_bt
