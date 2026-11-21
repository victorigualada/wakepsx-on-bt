#pragma once

#include <cstdint>

namespace esphome::wakepsx_on_bt::arp_watch {

// Counts the ARP requests one device sends for the console's IP, as seen on the ESP32's WiFi.
//
// When the PS Remote Play app (or a client such as Asobi) looks for a console that is in rest
// mode or off, the device it runs on keeps asking "who has <console IP>?" over ARP, every few
// seconds, and nobody answers. Those requests are broadcast, so they reach every WiFi client,
// this ESP32 included. lwIP drops them because they are not for us; this hook looks at them
// first, on the frames the WiFi driver hands to the STA interface, and then passes every frame
// on untouched. No promiscuous mode, nothing sent.

/// Hooks the WiFi STA interface if it is not hooked yet (also after it was recreated).
/// Call from the main loop once the network is up. Returns true while the hook is in place.
bool ensure_installed();

/// Count requests from `sender` asking for `target` (IPv4, network byte order). 0 disables.
void set_filter(uint32_t sender, uint32_t target);

/// Matching requests seen since the previous call.
uint32_t take_count();

}  // namespace esphome::wakepsx_on_bt::arp_watch
