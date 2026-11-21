// ARP requests for the console, seen on the WiFi STA interface.

#include "arp_watch.h"

#include "esphome/core/log.h"

#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "lwip/def.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/prot/etharp.h"
#include "lwip/prot/ethernet.h"

#include <atomic>
#include <cstring>

namespace esphome::wakepsx_on_bt::arp_watch {

static const char *const TAG = "wakepsx_on_bt.arp";

// Set and read from the main loop only.
static struct netif *hooked_netif = nullptr;
// lwIP's own input function. Stored (release) before the hook goes in, read by the hook.
static std::atomic<netif_input_fn> original_input{nullptr};

// The filter, double-buffered: the main loop fills the buffer the hook is not using, then
// publishes it by flipping `active`. The hook takes a few microseconds per frame and the filter
// changes at most every update interval, so the buffer being rewritten is never still in use.
struct Filter {
  uint32_t target;  // host byte order, 0 = nothing watched
  size_t count;
  Range ranges[MAX_RANGES];
};
static Filter filters[2] = {};
static std::atomic<uint8_t> active{0};

// Senders of matching requests, from the hook (the WiFi task, the only producer) to the main
// loop (the only consumer). When it is full, new requests are dropped: the clients repeat them.
static constexpr uint32_t QUEUE_SIZE = 16;  // power of two
static uint32_t queue[QUEUE_SIZE];
static std::atomic<uint32_t> queue_head{0};  // next slot the hook writes
static std::atomic<uint32_t> queue_tail{0};  // next slot the main loop reads

// Runs in the WiFi driver's task for every frame the STA interface receives, before lwIP sees
// it. Only reads the frame, keeps it short and always hands it on.
static err_t hook_input(struct pbuf *p, struct netif *inp) {
  if (p != nullptr && p->len >= SIZEOF_ETH_HDR + SIZEOF_ETHARP_HDR) {
    const auto *frame = static_cast<const uint8_t *>(p->payload);
    const auto *eth = reinterpret_cast<const struct eth_hdr *>(frame);
    if (eth->type == PP_HTONS(ETHTYPE_ARP)) {
      const auto *arp = reinterpret_cast<const struct etharp_hdr *>(frame + SIZEOF_ETH_HDR);
      const Filter &filter = filters[active.load(std::memory_order_acquire)];
      if (filter.target != 0 && arp->opcode == PP_HTONS(ARP_REQUEST)) {
        uint32_t sip, dip;
        memcpy(&sip, &arp->sipaddr, sizeof(sip));  // ip4_addr2_t: unaligned in the frame
        memcpy(&dip, &arp->dipaddr, sizeof(dip));
        sip = lwip_ntohl(sip);
        if (lwip_ntohl(dip) == filter.target) {
          for (size_t i = 0; i < filter.count; i++) {
            if (sip >= filter.ranges[i].first && sip <= filter.ranges[i].last) {
              const uint32_t head = queue_head.load(std::memory_order_relaxed);
              if (head - queue_tail.load(std::memory_order_acquire) < QUEUE_SIZE) {
                queue[head % QUEUE_SIZE] = sip;
                queue_head.store(head + 1, std::memory_order_release);
              }
              break;
            }
          }
        }
      }
    }
  }
  return original_input.load(std::memory_order_acquire)(p, inp);
}

bool ensure_installed() {
  esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (sta == nullptr)
    return false;
  auto *netif = static_cast<struct netif *>(esp_netif_get_netif_impl(sta));
  if (netif == nullptr || netif->input == nullptr)
    return false;
  if (netif == hooked_netif && netif->input == hook_input)
    return true;

  // First install, or the interface was recreated and got lwIP's input back.
  if (netif->input != hook_input) {
    original_input.store(netif->input, std::memory_order_release);
    // A single aligned pointer store: the WiFi task sees either the old or the new function,
    // both valid, so no lock is needed.
    netif->input = hook_input;
  }
  hooked_netif = netif;
  ESP_LOGD(TAG, "Watching ARP on the WiFi interface");
  return true;
}

void set_filter(const Range *ranges, size_t count, uint32_t target) {
  const uint8_t next = active.load(std::memory_order_relaxed) ^ 1;
  Filter &filter = filters[next];
  filter.count = count < MAX_RANGES ? count : MAX_RANGES;
  for (size_t i = 0; i < filter.count; i++)
    filter.ranges[i] = ranges[i];
  filter.target = filter.count > 0 ? target : 0;
  active.store(next, std::memory_order_release);

  // Drop what was queued under the old filter.
  queue_tail.store(queue_head.load(std::memory_order_acquire), std::memory_order_release);
}

bool pop(uint32_t *sender) {
  const uint32_t tail = queue_tail.load(std::memory_order_relaxed);
  if (tail == queue_head.load(std::memory_order_acquire))
    return false;
  *sender = queue[tail % QUEUE_SIZE];
  queue_tail.store(tail + 1, std::memory_order_release);
  return true;
}

}  // namespace esphome::wakepsx_on_bt::arp_watch
