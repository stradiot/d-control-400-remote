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

This is an ESPHome project for a **Wemos LOLIN C3 Mini** (ESP32-C3) that acts as a Wi-Fi bridge to a **Dogtrace D-Control 400** dog collar remote. It exposes a Home Assistant switch that triggers the collar's beep signal over 433 MHz RF.

**Signal flow:** Home Assistant switch → ESPHome YAML → `cc1101.h` C++ → CC1101 radio chip → 433 MHz OOK RF signal → collar.

### Key files

- `d-control-400.yaml` — ESPHome config: board, Wi-Fi, LED, the template switch, and the boot button.
- `cc1101.h` — Custom C++ namespace `cc1101_ctrl` wrapping RadioLib. Handles radio init, RF config, and the timing-critical transmission loop.
- `../include/pinout.h` — SPI pin definitions for the CC1101 (SCK=1, MISO=0, MOSI=3, CS=10, GDO0=8). **SOPS-encrypted.**
- `../include/signal.h` — The raw OOK pulse timings array (`SIGNAL_BEEP`). Positive values = HIGH, negative = LOW, in microseconds. **SOPS-encrypted.** Also defines `CARRIER_FREQUENCY`, `OUTPUT_POWER`, `BIT_RATE`, `RX_BANDWIDTH`, and `TRANSMIT_REPEAT`.
- `secrets.yaml` — Wi-Fi credentials (not committed).

### Critical implementation details

**Timing-critical transmission:** `cc1101_ctrl::transmit_beep_signal()` calls `vTaskSuspendAll()` / `xTaskResumeAll()` to freeze the FreeRTOS scheduler during each pulse burst, guaranteeing microsecond accuracy. Do not introduce `delay()` or async calls inside this window.

**ESPHome timing macro conflict:** `cc1101.h` undef/redefines `delay`, `delayMicroseconds`, `millis`, `micros`, `yield` to route through `esphome::` equivalents. This must stay at the top of the file to avoid Arduino SDK conflicts.

**Wi-Fi power cap:** `output_power: 8.5dBm` in the YAML is intentional — higher power causes LDO brownouts on the C3 Mini's tiny regulator. Do not increase it.

**Hardware safety flag:** `cc1101_ctrl::is_ready()` guards all transmission paths. If the CC1101 is not detected on SPI at boot, the flag stays false and the switch silently no-ops, preventing SPI crashes.

**Encrypted headers:** `pinout.h` and `signal.h` are encrypted with SOPS + age. Decrypt with `sops -d` before editing; re-encrypt before committing.
