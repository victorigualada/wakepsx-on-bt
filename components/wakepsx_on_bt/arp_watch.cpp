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

// Written by the main loop, read by the hook.
static std::atomic<uint32_t> want_sender{0};
static std::atomic<uint32_t> want_target{0};
// Written by the hook, drained by the main loop.
static std::atomic<uint32_t> matches{0};

// Runs in the WiFi driver's task for every frame the STA interface receives, before lwIP sees
// it. Only reads the frame, keeps it short and always hands it on.
static err_t hook_input(struct pbuf *p, struct netif *inp) {
  if (p != nullptr && p->len >= SIZEOF_ETH_HDR + SIZEOF_ETHARP_HDR) {
    const auto *frame = static_cast<const uint8_t *>(p->payload);
    const auto *eth = reinterpret_cast<const struct eth_hdr *>(frame);
    if (eth->type == PP_HTONS(ETHTYPE_ARP)) {
      const auto *arp = reinterpret_cast<const struct etharp_hdr *>(frame + SIZEOF_ETH_HDR);
      const uint32_t sender = want_sender.load(std::memory_order_relaxed);
      if (sender != 0 && arp->opcode == PP_HTONS(ARP_REQUEST)) {
        uint32_t sip, dip;
        memcpy(&sip, &arp->sipaddr, sizeof(sip));  // ip4_addr2_t: unaligned in the frame
        memcpy(&dip, &arp->dipaddr, sizeof(dip));
        if (sip == sender && dip == want_target.load(std::memory_order_relaxed))
          matches.fetch_add(1, std::memory_order_relaxed);
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

void set_filter(uint32_t sender, uint32_t target) {
  want_target.store(target, std::memory_order_relaxed);
  want_sender.store(target != 0 ? sender : 0, std::memory_order_relaxed);
  matches.store(0, std::memory_order_relaxed);
}

uint32_t take_count() { return matches.exchange(0, std::memory_order_relaxed); }

}  // namespace esphome::wakepsx_on_bt::arp_watch
