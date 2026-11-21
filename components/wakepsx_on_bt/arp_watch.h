#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome::wakepsx_on_bt::arp_watch {

// Watches the ARP requests some devices send for the console's IP, as seen on the ESP32's WiFi.
//
// When the PS Remote Play app (or a client such as Asobi) looks for a console that is in rest
// mode or off, the device it runs on keeps asking "who has <console IP>?" over ARP, every few
// seconds, and nobody answers. Those requests are broadcast, so they reach every WiFi client,
// this ESP32 included. lwIP drops them because they are not for us; this hook looks at them
// first, on the frames the WiFi driver hands to the STA interface, and then passes every frame
// on untouched. No promiscuous mode, nothing sent.

/// Most sender ranges the filter holds.
static constexpr size_t MAX_RANGES = 8;

/// Inclusive IPv4 range, host byte order. A single address has first == last.
struct Range {
  uint32_t first;
  uint32_t last;
};

/// Hooks the WiFi STA interface if it is not hooked yet (also after it was recreated).
/// Call from the main loop once the network is up. Returns true while the hook is in place.
bool ensure_installed();

/// Watch requests for `target` (host byte order) from senders in `ranges` (at most MAX_RANGES
/// are used). No ranges or target 0: nothing is watched. Call from the main loop only, and not
/// more often than every few hundred milliseconds (the hook reads the previous filter meanwhile).
void set_filter(const Range *ranges, size_t count, uint32_t target);

/// Takes the sender (host byte order) of the oldest matching request not taken yet.
/// Returns false when there is none. Main loop only.
bool pop(uint32_t *sender);

}  // namespace esphome::wakepsx_on_bt::arp_watch
