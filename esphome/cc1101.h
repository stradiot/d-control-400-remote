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

    // Run lengths in BASE_TICK_US units. Positive = RF ON, negative = RF OFF.
    constexpr int32_t beep_ticks[] = SIGNAL_BEEP_TICKS;
    constexpr size_t payload_size = sizeof(beep_ticks) / sizeof(beep_ticks[0]);

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
            config_ok = config_ok && (radio->setOutputPower(OUTPUT_POWER) == RADIOLIB_ERR_NONE);
            config_ok = config_ok && (radio->setBitRate(BIT_RATE) == RADIOLIB_ERR_NONE);
            config_ok = config_ok && (radio->setRxBandwidth(RX_BANDWIDTH) == RADIOLIB_ERR_NONE);
            config_ok = config_ok && (radio->setOOK(true) == RADIOLIB_ERR_NONE);
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
    // One burst = FRAMES_PER_BURST frames emitted back-to-back with NO gap, which is
    // how the remote actually transmits. The gap goes between bursts only; putting one
    // between frames stops the collar decoding at all (see signal.h).
    //
    // bursts defaults to TRANSMIT_REPEAT; pass a smaller count for a shorter beep,
    // since the collar sounds for as long as it keeps receiving frames.
    inline bool transmit_beep_signal(uint16_t bursts = TRANSMIT_REPEAT) {
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

        for (uint16_t burst = 0; burst < bursts; ++burst) {

            // LOCK THE CPU: Suspend the ESP32 OS to guarantee microsecond accuracy.
            // Held for the whole burst (~159 ms at FRAMES_PER_BURST 7), not per frame --
            // resuming the scheduler mid-burst is what would inject the fatal gap.
            // Well inside the 5 s task watchdog; interrupts still run while suspended.
            vTaskSuspendAll();

            // Every edge is scheduled against one absolute timebase taken at the start
            // of the burst, so the per-edge digitalWrite() cost does not integrate over
            // 616 edges. Sampling micros() after each write instead would stretch every
            // tick by the write overhead -- tolerable over one 22.8 ms frame, not over a
            // 159 ms burst. The cast to int32_t makes the comparison rollover-safe.
            uint32_t next = micros();
            for (uint16_t frame = 0; frame < FRAMES_PER_BURST; ++frame) {
                for (size_t i = 0; i < payload_size; ++i) {
                    int ticks = beep_ticks[i];

                    // Positive run lengths set HIGH, negative set LOW
                    digitalWrite(CC1101_GDO0, ticks > 0 ? HIGH : LOW);
                    // Absolute run length scaled to microseconds by the symbol period
                    next += (uint32_t)std::abs(ticks) * BASE_TICK_US;
                    while ((int32_t)(micros() - next) < 0) {
                    }
                }
            }
            // Ensure the transmitter pin is safely pulled LOW after the burst finishes
            digitalWrite(CC1101_GDO0, LOW);

            // UNLOCK THE CPU: Allow ESPHome to process Wi-Fi and background tasks
            xTaskResumeAll();

            // Feed the task watchdog explicitly rather than relying on the idle task
            // getting scheduled inside the gap. Wi-Fi and lwIP sit at far higher
            // priority than idle and can consume the whole window servicing the
            // backlog that built up while the scheduler was suspended.
            esphome::App.feed_wdt();

            // Inter-BURST gap, from signal.h. delay() yields to the scheduler and is
            // what actually lets ESPHome service Wi-Fi between bursts; delayMicroseconds()
            // busy-waits, so only the sub-millisecond remainder goes through it. With the
            // gap at 0 a bare yield() still hands over a slot.
            constexpr uint32_t gap_ms = TRANSMIT_GAP_US / 1000;
            constexpr uint32_t gap_rem_us = TRANSMIT_GAP_US % 1000;
            if (gap_ms > 0) {
                delay(gap_ms);
            }
            if (gap_rem_us > 0) {
                delayMicroseconds(gap_rem_us);
            }
            if (TRANSMIT_GAP_US == 0) {
                yield();
            }
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

