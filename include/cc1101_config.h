#pragma once

// CC1101 register configuration, shared by both firmware paths.
//
// These six calls are the radio's entire RF personality -- everything that decides
// what leaves the antenna, short of the payload itself. They were duplicated
// verbatim in src/main.cpp and esphome/cc1101.h, which is the one duplication in
// this repo a compiler cannot catch: a frequency or a bandwidth changed in only one
// path yields two firmwares that both build, both transmit, and differ at range.
//
// Carrier and power are parameters rather than constants because the standalone
// path sweeps them at runtime from its serial interface ('f' and 'p'). They default
// to the captured values, so the ESPHome path never has to mention them.
//
// Every return code is checked. A silently rejected setting would otherwise look
// identical to a bad capture during a calibration sweep.
//
// This header talks only to RadioLib and signal.h: no Arduino, no ESPHome, no
// logging, failure reported by return value, so each path logs in its own idiom.

#include <RadioLib.h>

#include "signal.h"

namespace cc1101_config {

inline bool apply(CC1101 &radio,
                  float carrier_mhz = CARRIER_FREQUENCY,
                  int8_t output_power = OUTPUT_POWER) {
    bool ok = true;
    ok = ok && (radio.setFrequency(carrier_mhz) == RADIOLIB_ERR_NONE);
    ok = ok && (radio.setOutputPower(output_power) == RADIOLIB_ERR_NONE);
    ok = ok && (radio.setBitRate(BIT_RATE) == RADIOLIB_ERR_NONE);
    ok = ok && (radio.setRxBandwidth(RX_BANDWIDTH) == RADIOLIB_ERR_NONE);
    ok = ok && (radio.setOOK(true) == RADIOLIB_ERR_NONE);
    ok = ok && (radio.standby() == RADIOLIB_ERR_NONE);
    return ok;
}

}  // namespace cc1101_config
