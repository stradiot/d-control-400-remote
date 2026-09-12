#pragma once

// Transmission core, shared by both firmware paths.
//
// The OOK envelope is clocked out of the ESP32-C3's RMT peripheral instead of being
// bit-banged from the CPU. The whole frame lives in the channel's own RAM and the
// hardware loops it, so a beep is one contiguous run of frames with no silence
// anywhere inside it, no vTaskSuspendAll(), no watchdog exposure, and timing that
// is immune to Wi-Fi activity. Nothing here blocks: rmt_transmit() queues the
// transaction and returns, and the transmission ends in an ISR.
//
// This header is deliberately free of Arduino and ESPHome: it talks only to the
// ESP-IDF RMT driver and signal.h, does no logging, and reports failure through
// return values so each path can log in its own idiom.

#include <stddef.h>
#include <stdint.h>

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_attr.h"
#include "soc/soc_caps.h"

#include "pinout.h"
#include "signal.h"

namespace rmt_beep {

// --- Payload, as stored ---
// Run lengths in symbol periods. Positive = RF ON (GDO0 HIGH), negative = RF OFF.
inline constexpr int32_t kTicks[] = SIGNAL_BEEP_TICKS;
inline constexpr size_t kRunCount = sizeof(kTicks) / sizeof(kTicks[0]);

// Two 16-bit pulse codes per 32-bit RMT word. An odd run count would leave a
// half-filled word AND could not be looped at all -- the hardware replays each
// entry's stored level bit verbatim, so an odd-length frame would put two runs of
// the same level against each other at the loop seam and merge them.
static_assert(kRunCount % 2 == 0, "frame must have an even number of runs to loop");
inline constexpr size_t kWordCount = kRunCount / 2;

// 44 data words plus the one word the driver appends for the end marker. Loop mode
// cannot use TX wrap refill, so the whole frame has to stay resident in the block.
static_assert(kWordCount + 1 <= SOC_RMT_MEM_WORDS_PER_CHANNEL,
              "frame + end marker must fit one RMT memory block");

// RMT_TX_LOOP_NUM_CHn is bits 18:9 of RMT_CHnCONF1_REG -- ten bits, and a count
// rather than an index. Past this the driver chains a second batch by stopping and
// restarting the channel, and the seam that introduces has never been measured.
inline constexpr uint32_t kMaxFramesPerBatch = 1023;

constexpr uint32_t abs_ticks(int32_t v) { return (uint32_t)(v < 0 ? -v : v); }

// Frame length in symbol periods: 109 for the beep frame, which is 88 runs plus one
// extra period for each of the 21 two-tick runs.
constexpr uint32_t frame_symbol_periods() {
    uint32_t sum = 0;
    for (size_t i = 0; i < kRunCount; ++i) {
        sum += abs_ticks(kTicks[i]);
    }
    return sum;
}

// --- Runtime parameters ---
// Seeded from signal.h and mutable so a sweep does not need a reflash. Changing
// either rebuilds nothing the hardware is currently reading: both are refused while
// a transmission is in flight.
inline uint32_t symbol_ticks = SYMBOL_TICKS;
inline uint32_t beep_duration_ms = BEEP_DURATION_MS;

inline uint32_t frame_channel_ticks() { return frame_symbol_periods() * symbol_ticks; }

inline uint32_t frame_duration_us() {
    return (uint32_t)(((uint64_t)frame_channel_ticks() * 1000000ULL) / RMT_RESOLUTION_HZ);
}

inline uint32_t symbol_period_ns() {
    return (uint32_t)(((uint64_t)symbol_ticks * 1000000000ULL) / RMT_RESOLUTION_HZ);
}

// Whole frames for the requested duration, rounded to nearest. The collar sounds
// for as long as it keeps receiving, so the count IS the duration.
inline uint32_t frame_count() {
    uint64_t want_ticks = ((uint64_t)beep_duration_ms * RMT_RESOLUTION_HZ) / 1000ULL;
    uint64_t per_frame = frame_channel_ticks();
    uint64_t n = (want_ticks + per_frame / 2) / per_frame;
    return n < 1 ? 1 : (uint32_t)n;
}

inline uint32_t beep_duration_actual_us() { return frame_count() * frame_duration_us(); }

// --- State ---
inline rmt_channel_handle_t channel = nullptr;
inline rmt_encoder_handle_t encoder = nullptr;
inline rmt_symbol_word_t frame[kWordCount];
inline volatile bool busy = false;

// Runs on the loop-end interrupt. The C3 has no loop auto-stop -- SOC_RMT_SUPPORT_
// TX_LOOP_COUNT is set but SOC_RMT_SUPPORT_TX_LOOP_AUTO_STOP is not -- so the driver
// issues rmt_ll_tx_stop() from its own ISR before calling this, which means a few
// entries past the target can still have reached the air. Harmless here: the last
// frame is truncated rather than absent, exactly as the real remote's is when the
// button is released.
inline bool IRAM_ATTR on_done(rmt_channel_handle_t, const rmt_tx_done_event_data_t *, void *) {
    busy = false;
    return false;  // no higher-priority task woken
}

// Packs the stored run lengths into the RMT's two-codes-per-word format. Runs
// alternate by construction, so every word is exactly one (ON, OFF) pair.
//
// No end marker is written here. The driver appends its own zero-period word after
// the payload (rmt_tx_mark_eof), and that marker is what the loop counter counts.
inline void build_frame() {
    for (size_t w = 0; w < kWordCount; ++w) {
        const int32_t on = kTicks[2 * w];
        const int32_t off = kTicks[2 * w + 1];
        frame[w].level0 = on > 0 ? 1 : 0;
        frame[w].duration0 = abs_ticks(on) * symbol_ticks;
        frame[w].level1 = off > 0 ? 1 : 0;
        frame[w].duration1 = abs_ticks(off) * symbol_ticks;
    }
}

// The period field is 15 bits. Nothing in this payload exceeds two symbol periods,
// so this only ever binds when symbol_ticks is swept upward.
constexpr bool ticks_representable(uint32_t k) { return k > 0 && 2 * k <= 32767; }

constexpr bool frames_representable(uint32_t k, uint32_t duration_ms) {
    uint64_t per_frame = (uint64_t)frame_symbol_periods() * k;
    uint64_t want = ((uint64_t)duration_ms * RMT_RESOLUTION_HZ) / 1000ULL;
    return ((want + per_frame / 2) / per_frame) <= kMaxFramesPerBatch;
}

static_assert(ticks_representable(SYMBOL_TICKS),
              "SYMBOL_TICKS makes a run exceed the RMT's 15-bit period field");
static_assert(frames_representable(SYMBOL_TICKS, BEEP_DURATION_MS),
              "BEEP_DURATION_MS needs more than one 1023-frame RMT loop batch");

// --- API ---

// Creates the channel and encoder and enables the channel. GDO0 rests at the idle
// level from this point on, so call it after the CC1101 is configured and before
// anything expects the pin to be driven.
//
// RMT_CLK_SRC_APB is requested explicitly rather than left to init order. The clock
// source and the group prescale belong to the RMT group, not the channel, and the
// first channel created fixes both; on this board the status LED usually gets there
// first and asks for RMT_CLK_SRC_DEFAULT, which is APB on the C3. A mismatch is a
// hard ESP_ERR_INVALID_ARG ("group clock conflict"), not a fallback.
inline esp_err_t init() {
    if (channel != nullptr) {
        return ESP_OK;
    }

    build_frame();

    rmt_tx_channel_config_t channel_config = {};
    channel_config.gpio_num = (gpio_num_t)CC1101_GDO0;
    channel_config.clk_src = RMT_CLK_SRC_APB;
    channel_config.resolution_hz = RMT_RESOLUTION_HZ;
    // One memory block. The C3 has only two TX-capable channels and the status LED
    // holds the other, so taking two blocks would leave it nowhere to go.
    channel_config.mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL;
    channel_config.trans_queue_depth = 1;
    channel_config.intr_priority = 0;
    channel_config.flags.invert_out = false;
    channel_config.flags.with_dma = false;
    channel_config.flags.io_loop_back = false;
    channel_config.flags.io_od_mode = false;
    // Carrier off while idle, from channel creation onward. GDO0 gates the CC1101's
    // power amplifier, so a pin left high is an unmodulated carrier on 869.525 MHz.
    channel_config.flags.init_level = 0;

    esp_err_t err = rmt_new_tx_channel(&channel_config, &channel);
    if (err != ESP_OK) {
        channel = nullptr;
        return err;
    }

    rmt_copy_encoder_config_t encoder_config = {};
    err = rmt_new_copy_encoder(&encoder_config, &encoder);
    if (err != ESP_OK) {
        rmt_del_channel(channel);
        channel = nullptr;
        encoder = nullptr;
        return err;
    }

    rmt_tx_event_callbacks_t callbacks = {};
    callbacks.on_trans_done = on_done;
    err = rmt_tx_register_event_callbacks(channel, &callbacks, nullptr);
    if (err != ESP_OK) {
        return err;
    }

    return rmt_enable(channel);
}

inline bool is_ready() { return channel != nullptr && encoder != nullptr; }

inline bool is_busy() { return busy; }

// Queues one beep and returns immediately; the hardware transmits for
// beep_duration_actual_us() afterwards. A trigger that arrives while a beep is
// already on the air is IGNORED rather than queued or restarted -- this device
// exists to emit one precise beep per press, not to replicate the original
// remote's press-and-hold behaviour.
//
// Returns false if the channel is not initialised, a beep is already running, or
// the driver refused the transaction.
inline bool start() {
    if (!is_ready() || busy) {
        return false;
    }

    rmt_transmit_config_t transmit_config = {};
    transmit_config.loop_count = (int)frame_count();
    // Sets both the appended end marker's level bits and RMT_IDLE_OUT_LV, which is
    // what the pin falls back to when the driver aborts the loop mid-frame.
    transmit_config.flags.eot_level = 0;
    transmit_config.flags.queue_nonblocking = true;

    busy = true;
    esp_err_t err = rmt_transmit(channel, encoder, frame, sizeof(frame), &transmit_config);
    if (err != ESP_OK) {
        busy = false;
        return false;
    }
    return true;
}

// Symbol-period sweep. Rescales every run at once, which is the whole point of
// storing the payload as run lengths rather than absolute durations.
inline bool set_symbol_ticks(uint32_t k) {
    if (busy || !ticks_representable(k) || !frames_representable(k, beep_duration_ms)) {
        return false;
    }
    symbol_ticks = k;
    build_frame();
    return true;
}

inline bool set_duration_ms(uint32_t ms) {
    if (busy || ms == 0 || !frames_representable(symbol_ticks, ms)) {
        return false;
    }
    beep_duration_ms = ms;
    return true;
}

}  // namespace rmt_beep
