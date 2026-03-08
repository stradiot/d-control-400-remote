#pragma once

// ---------------------------------------------------------------------
// ESPHome Timing Overrides
// ESPHome uses custom timing functions to keep the RTOS happy.
// We must undefine the standard Arduino ones and map them to ESPHome's.
// ---------------------------------------------------------------------
#undef delay
#undef delayMicroseconds
#undef millis
#undef micros
#undef yield

#include <RadioLib.h>

#define delay(x) esphome::delay(x)
#define delayMicroseconds(x) esphome::delayMicroseconds(x)
#define millis() esphome::millis()
#define micros() esphome::micros()
#define yield() esphome::yield()

#include <SPI.h>
#include "esphome.h"

// ---------------------------------------------------------------------
// Shared Project Headers
// Pulls in the exact same pin mapping and SDR timings used in the
// standalone PlatformIO version of this project.
// ---------------------------------------------------------------------
#include "signal.h"
#include "pinout.h"

// Global pointers and state flags
SPIClass* customSPI;
CC1101* radio;
bool cc1101_is_ready = false;

// Load the signal timings and calculate the array length dynamically
const int32_t beep_signal[] = SIGNAL_BEEP;
const size_t payload_size = sizeof(beep_signal) / sizeof(beep_signal[0]);

// ---------------------------------------------------------------------
// INITIALIZATION ROUTINE
// Called exactly once during ESPHome's on_boot sequence.
// ---------------------------------------------------------------------
inline void setup_cc1101() {
    // 1. Initialize the custom SPI bus using pins from pinout.h
    customSPI = new SPIClass(FSPI);
    customSPI->begin(CC1101_SCK, CC1101_MISO, CC1101_MOSI, CC1101_CS);

    // 2. Attach the CC1101 to the custom SPI bus (CS pin is hardcoded to 1 here,
    //    assuming it's handled by RadioLib or your hardware wiring).
    radio = new CC1101(new Module(1, RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC, *customSPI));

    // 3. Attempt to establish communication with the chip
    int state = radio->begin();

    if (state == RADIOLIB_ERR_NONE) {
        ESP_LOGI("CC1101", "Radio initialized successfully!");
        cc1101_is_ready = true;

        // 4. Load the specific Dogtrace RF parameters
        radio->setFrequency(CARRIER_FREQUENCY);
        radio->setFrequencyDeviation(FREQUENCY_DEVIATION);
        radio->setOutputPower(OUTPUT_POWER);
        radio->setBitRate(BIT_RATE);
        radio->setRxBandwidth(RX_BANDWIDTH);

        // 5. Disable OOK (use FSK) and place in standby to prevent voltage brownouts
        radio->setOOK(false);
        radio->standby();
    } else {
        // Flag as failed so the YAML knows to disable buttons and show Red LED
        ESP_LOGE("CC1101", "Radio initialization failed, code: %d", state);
        cc1101_is_ready = false;
    }

    // Initialize the GDO0 pin for asynchronous manual bit-banging
    pinMode(CC1101_GDO0, OUTPUT);
    digitalWrite(CC1101_GDO0, LOW);
}

// ---------------------------------------------------------------------
// TRANSMISSION ROUTINE
// Bit-bangs the SDR timings directly into the CC1101 via GDO0.
// ---------------------------------------------------------------------
inline void transmit_beep_signal() {
    // 1. Wake the radio and set it to Asynchronous Direct Transmit mode
    radio->transmitDirect();

    // Give the frequency synthesizer 5ms to stabilize and lock
    delay(5);

    // 2. Blast the signal 50 times
    for (int repeat = 0; repeat < 50; repeat++) {

        // LOCK THE CPU: Suspend the ESP32 OS to guarantee microsecond accuracy
        vTaskSuspendAll();

        // Iterate through the positive (HIGH) and negative (LOW) microsecond timings
        for (int i = 0; i < payload_size; i++) {
            int duration = beep_signal[i];

            if (duration > 0) {
                digitalWrite(CC1101_GDO0, HIGH);
                delayMicroseconds(duration);
            } else {
                digitalWrite(CC1101_GDO0, LOW);
                delayMicroseconds(-duration); // Invert negative values to positive delay
            }
        }

        // Ensure the transmitter pin is safely pulled LOW after the array finishes
        digitalWrite(CC1101_GDO0, LOW);

        // UNLOCK THE CPU: Allow ESPHome to process Wi-Fi and background tasks
        xTaskResumeAll();

        // 5ms gap between signal bursts
        delay(5);
    }

    // 3. Return the radio to low-power sleep mode
    radio->standby();
}
