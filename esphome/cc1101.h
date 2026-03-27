#pragma once

// ESPHome Timing Overrides
#undef delay
#undef delayMicroseconds
#undef millis
#undef micros
#undef yield

#include <RadioLib.h>
#include <SPI.h>

#include "esphome.h"
#include "signal.h"
#include "pinout.h"

#define delay(x) esphome::delay(x)
#define delayMicroseconds(x) esphome::delayMicroseconds(x)
#define millis() esphome::millis()
#define micros() esphome::micros()
#define yield() esphome::yield()

namespace cc1101_ctrl {
    SPIClass* radioSPI = nullptr;
    Module* radioModule = nullptr;
    CC1101* radio = nullptr;
    bool radio_ready = false;

    constexpr int32_t beep_signal[] = SIGNAL_BEEP;
    constexpr size_t payload_size = sizeof(beep_signal) / sizeof(beep_signal[0]);

    inline bool setup() {
        if (radio_ready) {
            return true;
        }

        if (radioSPI == nullptr) {
            radioSPI = new SPIClass(FSPI);
            if (!radioSPI) {
                ESP_LOGE("CC1101", "Failed to allocate SPIClass");
                return false;
            }
            radioSPI->begin(CC1101_SCK, CC1101_MISO, CC1101_MOSI, CC1101_CS);
        }

        if (radioModule == nullptr) {
            radioModule = new Module(CC1101_CS, RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC, *radioSPI);
        }

        if (radio == nullptr) {
            radio = new CC1101(radioModule);
            if (!radio) {
                ESP_LOGE("CC1101", "Failed to allocate CC1101");
                return false;
            }
        }
        int state = radio->begin();

        if (state == RADIOLIB_ERR_NONE) {
            ESP_LOGI("CC1101", "Radio initialized successfully!");
            // Use && to short-circuit if any of these fail

            bool config_ok = true;
            config_ok = config_ok && (radio->setFrequency(CARRIER_FREQUENCY) == RADIOLIB_ERR_NONE);
            config_ok = config_ok && (radio->setFrequencyDeviation(FREQUENCY_DEVIATION) == RADIOLIB_ERR_NONE);
            config_ok = config_ok && (radio->setOutputPower(OUTPUT_POWER) == RADIOLIB_ERR_NONE);
            config_ok = config_ok && (radio->setBitRate(BIT_RATE) == RADIOLIB_ERR_NONE);
            config_ok = config_ok && (radio->setRxBandwidth(RX_BANDWIDTH) == RADIOLIB_ERR_NONE);
            config_ok = config_ok && (radio->setOOK(false) == RADIOLIB_ERR_NONE);
            config_ok = config_ok && (radio->standby() == RADIOLIB_ERR_NONE);

            if (!config_ok) {
                ESP_LOGE("CC1101", "Failed to configure radio parameters");
                radio_ready = false;

                return false;
            }

            radio_ready = true;
        } else {
            ESP_LOGE("CC1101", "Radio initialization failed, code: %d", state);
            radio_ready = false;

            return false;
        }

        pinMode(CC1101_GDO0, OUTPUT);
        digitalWrite(CC1101_GDO0, LOW);

        return true;
    }

    // ---------------------------------------------------------------------
    // TRANSMISSION ROUTINE
    // Bit-bangs the SDR timings directly into the CC1101 via GDO0.
    // ---------------------------------------------------------------------
    inline bool transmit_beep_signal() {
        if (!radio_ready || radio == nullptr) {
            ESP_LOGE("CC1101", "Radio not ready for transmission");
            return false;
        }
        // 1. Wake the radio and set it to Asynchronous Direct Transmit mode
        if (radio->transmitDirect() != RADIOLIB_ERR_NONE) {
            ESP_LOGE("CC1101", "Failed to enter transmitDirect mode");
            return false;
        }
        delay(5); // Stabilize synthesizer

        for (int repeat = 0; repeat < TRANSMIT_REPEAT; ++repeat) {

            // LOCK THE CPU: Suspend the ESP32 OS to guarantee microsecond accuracy
            vTaskSuspendAll();

            // Iterate through the positive (HIGH) and negative (LOW) microsecond timings
            for (size_t i = 0; i < payload_size; ++i) {
                int duration = beep_signal[i];

                 // Positive durations set HIGH, negative durations set LOW
                digitalWrite(CC1101_GDO0, duration > 0 ? HIGH : LOW);
                 // Use absolute value for delay duration
                delayMicroseconds(std::abs(duration));
            }
            // Ensure the transmitter pin is safely pulled LOW after the array finishes
            digitalWrite(CC1101_GDO0, LOW);

            // UNLOCK THE CPU: Allow ESPHome to process Wi-Fi and background tasks
            xTaskResumeAll();
            // 5ms gap between signal bursts
            delay(5);
        }

        if (radio->standby() != RADIOLIB_ERR_NONE) {
            ESP_LOGE("CC1101", "Failed to return radio to standby");

            return false;
        }

        return true;
    }

    inline bool is_ready() {
        return radio_ready;
    }
}

