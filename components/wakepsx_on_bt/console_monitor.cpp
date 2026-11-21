// Console state over DDP (PlayStation Device Discovery Protocol).

#include "console_monitor.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/network/util.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace esphome::wakepsx_on_bt {

static const char *const TAG = "wakepsx_on_bt.console";

static constexpr uint16_t PS5_DDP_PORT = 9302;
static constexpr uint16_t PS4_DDP_PORT = 987;
static const char PS5_SEARCH[] = "SRCH * HTTP/1.1\ndevice-discovery-protocol-version:00030010\n";
static const char PS4_SEARCH[] = "SRCH * HTTP/1.1\ndevice-discovery-protocol-version:00020020\n";

// Unanswered probes in a row before the console counts as off. One lost UDP packet on WiFi
// should not flip the state.
static constexpr uint8_t MISSES_BEFORE_OFF = 2;

// Parses a dotted IPv4 address into network byte order. 0 for empty, invalid, broadcast or any.
static uint32_t parse_ipv4(const std::string &text) {
  uint32_t addr = 0;
  if (text.empty() || inet_pton(AF_INET, text.c_str(), &addr) != 1)
    return 0;
  if (addr == htonl(INADDR_BROADCAST) || addr == htonl(INADDR_ANY))
    return 0;
  return addr;
}

static std::string ip_to_string(uint32_t host_order) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%u.%u.%u.%u", static_cast<unsigned>(host_order >> 24),
           static_cast<unsigned>((host_order >> 16) & 0xFF), static_cast<unsigned>((host_order >> 8) & 0xFF),
           static_cast<unsigned>(host_order & 0xFF));
  return buf;
}

static bool parse_number(const std::string &text, unsigned max, unsigned *out) {
  if (text.empty() || text.size() > 3)
    return false;
  unsigned value = 0;
  for (char c : text) {
    if (c < '0' || c > '9')
      return false;
    value = value * 10 + (c - '0');
  }
  if (value > max)
    return false;
  *out = value;
  return true;
}

// One entry of the Remote Play list, spaces already removed:
//   192.168.1.102                  one address
//   192.168.1.200-192.168.1.210    a range, both ends included
//   192.168.1.200-210              the same, the second end as its last number only
//   192.168.1.192/28               a subnet (prefix 8 to 32)
static bool parse_range(const std::string &item, arp_watch::Range *range) {
  const size_t slash = item.find('/');
  const size_t dash = item.find('-');
  if (slash != std::string::npos) {
    unsigned prefix;
    const uint32_t addr = ntohl(parse_ipv4(item.substr(0, slash)));
    if (addr == 0 || !parse_number(item.substr(slash + 1), 32, &prefix) || prefix < 8)
      return false;
    const uint32_t mask = prefix == 32 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> prefix);
    range->first = addr & mask;
    range->last = range->first | ~mask;
    return true;
  }
  if (dash != std::string::npos) {
    const uint32_t first = ntohl(parse_ipv4(item.substr(0, dash)));
    const std::string end = item.substr(dash + 1);
    uint32_t last;
    unsigned octet;
    if (end.find('.') != std::string::npos) {
      last = ntohl(parse_ipv4(end));
    } else if (parse_number(end, 255, &octet)) {
      last = (first & 0xFFFFFF00u) | octet;
    } else {
      return false;
    }
    if (first == 0 || last == 0 || last < first)
      return false;
    range->first = first;
    range->last = last;
    return true;
  }
  const uint32_t addr = ntohl(parse_ipv4(item));
  if (addr == 0)
    return false;
  range->first = range->last = addr;
  return true;
}

// Parses "192.168.1.102, 192.168.1.200-210" (commas or semicolons between entries). Invalid
// entries are logged and skipped; at most arp_watch::MAX_RANGES are kept.
static std::vector<arp_watch::Range> parse_range_list(const std::string &text) {
  std::vector<arp_watch::Range> ranges;
  std::string item;
  for (size_t i = 0; i <= text.size(); i++) {
    const char c = i < text.size() ? text[i] : ',';
    if (c != ',' && c != ';') {
      if (c != ' ' && c != '\t')
        item += c;
      continue;
    }
    if (item.empty())
      continue;
    arp_watch::Range range;
    if (!parse_range(item, &range)) {
      ESP_LOGW(TAG, "Ignoring invalid Remote Play entry '%s'", item.c_str());
    } else if (ranges.size() >= arp_watch::MAX_RANGES) {
      ESP_LOGW(TAG, "Ignoring Remote Play entry '%s': at most %u entries", item.c_str(),
               static_cast<unsigned>(arp_watch::MAX_RANGES));
    } else {
      ranges.push_back(range);
    }
    item.clear();
  }
  return ranges;
}

// Replies are ~200 bytes; anything longer is not a DDP reply we care about.
static constexpr size_t REPLY_BUFFER_SIZE = 512;

const char *console_state_to_string(ConsoleState state) {
  switch (state) {
    case ConsoleState::ON:
      return "On";
    case ConsoleState::REST:
      return "Rest";
    case ConsoleState::OFF:
      return "Off";
    case ConsoleState::NO_ADDRESS:
      return "No IP";
    default:
      return "Unknown";
  }
}

void ConsoleMonitor::setup() {
  if (this->remote_play_sensor_ != nullptr)
    this->remote_play_sensor_->publish_initial_state(false);

  this->fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (this->fd_ < 0) {
    ESP_LOGE(TAG, "Could not create the UDP socket (errno %d)", errno);
    this->mark_failed();
    return;
  }
  fcntl(this->fd_, F_SETFL, fcntl(this->fd_, F_GETFL, 0) | O_NONBLOCK);

  // Any local port: the console replies to the port the probe came from.
  struct sockaddr_in local {};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  local.sin_port = 0;
  if (bind(this->fd_, reinterpret_cast<struct sockaddr *>(&local), sizeof(local)) != 0) {
    ESP_LOGE(TAG, "Could not bind the UDP socket (errno %d)", errno);
    close(this->fd_);
    this->fd_ = -1;
    this->mark_failed();
  }
}

// Reads the address (it may come from a text entity) and re-parses it when it changed.
bool ConsoleMonitor::resolve_address_() {
  std::string address = this->address_.value();
  if (address == this->current_address_ && !address.empty())
    return this->address_valid_;

  const bool changed = address != this->current_address_;
  this->current_address_ = std::move(address);
  this->address_valid_ = !this->current_address_.empty() &&
                         inet_pton(AF_INET, this->current_address_.c_str(), &this->dest_addr_) == 1 &&
                         this->dest_addr_ != htonl(INADDR_BROADCAST) && this->dest_addr_ != htonl(INADDR_ANY);
  if (changed) {
    this->probe_pending_ = false;
    this->misses_ = 0;
    if (this->address_valid_) {
      ESP_LOGI(TAG, "Asking the console at %s", this->current_address_.c_str());
    } else if (!this->current_address_.empty()) {
      ESP_LOGW(TAG, "Invalid console IP '%s'", this->current_address_.c_str());
    } else {
      ESP_LOGW(TAG, "No console IP set");
    }
  }
  return this->address_valid_;
}

void ConsoleMonitor::update() {
  if (this->fd_ < 0)
    return;
  const bool have_address = this->resolve_address_();
  this->update_remote_play_filter_();
  if (!have_address) {
    this->set_state_(ConsoleState::NO_ADDRESS);
    return;
  }
  if (!network::is_connected())
    return;
  if (this->state_ == ConsoleState::NO_ADDRESS)
    this->state_ = ConsoleState::UNKNOWN;  // new address: no verdict until it answers or misses

  if (this->probe_pending_ && this->misses_ < MISSES_BEFORE_OFF && ++this->misses_ >= MISSES_BEFORE_OFF)
    this->set_state_(ConsoleState::OFF);

  struct sockaddr_in dest {};
  dest.sin_family = AF_INET;
  dest.sin_addr.s_addr = this->dest_addr_;

  dest.sin_port = htons(PS5_DDP_PORT);
  sendto(this->fd_, PS5_SEARCH, sizeof(PS5_SEARCH) - 1, 0, reinterpret_cast<struct sockaddr *>(&dest), sizeof(dest));
  dest.sin_port = htons(PS4_DDP_PORT);
  sendto(this->fd_, PS4_SEARCH, sizeof(PS4_SEARCH) - 1, 0, reinterpret_cast<struct sockaddr *>(&dest), sizeof(dest));
  this->probe_pending_ = true;
}

void ConsoleMonitor::loop() {
  if (this->remote_play_sensor_ != nullptr)
    this->handle_remote_play_(millis());
  if (this->fd_ < 0)
    return;
  char buffer[REPLY_BUFFER_SIZE + 1];
  for (;;) {
    const ssize_t len = recvfrom(this->fd_, buffer, REPLY_BUFFER_SIZE, 0, nullptr, nullptr);
    if (len <= 0)
      return;  // EWOULDBLOCK: nothing (more) to read
    buffer[len] = '\0';
    this->handle_reply_(buffer, static_cast<size_t>(len));
  }
}

// Reads the Remote Play list (it may come from a text entity), hooks the WiFi interface once
// the network is up and tells arp_watch which requests to watch.
void ConsoleMonitor::update_remote_play_filter_() {
  if (this->remote_play_sensor_ == nullptr)
    return;

  std::string list = this->remote_play_address_.has_value() ? this->remote_play_address_.value() : "";
  bool changed = false;
  if (list != this->remote_play_current_) {
    this->remote_play_current_ = std::move(list);
    this->remote_play_ranges_ = parse_range_list(this->remote_play_current_);
    changed = true;
    if (this->remote_play_ranges_.empty()) {
      ESP_LOGD(TAG, "No Remote Play devices set");
    }
    for (const auto &range : this->remote_play_ranges_) {
      if (range.first == range.last) {
        ESP_LOGI(TAG, "Watching for Remote Play requests from %s", ip_to_string(range.first).c_str());
      } else {
        ESP_LOGI(TAG, "Watching for Remote Play requests from %s to %s", ip_to_string(range.first).c_str(),
                 ip_to_string(range.last).c_str());
      }
    }
  }

  if (network::is_connected()) {
    const bool hooked = arp_watch::ensure_installed();
    if (hooked != this->remote_play_hooked_ && !hooked)
      ESP_LOGW(TAG, "Could not watch ARP on the WiFi interface");
    this->remote_play_hooked_ = hooked;
  }

  const uint32_t target = this->address_valid_ ? ntohl(this->dest_addr_) : 0;
  if (!changed && target == this->remote_play_target_)
    return;
  this->remote_play_target_ = target;
  arp_watch::set_filter(this->remote_play_ranges_.data(), this->remote_play_ranges_.size(), target);
  this->reset_remote_play_();  // new devices or console: start over
}

void ConsoleMonitor::reset_remote_play_() {
  for (auto &device : this->remote_play_devices_)
    device = {};
  if (this->remote_play_active_) {
    this->remote_play_active_ = false;
    this->remote_play_sensor_->publish_state(false);
  }
}

// Each device counts on its own: the sensor turns on when one of them sends `min_requests`
// requests, each within `window` of its previous one, and off once `window` passes without a
// request from any device that got there.
void ConsoleMonitor::handle_remote_play_(uint32_t now) {
  bool any_counted = false;
  for (auto &device : this->remote_play_devices_) {
    if (device.ip == 0)
      continue;
    if (now - device.last_ms > this->remote_play_window_ms_) {
      if (device.counted)
        ESP_LOGD(TAG, "%s stopped looking for the console", ip_to_string(device.ip).c_str());
      device = {};
      continue;
    }
    any_counted |= device.counted;
  }
  if (this->remote_play_active_ && !any_counted) {
    this->remote_play_active_ = false;
    this->remote_play_sensor_->publish_state(false);
  }

  uint32_t sender;
  while (arp_watch::pop(&sender))
    this->handle_remote_play_request_(sender, now);
}

void ConsoleMonitor::handle_remote_play_request_(uint32_t sender, uint32_t now) {
  // Its slot, else a free one, else the one quiet for longest.
  RemotePlayDevice *slot = nullptr;
  RemotePlayDevice *free_slot = nullptr;
  RemotePlayDevice *oldest = &this->remote_play_devices_[0];
  for (auto &device : this->remote_play_devices_) {
    if (device.ip == sender) {
      slot = &device;
      break;
    }
    if (device.ip == 0 && free_slot == nullptr)
      free_slot = &device;
    if (now - device.last_ms > now - oldest->last_ms)
      oldest = &device;
  }
  if (slot == nullptr) {
    slot = free_slot != nullptr ? free_slot : oldest;
    *slot = {sender, now, 0, false};
  }

  if (slot->burst < 255)
    slot->burst++;
  slot->last_ms = now;
  const std::string ip = ip_to_string(sender);
  ESP_LOGD(TAG, "%s is looking for the console (%u in a row)", ip.c_str(), slot->burst);
  if (slot->counted || slot->burst < this->remote_play_min_requests_)
    return;

  slot->counted = true;
  ESP_LOGI(TAG, "Remote Play request from %s", ip.c_str());
  // The device first, so automations on the request can read it.
  if (this->remote_play_device_sensor_ != nullptr)
    this->remote_play_device_sensor_->publish_state(ip);
  if (!this->remote_play_active_) {
    this->remote_play_active_ = true;
    this->remote_play_sensor_->publish_state(true);
  }
}

// A reply looks like:
//   HTTP/1.1 620 Server Standby
//   host-id:D4F7D543E740
//   host-type:PS5
//   host-name:PS5-338
//   host-request-port:997
//   device-discovery-protocol-version:00030010
//   system-version:14230000
void ConsoleMonitor::handle_reply_(char *data, size_t len) {
  ConsoleState state;
  if (strncmp(data, "HTTP/1.1 200", 12) == 0) {
    state = ConsoleState::ON;
  } else if (strncmp(data, "HTTP/1.1 620", 12) == 0) {
    state = ConsoleState::REST;
  } else {
    return;  // not a DDP reply
  }

  std::string host_id, host_name, system_version;
  char *save = nullptr;
  for (char *line = strtok_r(data, "\n", &save); line != nullptr; line = strtok_r(nullptr, "\n", &save)) {
    const size_t line_len = strlen(line);
    if (line_len > 0 && line[line_len - 1] == '\r')
      line[line_len - 1] = '\0';
    char *colon = strchr(line, ':');
    if (colon == nullptr)
      continue;
    *colon = '\0';
    const char *value = colon + 1;
    if (strcmp(line, "host-id") == 0) {
      host_id = value;
    } else if (strcmp(line, "host-name") == 0) {
      host_name = value;
    } else if (strcmp(line, "system-version") == 0) {
      system_version = value;
    }
  }

  if (!this->host_id_.empty() && strcasecmp(host_id.c_str(), this->host_id_.c_str()) != 0) {
    ESP_LOGV(TAG, "Ignoring console %s", host_id.c_str());
    return;
  }

  this->probe_pending_ = false;
  this->misses_ = 0;
  this->set_state_(state);
  publish_if_changed_(this->host_name_sensor_, host_name);

  // "14230000" -> "14.23"
  if (system_version.size() == 8 && std::isdigit(static_cast<unsigned char>(system_version[0])) &&
      std::isdigit(static_cast<unsigned char>(system_version[1])) &&
      std::isdigit(static_cast<unsigned char>(system_version[2])) &&
      std::isdigit(static_cast<unsigned char>(system_version[3]))) {
    char formatted[8];
    snprintf(formatted, sizeof(formatted), "%d.%.2s", (system_version[0] - '0') * 10 + (system_version[1] - '0'),
             system_version.c_str() + 2);
    system_version = formatted;
  }
  publish_if_changed_(this->system_version_sensor_, system_version);
}

void ConsoleMonitor::set_state_(ConsoleState state) {
  if (state == this->state_)
    return;
  ESP_LOGD(TAG, "Console is %s", console_state_to_string(state));
  this->state_ = state;
  if (this->state_sensor_ != nullptr)
    this->state_sensor_->publish_state(console_state_to_string(state));
  if (this->power_sensor_ != nullptr && state != ConsoleState::UNKNOWN)
    this->power_sensor_->publish_state(state == ConsoleState::ON);
}

void ConsoleMonitor::publish_if_changed_(text_sensor::TextSensor *sensor, const std::string &value) {
  if (sensor == nullptr || value.empty())
    return;
  if (!sensor->has_state() || sensor->get_state() != value)
    sensor->publish_state(value);
}

void ConsoleMonitor::dump_config() {
  ESP_LOGCONFIG(TAG, "Console monitor (DDP):");
  ESP_LOGCONFIG(TAG, "  Address: %s", this->current_address_.empty() ? "(not set yet)" : this->current_address_.c_str());
  if (!this->host_id_.empty())
    ESP_LOGCONFIG(TAG, "  Host ID: %s", this->host_id_.c_str());
  LOG_UPDATE_INTERVAL(this);
  LOG_TEXT_SENSOR("  ", "State", this->state_sensor_);
  LOG_BINARY_SENSOR("  ", "Power", this->power_sensor_);
  LOG_TEXT_SENSOR("  ", "Host name", this->host_name_sensor_);
  LOG_TEXT_SENSOR("  ", "System version", this->system_version_sensor_);
  if (this->remote_play_sensor_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Remote Play devices: %s (%u entries)",
                  this->remote_play_current_.empty() ? "(not set yet)" : this->remote_play_current_.c_str(),
                  static_cast<unsigned>(this->remote_play_ranges_.size()));
    ESP_LOGCONFIG(TAG, "  Remote Play: %u requests, at most %" PRIu32 " ms apart", this->remote_play_min_requests_,
                  this->remote_play_window_ms_);
    LOG_BINARY_SENSOR("  ", "Remote Play request", this->remote_play_sensor_);
    LOG_TEXT_SENSOR("  ", "Remote Play device", this->remote_play_device_sensor_);
  }
}

}  // namespace esphome::wakepsx_on_bt
