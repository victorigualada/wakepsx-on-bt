// Console state over DDP (PlayStation Device Discovery Protocol).

#include "console_monitor.h"

#include "esphome/core/log.h"
#include "esphome/components/network/util.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include <cctype>
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
  if (!this->resolve_address_()) {
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
}

}  // namespace esphome::wakepsx_on_bt
