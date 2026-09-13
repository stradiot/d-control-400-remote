#include <Arduino.h>
#include <RadioLib.h>
#include <SPI.h>
#include <Adafruit_NeoPixel.h>

#include "signal.h"
#include "pinout.h"
#include "led_policy.h"
#include "cc1101_config.h"
#include "reset_reason.h"
#include "rmt_beep.h"

SPIClass customSPI(FSPI);
// RADIOLIB_NC for the GPIO argument: GDO0 belongs to the RMT peripheral, and
// RadioLib must never drive or reconfigure it. The CC1101 side of the pin is set up
// by transmitDirect() over SPI, which is a register write and needs no MCU pin.
CC1101 radio = new Module(CC1101_CS, RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC, customSPI);
Adafruit_NeoPixel strip(NUMPIXELS, LED_PIN, NEO_GRB + NEO_KHZ800);

// --- Runtime calibration parameters ---
// Seeded from signal.h, mutable over serial so a parameter sweep does not
// require a reflash per trial. Nothing is persisted; a reset restores defaults.
// The symbol period and the beep length live in rmt_beep.
int8_t outputPower = OUTPUT_POWER;
float carrierMHz = CARRIER_FREQUENCY;

// True between handing a beep to the RMT and putting the radio back in standby.
// The transmission itself runs in hardware, so loop() keeps running throughout.
bool txActive = false;

// --- Button Interrupt ---
volatile bool buttonTriggered = false;
unsigned long lastDebounceTime = 0;
constexpr unsigned long debounceDelay = 200;

// --- Liveness heartbeat ---
// An idle device used to be dark, which is indistinguishable from one that is
// dead or unpowered. A brief green pulse every HEARTBEAT_PERIOD_MS makes "alive
// and radio OK" visible at a glance. Short and dim on purpose: this repeats
// forever, so it must not be a nuisance in a room.
// Levels come from include/led_policy.h and are written to the strip verbatim --
// see the setBrightness(255) note in setup(). Adafruit applies no gamma curve and
// the ESPHome path now disables its own, so an emitted level means the same light
// on both paths and the two can be compared on the bench.
constexpr unsigned long HEARTBEAT_PERIOD_MS = LED_PULSE_PERIOD_MS;
constexpr unsigned long HEARTBEAT_ON_MS = LED_PULSE_MS;
constexpr uint8_t HEARTBEAT_GREEN = LED_LEVEL_PULSE;
unsigned long lastHeartbeatMs = 0;
bool heartbeatLit = false;

// --- Serial command buffer ---
char cmdBuf[32];
uint8_t cmdLen = 0;

// --- Loop-seam sweep ---
// Instrumentation for what the RMT's loop wrap costs. Answered 2026-09-13 -- nothing,
// to within +/-2 ns per frame, where one APB clock would be 12.5 ns -- and kept for
// the same reason the k sweep is kept: it is how the achieved channel clock gets
// checked at runtime instead of inferred. See the header comment on
// rmt_beep::tx_start_us for why this must be read as a slope.
//
// Both timestamps sit a fixed, unknown number of cycles away from the first and last
// edges, and a fixed offset added to one elapsed time is indistinguishable from a
// fixed per-seam cost summed over that beep -- same sign, same magnitude, same on
// every press. The frame count is the lever that separates them: the offsets do not
// scale with it and the seam does. Fit elapsed time against frames; the intercept absorbs
// every fixed cost and the slope is the per-frame period the hardware actually
// realises, wrap included.
//
// Durations, not frame counts, because set_duration_ms() is the existing knob;
// frame_count() rounds each to whole frames and last_frame_count() reports what it
// landed on. The points do not need to be round numbers, only known ones. The span
// runs from ~22 frames to just under the 1023-frame batch ceiling, which is the
// widest lever arm available without chaining a second batch.
constexpr uint32_t SWEEP_MS[] = {500, 1000, 2000, 4000, 8000, 12000, 16000, 20000, 23000};
constexpr size_t SWEEP_STEPS = sizeof(SWEEP_MS) / sizeof(SWEEP_MS[0]);
constexpr unsigned long SWEEP_GAP_MS = 2000;

bool sweepActive = false;
bool sweepPending = false;  // a beep is in flight and not yet harvested
size_t sweepStep = 0;
unsigned long sweepNextMs = 0;
uint32_t sweepSavedMs = 0;
uint32_t sweepFrames[SWEEP_STEPS];
int64_t sweepUs[SWEEP_STEPS];
uint32_t sweepCycles[SWEEP_STEPS];

// Serial.print() has no 64-bit overload, and casting to int32_t silently wrapped the
// first sweep's 3.59e9-cycle span to a negative number. Everything wide goes through
// here instead.
void printI64(int64_t v) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%lld", (long long)v);
    Serial.print(buf);
}

//
// --- LED Feedback ---
void setLEDColor(uint8_t r, uint8_t g, uint8_t b) {
    strip.setPixelColor(LED_INDEX, strip.Color(r, g, b));
    strip.show();
}

void clearLED() {
    strip.clear();
    strip.show();
}

// Non-blocking two-state pulse driven from loop(). Deliberately not a delay():
// loop() has to stay responsive to the button flag and to serial commands.
//
// The txActive guard is required, and it was not before the RMT migration. The old
// transmit call blocked loop() for the whole sequence, so the heartbeat and the
// transmission were mutually exclusive by construction. Now the beep runs in
// hardware while loop() keeps turning, so both ends of the pulse have to stand
// aside: starting one would overwrite the blue, and clearing one that began just
// before the trigger would blank the LED for the whole beep.
//
// Period is measured pulse-start to pulse-start; lastHeartbeatMs is only
// advanced when the LED lights. Unsigned arithmetic makes it millis()-rollover
// safe, same as the debounce above.
void pollHeartbeat() {
    if (txActive) {
        return;
    }

    unsigned long now = millis();

    if (!heartbeatLit) {
        if (now - lastHeartbeatMs >= HEARTBEAT_PERIOD_MS) {
            setLEDColor(0, HEARTBEAT_GREEN, 0);
            heartbeatLit = true;
            lastHeartbeatMs = now;
        }
    } else if (now - lastHeartbeatMs >= HEARTBEAT_ON_MS) {
        clearLED();
        heartbeatLit = false;
    }
}

// --- Interrupt Service Routine ---
void IRAM_ATTR handleButton() {
    unsigned long currentTime = millis();
    if (currentTime - lastDebounceTime > debounceDelay) {
        buttonTriggered = true;
        lastDebounceTime = currentTime;
    }
}

// Hands one beep to the RMT and returns. A press arriving while a beep is already
// on the air is ignored, not queued: the device emits one precise beep per press.
bool startTransmit() {
    if (txActive || rmt_beep::is_busy()) {
        Serial.println(F("busy - trigger ignored"));
        return false;
    }

    setLEDColor(0, 0, LED_LEVEL_SOLID); // Blue

    int state = radio.transmitDirect(); // Enter transparent mode
    if (state != RADIOLIB_ERR_NONE) {
        Serial.print(F("ERR transmitDirect failed, code: "));
        Serial.println(state);
        setLEDColor(LED_LEVEL_SOLID, 0, 0);
        delay(500);
        clearLED();
        return false;
    }

    delay(5); // Let the frequency synthesiser settle before the first edge

    if (!rmt_beep::start()) {
        Serial.println(F("ERR rmt_beep::start failed"));
        radio.standby();
        setLEDColor(LED_LEVEL_SOLID, 0, 0);
        delay(500);
        clearLED();
        return false;
    }

    txActive = true;
    return true;
}

// Called from loop(). The RMT clears its busy flag from the loop-end interrupt;
// putting the radio back to standby is SPI work and has to happen in task context.
void pollTransmit() {
    if (!txActive || rmt_beep::is_busy()) {
        return;
    }

    int state = radio.standby();
    if (state != RADIOLIB_ERR_NONE) {
        Serial.print(F("ERR standby failed, code: "));
        Serial.println(state);
    }

    txActive = false;
    clearLED();

    // Restart the heartbeat cycle rather than letting a pulse fire the instant the
    // beep ends: the beep outlasts HEARTBEAT_PERIOD_MS, so the timer is always
    // overdue by the time the transmission finishes.
    heartbeatLit = false;
    lastHeartbeatMs = millis();

    Serial.println(F("TX done"));

    // One measurement point per beep, whether it came from the sweep, a button or a
    // 't'. Raw cycles and the frame count the loop counter was actually given --
    // deliberately not a derived period, because a single point cannot give one.
    Serial.print(F("MEAS frames=")); Serial.print(rmt_beep::last_frame_count());
    Serial.print(F(" us=")); printI64(rmt_beep::last_tx_us());
    Serial.print(F(" cycles=")); Serial.println(rmt_beep::last_tx_cycles());
}

// The frame period the hardware is predicted to realise, in NANOSECONDS, derived at
// runtime rather than hardcoded so a wrong clock shows up as a wrong prediction
// instead of a silent assumption. The divider is what the driver picked for the
// requested resolution (214 off an 80 MHz APB). Nanoseconds because the exact value
// is 22742.85 us -- an integer number of nanoseconds, and not an integer number of
// microseconds, so this is the coarsest unit that stays exact: 8502 x 214 x 12.5.
uint64_t predictedNsPerFrame() {
    const uint32_t apbHz = getApbFrequency();
    const uint32_t divider = (apbHz + RMT_RESOLUTION_HZ / 2) / RMT_RESOLUTION_HZ;
    return (uint64_t)rmt_beep::frame_channel_ticks() * divider * 1000000000ULL / apbHz;
}

// Prints the sweep as raw triples, for fitting off-device. The endpoint slope below
// is a sanity check and NOT the result: two points cannot show whether the relation
// is linear, and non-linearity would mean one of the "fixed" offsets is not fixed.
void printSweepResult() {
    const uint64_t predictedNs = predictedNsPerFrame();

    Serial.println(F("--- loop-seam sweep ---"));
    Serial.print(F("  apb Hz            : ")); Serial.println(getApbFrequency());
    Serial.print(F("  channel ticks/frm : ")); Serial.println(rmt_beep::frame_channel_ticks());
    Serial.print(F("  predicted ns/frm  : ")); printI64((int64_t)predictedNs); Serial.println();
    Serial.println(F("  frames,us,cycles"));

    for (size_t i = 0; i < SWEEP_STEPS; ++i) {
        Serial.print(F("  ")); Serial.print(sweepFrames[i]);
        Serial.print(F(",")); printI64(sweepUs[i]);
        Serial.print(F(",")); Serial.println(sweepCycles[i]);
    }

    const int64_t df = (int64_t)sweepFrames[SWEEP_STEPS - 1] - (int64_t)sweepFrames[0];
    const int64_t dus = sweepUs[SWEEP_STEPS - 1] - sweepUs[0];
    const int64_t dcyc = (int64_t)sweepCycles[SWEEP_STEPS - 1] - (int64_t)sweepCycles[0];
    if (df > 0 && dus > 0) {
        // Excess per frame in nanoseconds. One channel tick is 2675 ns, so whole-tick
        // and even sub-tick wrap costs are both readable here without floating point.
        const int64_t excessNs = (dus * 1000 - (int64_t)predictedNs * df) / df;
        Serial.print(F("  endpoint dframes  : ")); printI64(df); Serial.println();
        Serial.print(F("  endpoint dus      : ")); printI64(dus); Serial.println();
        Serial.print(F("  excess ns/frm     : ")); printI64(excessNs); Serial.println();
        // The diagnostic: what the CPU performance counter's rate actually is over the
        // same span. It is not 160 MHz, and this line is the evidence rather than an
        // assumption -- see the note in rmt_beep.h.
        Serial.print(F("  cpu counter Hz    : "));
        printI64(dcyc * 1000000LL / dus); Serial.println();
    }
}

// Non-blocking sweep driver, stepped from loop(). One beep per point, harvested
// after the ISR has cleared busy and the radio is back in standby, then a gap before
// the next -- the gap is not a settling requirement, it just keeps a 23 s burst from
// running straight into the next one.
void pollSweep() {
    if (!sweepActive || txActive || rmt_beep::is_busy()) {
        return;
    }

    if (sweepPending) {
        sweepFrames[sweepStep] = rmt_beep::last_frame_count();
        sweepUs[sweepStep] = rmt_beep::last_tx_us();
        sweepCycles[sweepStep] = rmt_beep::last_tx_cycles();
        sweepPending = false;
        sweepStep++;
        sweepNextMs = millis() + SWEEP_GAP_MS;
        return;
    }

    if ((long)(millis() - sweepNextMs) < 0) {
        return;
    }

    if (sweepStep == SWEEP_STEPS) {
        sweepActive = false;
        rmt_beep::set_duration_ms(sweepSavedMs);
        printSweepResult();
        return;
    }

    if (!rmt_beep::set_duration_ms(SWEEP_MS[sweepStep]) || !startTransmit()) {
        Serial.println(F("ERR sweep aborted"));
        sweepActive = false;
        rmt_beep::set_duration_ms(sweepSavedMs);
        return;
    }
    sweepPending = true;
}

// --- Serial calibration interface ---
void printState() {
    Serial.println(F("--- calibration state ---"));
    Serial.print(F("  k  symbol ticks   : ")); Serial.print(rmt_beep::symbol_ticks);
    Serial.print(F(" (")); Serial.print(rmt_beep::symbol_period_ns());
    Serial.println(F(" ns)"));
    Serial.print(F("  d  beep ms        : ")); Serial.println(rmt_beep::beep_duration_ms);
    Serial.print(F("  p  power dBm      : ")); Serial.println(outputPower);
    Serial.print(F("  f  carrier MHz    : ")); Serial.println(carrierMHz, 3);
    Serial.print(F("     runs per frame : ")); Serial.println(rmt_beep::kRunCount);
    Serial.print(F("     frame us       : ")); Serial.println(rmt_beep::frame_duration_us());
    Serial.print(F("     frames         : ")); Serial.println(rmt_beep::frame_count());
    Serial.print(F("     actual beep us : ")); Serial.println(rmt_beep::beep_duration_actual_us());
    Serial.println(F("  t  transmit once     ?  this help"));
    Serial.println(F("  m  loop-seam sweep (~90 s of airtime -- silence the collar)"));
}

void executeCommand(const char *cmd) {
    const char *arg = cmd + 1;
    while (*arg == ' ') arg++;

    switch (cmd[0]) {
        case 'k': {
            // Symbol period sweep, in RMT channel ticks per symbol. One step is
            // 1/SYMBOL_TICKS of the period, about 1.28% at the default of 78.
            uint32_t v = strtoul(arg, nullptr, 10);
            if (!rmt_beep::set_symbol_ticks(v)) {
                Serial.println(F("ERR k rejected (busy, or period/frame count out of range)"));
                break;
            }
            Serial.print(F("OK symbol = ")); Serial.print(rmt_beep::symbol_period_ns());
            Serial.print(F(" ns, frame = ")); Serial.print(rmt_beep::frame_duration_us());
            Serial.print(F(" us, frames = ")); Serial.println(rmt_beep::frame_count());
            break;
        }
        case 'd': {
            uint32_t v = strtoul(arg, nullptr, 10);
            if (!rmt_beep::set_duration_ms(v)) {
                Serial.println(F("ERR d rejected (busy, or over 1023 frames in one batch)"));
                break;
            }
            Serial.print(F("OK beep = ")); Serial.print(rmt_beep::beep_duration_ms);
            Serial.print(F(" ms, frames = ")); Serial.print(rmt_beep::frame_count());
            Serial.print(F(", actual = ")); Serial.print(rmt_beep::beep_duration_actual_us());
            Serial.println(F(" us"));
            break;
        }
        case 'p': {
            int v = atoi(arg);
            int state = radio.setOutputPower(v);
            if (state != RADIOLIB_ERR_NONE) {
                Serial.print(F("ERR setOutputPower code: ")); Serial.println(state);
                break;
            }
            outputPower = (int8_t)v;
            Serial.print(F("OK power = ")); Serial.println(outputPower);
            break;
        }
        case 'f': {
            float v = atof(arg);
            int state = radio.setFrequency(v);
            if (state != RADIOLIB_ERR_NONE) {
                Serial.print(F("ERR setFrequency code: ")); Serial.println(state);
                break;
            }
            carrierMHz = v;
            Serial.print(F("OK carrier = ")); Serial.println(carrierMHz, 3);
            break;
        }
        case 't':
            if (startTransmit()) {
                Serial.println(F("TX ..."));
            }
            break;
        case 'm':
            // Loop-seam sweep. Long and loud: roughly 87 s of airtime plus gaps, and
            // the collar beeps for every second of it. Power the collar down or take
            // it out of range first, and mind the duty cycle in the 869.4-869.65 MHz
            // sub-band.
            if (sweepActive || txActive || rmt_beep::is_busy()) {
                Serial.println(F("ERR m rejected (busy)"));
                break;
            }
            sweepSavedMs = rmt_beep::beep_duration_ms;
            sweepStep = 0;
            sweepPending = false;
            sweepNextMs = millis();
            sweepActive = true;
            Serial.print(F("sweep started, ")); Serial.print(SWEEP_STEPS);
            Serial.println(F(" points"));
            break;
        case '?':
            printState();
            break;
        default:
            Serial.print(F("ERR unknown command '")); Serial.print(cmd[0]);
            Serial.println(F("' - send ? for help"));
            break;
    }
}

void pollSerial() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            cmdBuf[cmdLen] = '\0';
            if (cmdLen > 0) {
                executeCommand(cmdBuf);
            }
            cmdLen = 0;
        } else if (cmdLen < sizeof(cmdBuf) - 1) {
            cmdBuf[cmdLen++] = c;
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(2000); // Give serial monitor time to connect

    // Why the chip last reset. Shared with the ESPHome path -- a brownout and a
    // watchdog reset are indistinguishable from outside the board and have
    // opposite fixes. This is the development path, so it gets the diagnostic too.
    Serial.print(F("last reset reason: "));
    Serial.println(reset_reason::describe());

    // 1. Initialize RGB LED
    strip.begin();
    // 255 disables Adafruit's global scaling entirely rather than setting it to
    // maximum: setBrightness() stores b + 1 in a uint8_t, so 255 rolls over to 0,
    // and show() skips the scaling pass when the stored value is 0. Colour
    // components are then written literally, which is what lets led_policy.h state
    // emitted levels instead of a level and a scale that have to be multiplied out.
    strip.setBrightness(255);
    clearLED();

    // 2. Configure Button and attach Interrupt
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), handleButton, FALLING);

    // 3. Radio Initialization
    customSPI.begin(CC1101_SCK, CC1101_MISO, CC1101_MOSI, CC1101_CS);

    int state = radio.begin();
    if (state == RADIOLIB_ERR_NONE) {
        Serial.println(F("CC1101 radio initialized successfully!"));
        // Flash Green to indicate radio is ready
        setLEDColor(0, LED_LEVEL_SOLID, 0); // Green

        delay(500);

        clearLED();
    } else {
        Serial.print(F("CC1101 radio initialization failed, code: "));
        Serial.println(state);
        // Flash Red if radio fails to initialize
        setLEDColor(LED_LEVEL_SOLID, 0, 0); // Red

        while (true);
    }

    // 4. Configure CC1101 settings for Dogtrace. Shared with the ESPHome path --
    // see include/cc1101_config.h. The runtime carrier and power are passed in
    // rather than defaulted, so a reset restores the captured values but a sweep
    // set over serial survives a re-apply.
    bool ok = cc1101_config::apply(radio, carrierMHz, outputPower);

    if (!ok) {
        Serial.println(F("CC1101 parameter configuration FAILED - readings will be unreliable"));
        setLEDColor(LED_LEVEL_SOLID, 0, 0); // Red
        while (true);
    }

    // 5. Claim GDO0 for the RMT. Deliberately after strip.begin(): the LED is the
    // other RMT client and both channels must agree on the group clock. Either
    // order works because both ask for APB, and this one makes the shared-clock
    // dependency visible rather than accidental.
    esp_err_t err = rmt_beep::init();
    if (err != ESP_OK) {
        Serial.print(F("RMT init FAILED, esp_err: ")); Serial.println(err);
        setLEDColor(LED_LEVEL_SOLID, 0, 0); // Red
        while (true);
    }

    Serial.println(F("System Ready. Press BOOT button or send 't' to transmit."));
    printState();
}

void loop() {
    pollSerial();

    if (buttonTriggered) {
        buttonTriggered = false; // Reset the flag before acting, so a press during
                                 // the beep is dropped rather than deferred.
        Serial.println(F("Button pressed! Transmitting..."));
        startTransmit();
    }

    pollTransmit();
    pollSweep();  // after pollTransmit: it harvests the point txActive has just freed
    pollHeartbeat();
}
