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

**Signal flow:** Home Assistant switch → ESPHome YAML → `cc1101.h` C++ → `rmt_beep.h` → RMT peripheral → GDO0 → CC1101 radio chip → 869.525 MHz OOK RF signal → collar.

### Key files

- `d-control-400.yaml` — ESPHome config: board, Wi-Fi, LED, the template switch, and the boot button.
- `cc1101.h` — Custom C++ namespace `cc1101_ctrl` wrapping RadioLib. Handles radio init and the start/finish halves of a transmission. It owns no timing and no RF settings — the register configuration comes from the shared `../include/cc1101_config.h`.
- `../include/rmt_beep.h` — Shared transmission core: packs the payload into RMT words, creates the channel, starts a looped transmission and reports when it ends. Framework-agnostic (ESP-IDF driver only, no Arduino, no ESPHome, no logging), and used verbatim by the standalone path. It also timestamps each transmission at both ends; that data is read only by the standalone path's `m` sweep, and is recorded but unused here.
- `../include/cc1101_config.h` — The six RadioLib calls that are the radio's whole RF personality: frequency, power, bit rate, RX bandwidth, OOK, standby. Shared verbatim with the standalone path, because a value changed in only one of them produces two firmwares that both build, both transmit, and differ only at range. Carrier and power are parameters defaulting to `signal.h`, which is what lets the standalone serial sweep pass its own.
- `../include/led_policy.h` — The status LED palette as levels *emitted* by the WS2812, 0-255. Consumed directly by the standalone path; this YAML repeats the numbers as literals and cites the header (see below).
- `../include/reset_reason.h` — `esp_reset_reason()` as a printable string, shared with the standalone path.
- `../include/pinout.h` — SPI pin definitions for the CC1101 (SCK=1, MISO=0, MOSI=3, CS=10, GDO0=8). Plaintext, committed as-is.
- `../include/signal.h` — The OOK payload as run lengths (`SIGNAL_BEEP_TICKS`), expressed in symbol periods rather than absolute microseconds. Positive = HIGH, negative = LOW; element `i` lasts `SYMBOL_TICKS` RMT channel ticks times `abs(ticks[i])`. **SOPS-encrypted.** Also defines `CARRIER_FREQUENCY`, `OUTPUT_POWER`, `BIT_RATE`, `RX_BANDWIDTH`, `SYMBOL_TICKS`, `RMT_RESOLUTION_HZ` and `BEEP_DURATION_MS`.
- `secrets.yaml` — `wifi_ssid`, `wifi_password` and `api_encryption_key`. Gitignored; `esphome config` fails without all three.

### Critical implementation details

**Transmission is non-blocking.** `cc1101_ctrl::begin_transmission()` puts the radio in direct mode and hands the frame to the RMT, then returns; the peripheral clocks the whole beep out of its own memory and raises an interrupt at the end. There is no timing-critical section, no `vTaskSuspendAll()`, and no watchdog exposure. `end_transmission()` puts the radio back to standby and must run in task context — it is SPI work and cannot happen in the completion interrupt.

**The script waits on the hardware, not on a delay.** `transmit_beep` is `mode: single`, so a second trigger while it runs is dropped — that is the ignore-while-busy policy, and `rmt_beep::start()` enforces it independently so the Home Assistant switch and the physical button cannot race past the script. The script uses `wait_until` on `cc1101_ctrl::is_transmitting()` with a 30 s backstop, which keeps it running for exactly as long as the beep lasts. Two things depend on that: the retrigger block, and the heartbeat's `script.is_running: transmit_beep` guard at both ends of its pulse.

**No yield before `begin_transmission()`, and one used to be needed.** The script sets the LED blue and then transmits immediately. ESPHome's light component does not write the LED inline — it sets a target state and the RMT write happens on a later main-loop pass — and the bit-banged transmit took the main loop away for the whole burst under `vTaskSuspendAll()`, so a `delay: 500ms` sat between them to force a pass through the loop while there still was one. `begin_transmission()` now returns in about 5 ms and the `wait_until` after it yields continuously, so the write lands within one loop iteration either way. That delay was removed on 2026-09-13; it bought nothing and cost half a second between the press and the air. Re-adding one is a straight latency regression.

**`gamma_correct: 1.0` on the LED is deliberate, and every brightness in the YAML depends on it.** ESPHome defaults to 2.8 and applies brightness *linearly to the 8-bit channel* before the gamma table lookup, so a low brightness indexes entries crushed to 0 or 1. At the original `brightness: 15%` the Wi-Fi-down heartbeat emitted `(1, 0, 0)` — pure red at one count out of 255, with the green channel quantised away — so amber was never actually on screen and the two Wi-Fi states were indistinguishable. The curve exists to smooth a *dimming sweep*; this LED shows three fixed colours and never sweeps. At 1.0 a brightness percentage is very nearly the emitted fraction, a colour ratio survives at any level, and the values match the standalone path, whose `Adafruit_NeoPixel` applies no curve at all.

**`rmt_symbols: 48` on the LED is mandatory.** ESPHome defaults it to 96 on the C3 (`esp32_rmt_led_strip/light.py`), which is two 48-word memory blocks, and a two-block channel claims its neighbour's. The C3 has exactly two TX-capable channels, so at the default the LED consumes both and `rmt_beep::init()` fails. The RMT group's clock source and prescale are also shared — first channel created wins — which is why the radio channel asks for `RMT_CLK_SRC_APB` explicitly rather than relying on boot order.

**Watchdog:** the task watchdog was a live failure while the frame was bit-banged (a 159 ms burst plus a 5 ms gap reset the device with `ESP_RST_TASK_WDT`, because the idle task cannot run while the scheduler is suspended). The RMT path never suspends the scheduler, so a `TASK_WDT` reset here now would mean something new rather than something known. `d-control-400.yaml` still logs `esp_reset_reason()` on boot and on every API client connect; the on-boot copy only reaches USB serial, which is why the trigger is duplicated on client connect.

**ESPHome timing macro conflict:** `cc1101.h` undef/redefines `delay`, `delayMicroseconds`, `millis`, `micros`, `yield` to route through `esphome::` equivalents. This must stay at the top of the file to avoid Arduino SDK conflicts. `rmt_beep.h` deliberately uses none of them — the hardware does the timing — so it is unaffected either way.

**Wi-Fi power cap:** `output_power: 8.5dBm` in the YAML is intentional — higher power causes LDO brownouts on the C3 Mini's tiny regulator. Do not increase it.

**Hardware safety flag:** `cc1101_ctrl::is_ready()` guards all transmission paths. If the CC1101 is not detected on SPI at boot, the flag stays false and the switch silently no-ops, preventing SPI crashes.

**Encrypted header:** `signal.h` is encrypted with SOPS + age (`.sops.yaml` carries a rule for it and one for `signal_captures.txt`, to the same age recipient). A fresh clone will not compile until it is decrypted with `sops -d` or replaced with your own captured signal. Never commit it decrypted. If the plaintext was only read — which is the usual case, since decrypting is just to make the build work — restore it with `git checkout -- include/signal.h` rather than re-encrypting: `sops -e -i` rolls a fresh data key and MAC, so it produces a full-file diff for byte-identical content. Re-encrypt only when the payload was genuinely edited. `pinout.h` is not encrypted.
