#include <Arduino.h>
#include <RadioLib.h>
#include <SPI.h>
#include <Adafruit_NeoPixel.h>

#include "signal.h"
#include "pinout.h"

SPIClass customSPI(FSPI);
CC1101 radio = new Module(CC1101_CS, CC1101_GDO0, RADIOLIB_NC, RADIOLIB_NC, customSPI);
Adafruit_NeoPixel strip(NUMPIXELS, LED_PIN, NEO_GRB + NEO_KHZ800);

// --- Dogtrace Signal ---
// Run lengths in BASE_TICK_US units. Positive = RF ON, negative = RF OFF.
constexpr int32_t beepTicks[] = SIGNAL_BEEP_TICKS;
constexpr size_t payloadSize = sizeof(beepTicks) / sizeof(beepTicks[0]);

// --- Runtime calibration parameters ---
// Seeded from signal.h, mutable over serial so a parameter sweep does not
// require a reflash per trial. Nothing is persisted; a reset restores defaults.
uint32_t baseTickUs = BASE_TICK_US;
uint16_t repeatCount = TRANSMIT_REPEAT;
uint32_t gapUs = TRANSMIT_GAP_US;
// Contiguous frames per burst, with no silence between them. This is the parameter
// that decides whether the collar responds at all -- see the note in signal.h. The
// remote emits 7 for a tap and 180 for a 4.1 s hold, never with an internal gap.
uint16_t framesPerBurst = FRAMES_PER_BURST;
int8_t outputPower = OUTPUT_POWER;
float carrierMHz = CARRIER_FREQUENCY;

// Timing engine selection.
//   0 = legacy   : reproduces the original firmware bit-for-bit (per-edge micros()
//                  restart, no synthesiser settle). This is the baseline to measure against.
//   1 = deadline : absolute-deadline scheduling so digitalWrite() overhead stops
//                  integrating across the frame, plus a synthesiser settle delay.
uint8_t timingMode = 0;

// Sum of all run lengths in ticks; used for the watchdog budget check.
uint32_t tickSum = 0;

// vTaskSuspendAll() is held for the whole sequence (every burst and the gaps
// between them), so it must finish well inside the 5 s task watchdog. Refuse
// settings that would not.
constexpr uint32_t WATCHDOG_BUDGET_US = 4000000;

// --- Button Interrupt ---
volatile bool buttonTriggered = false;
unsigned long lastDebounceTime = 0;
constexpr unsigned long debounceDelay = 200;

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

// --- Interrupt Service Routine ---
void IRAM_ATTR handleButton() {
    unsigned long currentTime = millis();
    if (currentTime - lastDebounceTime > debounceDelay) {
        buttonTriggered = true;
        lastDebounceTime = currentTime;
    }
}

// --- Timing helpers ---
inline uint32_t ticksOf(int32_t v) {
    return (uint32_t)(v < 0 ? -v : v);
}

uint32_t frameDurationUs() {
    return tickSum * baseTickUs;
}

// One burst = framesPerBurst frames emitted back-to-back, no gap between them.
uint32_t burstDurationUs() {
    return (uint32_t)framesPerBurst * frameDurationUs();
}

// The whole on-air sequence for one trigger: repeatCount bursts, gap between bursts.
uint32_t sequenceDurationUs() {
    return (uint32_t)repeatCount * (gapUs + burstDurationUs());
}

void transmitSequence() {
    // Suspend all FreeRTOS background tasks for absolute timing precision.
    // burstDurationUs() is validated against WATCHDOG_BUDGET_US before any
    // parameter change is accepted, so this cannot overrun the watchdog.
    vTaskSuspendAll();

    for (uint16_t repeat = 0; repeat < repeatCount; repeat++) {
        // Ensure PA is OFF during the inter-burst gap. Nothing separates the frames
        // inside a burst -- that contiguity is what the collar's decoder needs.
        digitalWrite(CC1101_GDO0, LOW);
        delayMicroseconds(gapUs);

        if (timingMode == 1) {
            // Schedule every edge against one timebase taken at the start of the
            // burst, so the per-edge digitalWrite() cost does not accumulate across
            // all framesPerBurst * payloadSize edges.
            uint32_t next = micros();
            for (uint16_t frame = 0; frame < framesPerBurst; frame++) {
                for (size_t i = 0; i < payloadSize; i++) {
                    next += ticksOf(beepTicks[i]) * baseTickUs;
                    digitalWrite(CC1101_GDO0, (beepTicks[i] > 0) ? HIGH : LOW);
                    while ((int32_t)(micros() - next) < 0);
                }
            }
        } else {
            // Legacy path: micros() is sampled after the write, so each pulse
            // is stretched by the write overhead and the error integrates.
            for (uint16_t frame = 0; frame < framesPerBurst; frame++) {
                for (size_t i = 0; i < payloadSize; i++) {
                    digitalWrite(CC1101_GDO0, (beepTicks[i] > 0) ? HIGH : LOW);

                    uint32_t start = micros();
                    uint32_t target = ticksOf(beepTicks[i]) * baseTickUs;
                    while (micros() - start < target);
                }
            }
        }

        digitalWrite(CC1101_GDO0, LOW);
    }

    // Resume normal system operations
    xTaskResumeAll();
}

void triggerTransmit() {
    setLEDColor(0, 0, 255); // Blue

    int state = radio.transmitDirect(); // Enter transparent mode
    if (state != RADIOLIB_ERR_NONE) {
        Serial.print(F("ERR transmitDirect failed, code: "));
        Serial.println(state);
        setLEDColor(255, 0, 0);
        delay(500);
        clearLED();
        return;
    }

    if (timingMode == 1) {
        delay(5); // Let the frequency synthesiser settle before the first edge
    }

    transmitSequence();

    state = radio.standby(); // Back to sleep
    if (state != RADIOLIB_ERR_NONE) {
        Serial.print(F("ERR standby failed, code: "));
        Serial.println(state);
    }

    clearLED();
}

// --- Serial calibration interface ---
void printState() {
    Serial.println(F("--- calibration state ---"));
    Serial.print(F("  k  base tick us  : ")); Serial.println(baseTickUs);
    Serial.print(F("  p  power dBm     : ")); Serial.println(outputPower);
    Serial.print(F("  b  frames/burst  : ")); Serial.println(framesPerBurst);
    Serial.print(F("  r  bursts        : ")); Serial.println(repeatCount);
    Serial.print(F("  g  inter-burst us: ")); Serial.println(gapUs);
    Serial.print(F("  f  carrier MHz   : ")); Serial.println(carrierMHz, 3);
    Serial.print(F("  m  timing mode   : "));
    Serial.println(timingMode == 1 ? F("1 (deadline + settle)") : F("0 (legacy)"));
    Serial.print(F("     payload edges : ")); Serial.println(payloadSize);
    Serial.print(F("     frame us      : ")); Serial.println(frameDurationUs());
    Serial.print(F("     burst us      : ")); Serial.println(burstDurationUs());
    Serial.print(F("     sequence us   : ")); Serial.println(sequenceDurationUs());
    Serial.println(F("  t  transmit once     ?  this help"));
}

// Rejects a change that would push the burst past the watchdog budget.
bool budgetOk() {
    uint32_t d = sequenceDurationUs();
    if (d <= WATCHDOG_BUDGET_US) {
        return true;
    }
    Serial.print(F("ERR rejected: sequence would be "));
    Serial.print(d / 1000);
    Serial.print(F(" ms, budget is "));
    Serial.print(WATCHDOG_BUDGET_US / 1000);
    Serial.println(F(" ms (task watchdog)"));
    return false;
}

void executeCommand(const char *cmd) {
    const char *arg = cmd + 1;
    while (*arg == ' ') arg++;

    switch (cmd[0]) {
        case 'k': {
            uint32_t prev = baseTickUs;
            uint32_t v = strtoul(arg, nullptr, 10);
            if (v < 20 || v > 2000) {
                Serial.println(F("ERR k out of range (20..2000)"));
                break;
            }
            baseTickUs = v;
            if (!budgetOk()) {
                baseTickUs = prev;
                break;
            }
            Serial.print(F("OK base tick = ")); Serial.print(baseTickUs);
            Serial.print(F(" us, frame = ")); Serial.print(frameDurationUs());
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
        case 'b': {
            // The sweep that matters most: how many contiguous frames the collar
            // needs before it acts. 1 = one frame then a gap (known dead), 7 = a
            // real tap. Sweep upward from 1 to find the actual threshold.
            uint16_t prev = framesPerBurst;
            long v = atol(arg);
            if (v < 1 || v > 1000) {
                Serial.println(F("ERR b out of range (1..1000)"));
                break;
            }
            framesPerBurst = (uint16_t)v;
            if (!budgetOk()) {
                framesPerBurst = prev;
                break;
            }
            Serial.print(F("OK frames/burst = ")); Serial.print(framesPerBurst);
            Serial.print(F(", burst = ")); Serial.print(burstDurationUs());
            Serial.println(F(" us"));
            break;
        }
        case 'r': {
            uint16_t prev = repeatCount;
            long v = atol(arg);
            if (v < 1 || v > 10000) {
                Serial.println(F("ERR r out of range (1..10000)"));
                break;
            }
            repeatCount = (uint16_t)v;
            if (!budgetOk()) {
                repeatCount = prev;
                break;
            }
            Serial.print(F("OK bursts = ")); Serial.println(repeatCount);
            break;
        }
        case 'g': {
            uint32_t prev = gapUs;
            uint32_t v = strtoul(arg, nullptr, 10);
            if (v > 100000) {
                Serial.println(F("ERR g out of range (0..100000)"));
                break;
            }
            gapUs = v;
            if (!budgetOk()) {
                gapUs = prev;
                break;
            }
            Serial.print(F("OK gap = ")); Serial.println(gapUs);
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
        case 'm': {
            int v = atoi(arg);
            if (v != 0 && v != 1) {
                Serial.println(F("ERR m must be 0 (legacy) or 1 (deadline)"));
                break;
            }
            timingMode = (uint8_t)v;
            Serial.print(F("OK timing mode = ")); Serial.println(timingMode);
            break;
        }
        case 't':
            Serial.println(F("TX ..."));
            triggerTransmit();
            Serial.println(F("TX done"));
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

    for (size_t i = 0; i < payloadSize; i++) {
        tickSum += ticksOf(beepTicks[i]);
    }

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

    pinMode(CC1101_GDO0, OUTPUT);
    digitalWrite(CC1101_GDO0, LOW);

    Serial.println(F("System Ready. Press BOOT button or send 't' to transmit."));
    printState();
}

void loop() {
    pollSerial();

    if (buttonTriggered) {
        Serial.println(F("Button pressed! Transmitting..."));

        triggerTransmit();

        buttonTriggered = false; // Reset the flag
        Serial.println(F("Done. Waiting for next press."));
    }
}
