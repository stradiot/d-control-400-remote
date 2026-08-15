# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Firmware for a Wemos LOLIN C3 Mini (ESP32-C3) + CC1101 868 MHz transceiver that replays a captured RF signal to trigger the beep function of a Dogtrace d-control 400 dog collar. The protocol is **not** decoded — this is a raw OOK pulse-train replay, bit-banged onto the CC1101's GDO0 pin in asynchronous direct mode. There is no packet engine, no sync word, no CRC.

Repo also carries hardware artifacts: `pcb/gerber.zip` (fab-ready), `enclosure/` (Fusion 360 + STL/3MF), `doc/` (media). Those are binary deliverables, not build inputs.

## Two firmware paths, one shared core

The same radio logic is implemented twice against two different runtimes. Any change to RF behaviour usually needs to land in **both**:

| Path | Entry point | Runtime |
| :--- | :--- | :--- |
| Standalone PlatformIO | `src/main.cpp` | Arduino, button-only, no network |
| ESPHome / Home Assistant | `esphome/d-control-400.yaml` + `esphome/cc1101.h` | ESPHome, exposes a template switch |

Both include the shared headers `include/pinout.h` and `include/signal.h`. See `esphome/CLAUDE.md` for ESPHome-specific detail.

Behavioural differences that are intentional, not drift:
- `src/main.cpp` holds `vTaskSuspendAll()` across **all** `TRANSMIT_REPEAT` bursts; `esphome/cc1101.h` suspends/resumes **per burst** so ESPHome can service Wi-Fi in the gaps. Neither suspends per *frame* — see "Burst contiguity" below.
- `cc1101.h` calls `esphome::App.feed_wdt()` in every inter-burst gap. `main.cpp` has no equivalent and does not need one — see "The two paths have different watchdog exposure" below.
- `main.cpp` keeps a `timingMode` switch (legacy per-edge vs absolute-deadline); `cc1101.h` only implements the deadline engine.
- `main.cpp` passes `CC1101_GDO0` as RadioLib's GPIO arg; `cc1101.h` passes `RADIOLIB_NC` and drives the pin manually.

## Build & flash

```bash
# Standalone firmware (PlatformIO)
pio run                                      # build only
pio run --target upload                      # flash
pio run --target upload --target monitor     # flash + serial @115200

# ESPHome (run from esphome/)
esphome config d-control-400.yaml            # validate without flashing
esphome run d-control-400.yaml               # compile + flash over USB
esphome run d-control-400.yaml --device d-control-400.local   # OTA
esphome logs d-control-400.yaml
```

`upload_port` / `monitor_port` in `platformio.ini` are hardcoded to `/dev/tty.usbmodem101` — expect to change these per machine.

There are no tests. `test/`, `lib/`, and `include/README` are empty PlatformIO scaffolding placeholders.

## A fresh clone does not compile

`include/signal.h` is **SOPS + age encrypted** (see `.sops.yaml`) — the checked-in file is JSON ciphertext, not C. Building requires either decrypting it (`sops -d include/signal.h`) with the age key, or replacing it with your own captured signal using the template in the README's "Adding Custom Signal" section. `include/pinout.h` is plaintext and committed as-is.

`signal.h` must define: `CARRIER_FREQUENCY`, `OUTPUT_POWER`, `BIT_RATE`, `RX_BANDWIDTH`, `FRAMES_PER_BURST`, `TRANSMIT_REPEAT`, `TRANSMIT_GAP_US`, `BASE_TICK_US`, and `SIGNAL_BEEP_TICKS`. All are used by both paths.

`SIGNAL_BEEP_TICKS` is a brace-enclosed initializer of signed **run lengths in `BASE_TICK_US` units** — not absolute microseconds. **Positive = RF ON (GDO0 HIGH), negative = RF OFF**; absolute duration of element `i` is `BASE_TICK_US * abs(ticks[i])`. Both firmware paths consume it as `constexpr int32_t[]` and derive length via `sizeof`.

This encoding is deliberate: the payload's elements are all ±1 or ±2, so the entire 88-element frame is parameterised by the single scalar `BASE_TICK_US`. That is what makes the symbol-period sweep (serial `k` command) possible without a re-capture. Do not "simplify" this back to absolute microseconds — it would destroy the ability to calibrate.

The age private key lives at SOPS's default key location, so `sops -d -i include/signal.h` works with no env var and no flag. **The path is platform-dependent** — SOPS resolves it via Go's `os.UserConfigDir()`:

- **macOS:** `~/Library/Application Support/sops/age/keys.txt`
- **Linux:** `~/.config/sops/age/keys.txt`

Using the Linux path on macOS fails with `identity did not match any of the recipients`, which is misleading: the key was never loaded, not mismatched. SOPS's error lists only the `SOPS_AGE_*` env vars it checked and never names the default path, so it reads like a wrong key when it means no key.

Verify with `age-keygen -y <path>` — that prints the *public* key, which must equal the recipient in `.sops.yaml`. Matching it proves decryption will work without touching the real file, and without putting the private key on screen.

Re-encrypt with `sops -e -i include/signal.h` before committing. Never commit a decrypted `signal.h`. If the plaintext was only read and not edited, restore a pre-decrypt snapshot instead — `sops -e -i` rolls a fresh data key and MAC, so it diffs against HEAD even for identical content.

Decrypt from the repo root and do not `cd` afterwards. A cleanup step holding a *relative* path stops protecting anything the moment the shell changes directory — that is how a decrypted `signal.h` was once left in the working tree.

`esphome/secrets.yaml` (Wi-Fi creds + `api_encryption_key`) is gitignored and must be created locally.

## Burst contiguity — the rule that actually decides whether it works

The single most important property of this firmware, and the least obvious.

**Frames within a burst must be emitted back-to-back with no silence between them.** The real remote never inserts a gap mid-transmission: a tap is 7 contiguous frames (159.3 ms), a 4.14 s hold is 180 contiguous frames, and in neither case is there an OFF run longer than 2 ticks anywhere inside. Verified against three captures, 13 presses, 2026-08-11.

A build that sent **one** frame followed by a 5 ms gap produced **zero** beeps, while the older, badly mistimed payload that happened to contain 2.5 frames per burst worked ~70% of the time. The likely mechanisms are consecutive-frame validation in the collar's decoder (it wants the next frame immediately, and a gap expires that timer) and AGC settling (5 ms of no carrier lets the receiver's gain walk off and corrupt the next frame's preamble). Either way, contiguity is the requirement.

So the knobs are layered, and they are not independent:
- `FRAMES_PER_BURST` — contiguous frames, **no gap between them**. 7 matches a real tap and is known-good.
- `TRANSMIT_GAP_US` — silence *between bursts only*. Bounded by audibility: 5 ms is inaudible, 30 ms audibly chops the beep.
- `TRANSMIT_REPEAT` — number of bursts, i.e. beep duration.

The historic 70% reliability was never a timing problem. It was burst structure, and the old payload only ever worked by accident.

## TODO

### 1. Move transmission from the CPU to the RMT peripheral

The gap exists solely because the waveform is bit-banged from the CPU with the scheduler suspended, which forces a three-way trade between contiguity, watchdog starvation, and audible chopping. The ESP32-C3's RMT peripheral clocks a pulse train out of a buffer in hardware with no CPU involvement, which removes all three constraints at once — no `vTaskSuspendAll()`, no gap, no watchdog exposure, and timing immune to Wi-Fi activity. It would also allow a genuinely continuous transmission for the full beep duration, exactly like a button hold, which the current design cannot do at all.

The frame fits: each RMT symbol holds two level+duration entries, so 88 runs = **44 symbols**, against a 48-symbol channel block on the C3. Open question is whether the C3 supports hardware TX looping or whether continuous output needs a wrap-around refill interrupt (ping-pong on the half-buffer threshold).

### 2. Capture and analyse the shock signal, all levels, and the B channel

Currently only the beep on channel A is captured, and the protocol is not decoded — this is a verified replay, not a decode. The next capture campaign should cover the **shock function at several intensity levels** and the **B channel**, because that is the differential data that makes a decode tractable: frames that differ only in the level field localise where intensity is encoded, and A vs B localises the channel field. One frame in isolation is undecodable; a family of frames that vary along known axes is not.

Expect this to reveal the frame layout — where the remote's identity sits, where the command sits, and whether there is a checksum. Note the safety asymmetry: capturing shock frames means being able to *transmit* them, so the collar must not be on the dog during this work.

Beep-only remains the scope of the shipped firmware. This item is about understanding the protocol, not extending the device's capability.

## RF constraints that look wrong but aren't

- **Bit rate 100 kbps for a ~5 kBaud signal.** Deliberate 20x oversampling. In async mode the CC1101 samples GDO0 on its internal clock; at 5 kbps the PA gating jitters badly. Do not "fix" this to match the actual baud.
- **`RX_BANDWIDTH` set on a TX-only device.** Selects a stable hardware filter tap off the 26 MHz crystal; keeps the RF bursts clean. Not dead config.
- **Frequency is 869.525 MHz**, not 433 MHz. The CC1101 module must be the 868 MHz / 26 MHz-crystal variant.
- **The symbol period is 208.647 µs, stored as 209** — not 200. Measured frame-start to frame-start: 272910 samples across exactly 654 ticks (417.294 samples/tick), with the six intermediate estimates agreeing to within 0.02%. Measure it this way rather than across the preamble or across the whole message — both endpoints are then rising edges at the same structural position, so rise-time bias cancels, and there is no ambiguity about whether the truncated final tick counts. Do not trust URH's *Autodetect parameters* here — it reports 400 samples/symbol, wrong by 9%, because it fits a symbol length rather than measuring one.
- **`TRANSMIT_REPEAT` controls beep length.** The collar beeps as long as it keeps receiving; the repeat count is the duration knob, not a reliability-only retry. It counts **bursts**, not frames.

## Timing-critical section

The transmit loop runs with the FreeRTOS scheduler suspended, for a whole burst at a time. Inside that window: no `delay()`, no logging, no allocation, no async calls, nothing that can yield.

**The task watchdog is a live constraint, not a theoretical one.** The idle task cannot run under `vTaskSuspendAll()`, and on the ESPHome path a 159 ms burst followed by only a 5 ms gap reset the device with `ESP_RST_TASK_WDT` (confirmed 2026-08-11). The fix that worked was calling `esphome::App.feed_wdt()` in each gap — Wi-Fi and lwIP sit far above idle in priority and can consume the entire window servicing the backlog that accumulated during the burst, so relying on idle being scheduled is not safe. Lengthening the gap also works but is not free: it chops the beep audibly.

If `FRAMES_PER_BURST` or `TRANSMIT_REPEAT` grow substantially, feed the TWDT more often or raise its limit rather than letting it panic-reboot. `d-control-400.yaml` logs `esp_reset_reason()` on boot and on every API client connect, which is how the watchdog was distinguished from a brownout — the two look identical from the outside and have opposite fixes.

### The two paths have different watchdog exposure

The standalone build cannot trip the task watchdog at all, which is why it can
hold `vTaskSuspendAll()` across the *whole* ~2.96 s sequence where the ESPHome
path must release it every burst. In
`~/.platformio/packages/framework-arduinoespressif32-libs/esp32c3/sdkconfig` the
TWDT is enabled and set to panic at 5 s, but
`# CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0 is not set` — **the idle task is
never subscribed**. Nothing feeds that watchdog and nothing trips it. The exact
mechanism that reset the ESPHome device (idle can't run while suspended, idle
feeds the TWDT, so the TWDT starves) has no counterpart here.

So `WATCHDOG_BUDGET_US` in `main.cpp` is defensive, not load-bearing. Keep it —
it costs nothing and it stops a calibration command from setting up a
multi-second blocking transmit — but do not read a passing budget check as the
reason the standalone path survives.

The interrupt watchdog is not a concern on either path despite its 300 ms
timeout being shorter than a 159 ms burst plus overhead:
`vTaskSuspendAll()` defers context switches but does **not** disable
interrupts, so the FreeRTOS tick ISR keeps running. `micros()` keeps working
across a suspend for the same class of reason — it reads the systimer, not the
FreeRTOS tick count.

In `esphome/cc1101.h`, the `#undef`/`#define` block that routes `delay`/`delayMicroseconds`/`millis`/`micros`/`yield` through `esphome::` must stay at the top of the file, before the RadioLib include, or the Arduino SDK macros win and timing breaks.

## Safety gate

`cc1101_ctrl::is_ready()` (ESPHome) / the init check in `setup()` (standalone) guards every transmission path. If the CC1101 doesn't answer on SPI at boot, transmission is a no-op — this prevents SPI crashes on absent hardware. Keep new trigger paths behind that flag.

Wi-Fi `output_power: 8.5dBm` in the YAML is a hardware fix for LDO brownout on the C3 Mini regulator. Do not raise it.

## LED status convention

Standalone: green 0.5 s at boot = radio OK · **green pulse 80 ms / 5 s = alive heartbeat** · solid red = init failed (halts) · blue = transmitting · off = no power or crashed.
ESPHome: red = booting or init failed · green flash = Wi-Fi + radio ready · **green pulse 80 ms / 5 s = alive, Wi-Fi up** · **amber pulse 80 ms / 5 s = alive, Wi-Fi down** · blue = transmitting.

The heartbeat is what makes idle distinguishable from dead, and it is gated on
`is_ready()` so a board that sits solid red and never pulses has exactly one
problem (radio init) rather than two.

Two things about it are load-bearing, not decoration:

- **The ESPHome colour encodes Wi-Fi state because nothing else can.** A device
  with a dropped Wi-Fi link is alive and transmits fine but is invisible to Home
  Assistant *and* to `esphome logs`, so the LED is the only witness. A plain
  green pulse there would be a false all-clear in the one failure mode where you
  walk over and look at the board.
- **The ESPHome heartbeat guards `light.turn_off` as well as the pulse start.**
  It is an async automation, so a trigger arriving mid-pulse sets the LED blue
  and an unconditional `turn_off` would then blank it for the whole ~3 s
  transmission. Both ends need the `script.is_running: transmit_beep` check.
  `src/main.cpp` needs no such guard: `triggerTransmit()` blocks `loop()`, so the
  heartbeat and the timing-critical section are mutually exclusive by
  construction and `strip.show()` can never land inside `transmitSequence()`.

## Scope

Beep only. Shock is deliberately out of scope. The captured signal is specific to one physical remote (each likely embeds a unique ID) — the committed payload is a structural template, not a universal key.
