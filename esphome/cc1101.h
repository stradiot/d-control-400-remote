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
#include "rmt_beep.h"

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
            // RADIOLIB_NC for the GPIO argument: GDO0 belongs to the RMT peripheral
            // and RadioLib must never drive or reconfigure it.
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
        } else {
            ESP_LOGE("CC1101", "Radio initialization failed, code: %d", state);
            radio_ready = false;

            return false;
        }

        // Claim GDO0 for the RMT. The status LED (esp32_rmt_led_strip) is the other
        // RMT client on this board and the group clock source is shared, so this can
        // only succeed if both ask for APB -- see the note in signal.h. It runs after
        // the LED component's own setup because on_boot priority 600 does.
        esp_err_t err = rmt_beep::init();
        if (err != ESP_OK) {
            ESP_LOGE("CC1101", "RMT channel setup failed: %s", esp_err_to_name(err));
            radio_ready = false;

            return false;
        }

        ESP_LOGI("CC1101", "RMT ready: %u runs, frame %u us, %u frames per beep",
                 (unsigned) rmt_beep::kRunCount, (unsigned) rmt_beep::frame_duration_us(),
                 (unsigned) rmt_beep::frame_count());

        radio_ready = true;
        return true;
    }

    // ---------------------------------------------------------------------
    // TRANSMISSION
    // ---------------------------------------------------------------------
    // The RMT peripheral clocks the whole beep out of its own memory: one contiguous
    // run of frames, no silence anywhere inside it, no scheduler suspension and no
    // watchdog exposure. Nothing here blocks -- begin() queues the transaction and
    // returns, and the transmission ends in an interrupt.
    //
    // A trigger arriving while a beep is on the air is IGNORED. The ESPHome script is
    // `mode: single`, which drops a second execution, and rmt_beep::start() refuses
    // one independently so the Home Assistant switch and the physical button cannot
    // race each other into a half-started transmission.
    inline bool begin_transmission() {
        if (!radio_ready || radio == nullptr) {
            ESP_LOGE("CC1101", "Radio not ready for transmission");
            return false;
        }
        if (rmt_beep::is_busy()) {
            ESP_LOGW("CC1101", "Beep already in flight - trigger ignored");
            return false;
        }

        // Wake the radio into asynchronous direct transmit mode
        if (radio->transmitDirect() != RADIOLIB_ERR_NONE) {
            ESP_LOGE("CC1101", "Failed to enter transmitDirect mode");
            return false;
        }
        delay(5); // Stabilize synthesizer before the first edge

        if (!rmt_beep::start()) {
            ESP_LOGE("CC1101", "Failed to queue the RMT transmission");
            radio->standby();
            return false;
        }

        return true;
    }

    inline bool is_transmitting() { return rmt_beep::is_busy(); }

    // Puts the radio back to sleep once the hardware has finished. SPI work, so it
    // must run in task context rather than in the RMT's completion interrupt.
    inline void end_transmission() {
        if (radio == nullptr) {
            return;
        }
        if (radio->standby() != RADIOLIB_ERR_NONE) {
            ESP_LOGE("CC1101", "Failed to return radio to standby");
        }
    }

    inline bool is_ready() {
        return radio_ready;
    }
}
