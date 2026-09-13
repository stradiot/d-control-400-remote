#pragma once

// Why the chip last reset, as a printable string. Shared by both firmware paths.
//
// BROWNOUT means the supply rail sagged; TASK_WDT / INT_WDT mean the scheduler was
// starved. The two are indistinguishable from outside the board and have opposite
// fixes, which is the whole reason for reporting it rather than guessing. The
// watchdog half was a live failure while the frame was bit-banged under
// vTaskSuspendAll(); nothing suspends the scheduler now, so a watchdog reset here
// would mean something new rather than something already known.
//
// esp_reset_reason() reads a latched RTC register, so it stays valid for the whole
// boot and can be re-read as often as a log consumer needs it -- which is what lets
// the ESPHome path repeat it on every API client connect, long after boot.

#include "esp_system.h"

namespace reset_reason {

inline const char *describe() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "POWERON (normal cold boot)";
        case ESP_RST_SW:        return "SW (esp_restart, e.g. OTA)";
        case ESP_RST_PANIC:     return "PANIC (exception / abort)";
        case ESP_RST_INT_WDT:   return "INT_WDT (interrupt watchdog)";
        case ESP_RST_TASK_WDT:  return "TASK_WDT (task watchdog)";
        case ESP_RST_WDT:       return "WDT (other watchdog)";
        case ESP_RST_BROWNOUT:  return "BROWNOUT (supply rail sagged)";
        case ESP_RST_EXT:       return "EXT (reset pin)";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        default:                return "UNKNOWN";
    }
}

}  // namespace reset_reason
