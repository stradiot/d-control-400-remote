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
constexpr int32_t beepSignal[] = SIGNAL_BEEP;
constexpr size_t payloadSize = sizeof(beepSignal) / sizeof(beepSignal[0]);

// --- Button Interrupt ---
volatile bool buttonTriggered = false;
unsigned long lastDebounceTime = 0;
constexpr unsigned long debounceDelay = 200;

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
    radio.setFrequency(CARRIER_FREQUENCY);
    radio.setFrequencyDeviation(FREQUENCY_DEVIATION);
    radio.setOutputPower(OUTPUT_POWER);
    radio.setBitRate(BIT_RATE);
    radio.setRxBandwidth(RX_BANDWIDTH);
    radio.setOOK(false);
    radio.standby();

    pinMode(CC1101_GDO0, OUTPUT);
    digitalWrite(CC1101_GDO0, LOW);

    Serial.println(F("System Ready. Press BOOT button to transmit."));
}

void transmitSequence() {
    // Suspend all FreeRTOS background tasks for absolute timing precision
    // If the total transmission time exceeds the watchdog timeout, consider feeding the watchdog here or increasing its timeout duration.
    vTaskSuspendAll();

    for (uint16_t repeat = 0; repeat < TRANSMIT_REPEAT; repeat++) {
        digitalWrite(CC1101_GDO0, LOW);
        delayMicroseconds(TRANSMIT_GAP_US);

        for (size_t i = 0; i < payloadSize; i++) {
            int32_t duration = beepSignal[i];

            digitalWrite(CC1101_GDO0, (duration > 0) ? HIGH : LOW);

            uint32_t start = micros();
            uint32_t target = abs(duration);
            while (micros() - start < target);
        }
    }

    // Resume normal system operations
    xTaskResumeAll();
}

void loop() {
    if (buttonTriggered) {
        Serial.println(F("Button pressed! Transmitting..."));

        // Turn RGB LED BLUE while transmitting
        setLEDColor(0, 0, 255); // Blue

        radio.transmitDirect(); // Enter transparent mode
        transmitSequence();     // Fire the sequence
        radio.standby();        // Back to sleep

        // Turn RGB LED OFF after finishing
        clearLED();

        buttonTriggered = false; // Reset the flag
        Serial.println(F("Done. Waiting for next press."));
    }
}
