#pragma once

// The status LED palette, as levels actually EMITTED by the WS2812, 0-255.
//
// This is the authority for both firmware paths, but only one of them can consume
// it. ESPHome's YAML cannot include a C header, and while `brightness`, the colour
// channels and `delay:` all accept `!lambda`, `interval:` is refused outright --
// ESPHome answers a lambda there with "This option is not templatable!". Wiring the
// templatable fields through lambdas would therefore unify the three values whose
// divergence is visible the moment anyone looks at the board, and miss the one
// whose divergence is invisible: two devices pulsing at different periods look
// identical unless they are side by side. So the YAML repeats these numbers as
// plain literals and cites this file, rather than pretending to a single source of
// truth with one silent hole in it.
//
// Expressed as emitted levels because that is the only unit the two paths share.
// ESPHome applies a float `brightness` multiplier to float channel ratios;
// Adafruit_NeoPixel applies an 8-bit global scale to 8-bit components. A percentage
// means different light in the two, an emitted level does not.

// Solid, one-off states: boot, radio ready, init failure, transmitting.
#define LED_LEVEL_SOLID 50

// The liveness heartbeat. Dimmer on purpose: unlike the states above this repeats
// forever, so it has to be legible in a dark room without being a nuisance in one.
#define LED_LEVEL_PULSE 23

// Heartbeat cadence. Period is pulse-start to pulse-start.
#define LED_PULSE_MS 150
#define LED_PULSE_PERIOD_MS 5000

// ESPHome only: the Wi-Fi-down pulse is amber, green against red at this ratio.
// It survives only because gamma correction is off on both paths -- under a curve
// this would emit 0.35 and read as orange-red. There is no standalone equivalent,
// since that path has no network state to report.
#define LED_AMBER_GREEN_RATIO 0.70f
