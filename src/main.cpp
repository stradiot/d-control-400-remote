#include <Arduino.h>
#include <RadioLib.h>
#include <SPI.h>
#include <Adafruit_NeoPixel.h>

#include "signal.h"
#include "pinout.h"
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
// Green is scaled by strip.setBrightness(LED_BRIGHTNESS) on top of this value.
constexpr unsigned long HEARTBEAT_PERIOD_MS = 5000;
constexpr unsigned long HEARTBEAT_ON_MS = 80;
constexpr uint8_t HEARTBEAT_GREEN = 120;
unsigned long lastHeartbeatMs = 0;
bool heartbeatLit = false;

// --- Serial command buffer ---
char cmdBuf[32];
uint8_t cmdLen = 0;

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

    setLEDColor(0, 0, 255); // Blue

    int state = radio.transmitDirect(); // Enter transparent mode
    if (state != RADIOLIB_ERR_NONE) {
        Serial.print(F("ERR transmitDirect failed, code: "));
        Serial.println(state);
        setLEDColor(255, 0, 0);
        delay(500);
        clearLED();
        return false;
    }

    delay(5); // Let the frequency synthesiser settle before the first edge

    if (!rmt_beep::start()) {
        Serial.println(F("ERR rmt_beep::start failed"));
        radio.standby();
        setLEDColor(255, 0, 0);
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

    // 1. Initialize RGB LED
    strip.begin();
    strip.setBrightness(LED_BRIGHTNESS);
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
        setLEDColor(0, 255, 0); // Green

        delay(500);

        clearLED();
    } else {
        Serial.print(F("CC1101 radio initialization failed, code: "));
        Serial.println(state);
        // Flash Red if radio fails to initialize
        setLEDColor(255, 0, 0); // Red

        while (true);
    }

    // 4. Configure CC1101 settings for Dogtrace
    // Every return code is checked: a silently rejected setting would otherwise
    // look identical to a bad capture during a calibration sweep.
    bool ok = true;
    ok = ok && (radio.setFrequency(carrierMHz) == RADIOLIB_ERR_NONE);
    ok = ok && (radio.setOutputPower(outputPower) == RADIOLIB_ERR_NONE);
    ok = ok && (radio.setBitRate(BIT_RATE) == RADIOLIB_ERR_NONE);
    ok = ok && (radio.setRxBandwidth(RX_BANDWIDTH) == RADIOLIB_ERR_NONE);
    ok = ok && (radio.setOOK(true) == RADIOLIB_ERR_NONE);
    ok = ok && (radio.standby() == RADIOLIB_ERR_NONE);

    if (!ok) {
        Serial.println(F("CC1101 parameter configuration FAILED - readings will be unreliable"));
        setLEDColor(255, 0, 0); // Red
        while (true);
    }

    // 5. Claim GDO0 for the RMT. Deliberately after strip.begin(): the LED is the
    // other RMT client and both channels must agree on the group clock. Either
    // order works because both ask for APB, and this one makes the shared-clock
    // dependency visible rather than accidental.
    esp_err_t err = rmt_beep::init();
    if (err != ESP_OK) {
        Serial.print(F("RMT init FAILED, esp_err: ")); Serial.println(err);
        setLEDColor(255, 0, 0); // Red
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
    pollHeartbeat();
}
