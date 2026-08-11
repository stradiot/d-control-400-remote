# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Commands

```bash
# Validate config without flashing
esphome config d-control-400.yaml

# Compile and flash to device over USB
esphome run d-control-400.yaml

# Flash over the air (device must be on network)
esphome run d-control-400.yaml --device d-control-400.local

# Stream logs from device
esphome logs d-control-400.yaml
```

## Architecture

This is an ESPHome project for a **Wemos LOLIN C3 Mini** (ESP32-C3) that acts as a Wi-Fi bridge to a **Dogtrace D-Control 400** dog collar remote. It exposes a Home Assistant switch that triggers the collar's beep signal over 869.525 MHz RF.

**Signal flow:** Home Assistant switch → ESPHome YAML → `cc1101.h` C++ → CC1101 radio chip → 869.525 MHz OOK RF signal → collar.

### Key files

- `d-control-400.yaml` — ESPHome config: board, Wi-Fi, LED, the template switch, and the boot button.
- `cc1101.h` — Custom C++ namespace `cc1101_ctrl` wrapping RadioLib. Handles radio init, RF config, and the timing-critical transmission loop.
- `../include/pinout.h` — SPI pin definitions for the CC1101 (SCK=1, MISO=0, MOSI=3, CS=10, GDO0=8). Plaintext, committed as-is.
- `../include/signal.h` — The OOK payload as run lengths (`SIGNAL_BEEP_TICKS`), expressed in `BASE_TICK_US` symbol periods rather than absolute microseconds. Positive = HIGH, negative = LOW; element `i` lasts `BASE_TICK_US * abs(ticks[i])`. **SOPS-encrypted.** Also defines `CARRIER_FREQUENCY`, `OUTPUT_POWER`, `BIT_RATE`, `RX_BANDWIDTH`, `FRAMES_PER_BURST`, `TRANSMIT_REPEAT` and `TRANSMIT_GAP_US`.
- `secrets.yaml` — Wi-Fi credentials (not committed).

### Critical implementation details

**Timing-critical transmission:** `cc1101_ctrl::transmit_beep_signal()` calls `vTaskSuspendAll()` / `xTaskResumeAll()` around each **burst** — all `FRAMES_PER_BURST` frames, not one frame — guaranteeing microsecond accuracy. Do not introduce `delay()` or async calls inside this window, and do not resume the scheduler between frames: the silence that introduces stops the collar decoding entirely. Edges are scheduled against one absolute `micros()` deadline per burst so per-edge `digitalWrite()` cost cannot accumulate over 616 edges.

**Watchdog:** `esphome::App.feed_wdt()` is called in every inter-burst gap. Without it a 159 ms burst plus a 5 ms gap resets the device with `ESP_RST_TASK_WDT` — the idle task, which normally feeds the watchdog, cannot run while the scheduler is suspended and is outranked by Wi-Fi and lwIP in the gap. `d-control-400.yaml` logs `esp_reset_reason()` on boot and on every API client connect; the on-boot copy only reaches USB serial, which is why the trigger is duplicated on client connect.

**ESPHome timing macro conflict:** `cc1101.h` undef/redefines `delay`, `delayMicroseconds`, `millis`, `micros`, `yield` to route through `esphome::` equivalents. This must stay at the top of the file to avoid Arduino SDK conflicts.

**Wi-Fi power cap:** `output_power: 8.5dBm` in the YAML is intentional — higher power causes LDO brownouts on the C3 Mini's tiny regulator. Do not increase it.

**Hardware safety flag:** `cc1101_ctrl::is_ready()` guards all transmission paths. If the CC1101 is not detected on SPI at boot, the flag stays false and the switch silently no-ops, preventing SPI crashes.

**Encrypted header:** `signal.h` is encrypted with SOPS + age (`.sops.yaml` matches only that path). A fresh clone will not compile until it is decrypted with `sops -d` or replaced with your own captured signal. Re-encrypt with `sops -e -i` before committing; never commit it decrypted. `pinout.h` is not encrypted.
