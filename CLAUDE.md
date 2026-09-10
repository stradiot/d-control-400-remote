# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Firmware for a Wemos LOLIN C3 Mini (ESP32-C3) + CC1101 868 MHz transceiver that replays a captured RF signal to trigger the beep function of a Dogtrace d-control 400 dog collar. Transmission is a raw OOK pulse-train replay, bit-banged onto the CC1101's GDO0 pin in asynchronous direct mode — no packet engine, no sync word, no CRC.

The frame **is** now structurally decoded (2026-09-06, see "Frame structure" below), but nothing in the transmit path uses that. The firmware still emits a stored payload verbatim. The decode is analysis; it changed no code.

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

`platformio.ini` deliberately sets **no** `upload_port` / `monitor_port`. The
`lolin_c3_mini` board definition declares hwids `303A:1001` (Espressif VID +
USB Serial/JTAG PID), so PlatformIO finds the board by VID/PID and the file
stays machine-independent. Override per invocation only when several Espressif
boards are attached:

```bash
pio run -t upload --upload-port /dev/cu.usbmodem101
```

Prefer `/dev/cu.*` over `/dev/tty.*` on macOS: `tty.*` is the callin device and
blocks on open until DCD asserts, which a USB CDC-ACM device typically never
does. Do not re-add hardcoded ports — that only ever meant editing this file on
every machine.

There are no tests. `test/`, `lib/`, and `include/README` are empty PlatformIO scaffolding placeholders.

## A fresh clone does not compile

`include/signal.h` is **SOPS + age encrypted** (see `.sops.yaml`) — the checked-in file is JSON ciphertext, not C. Building requires either decrypting it (`sops -d include/signal.h`) with the age key, or replacing it with your own captured signal using the template in the README's "Providing your own signal" section. `include/pinout.h` is plaintext and committed as-is.

`signal_captures.txt` is encrypted under the same `.sops.yaml` rules and to the same age recipient. It is the decoded frame worksheet, not a build input — nothing includes it.

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

### The pre-commit guard

`.githooks/pre-commit` refuses any commit that stages `include/signal.h` or
`signal_captures.txt` in plaintext, and warns (without blocking) when either is
sitting decrypted in the working tree. It checks the **staged blob**, not the file
on disk — a decrypted working copy with ciphertext staged is fine, and the reverse
is the accident being guarded against. Detection is a text test for sops's `"sops"`
key and `ENC[AES256_GCM` envelope, so it needs no age key and works on a machine
that cannot decrypt.

Enable it in a fresh clone:

```bash
git config core.hooksPath .githooks
```

**A local `core.hooksPath` replaces the global one rather than adding to it.** This
machine sets a global `core.hooksPath` in `~/.gitconfig` for a `prepare-commit-msg`
generator, which would silently stop running here. That is why
`.githooks/prepare-commit-msg` exists: it resolves the global hooks directory at run
time and execs its hook if there is one.

**Its re-entry guard is load-bearing and must not be removed.** A global hook may
chain the other way, into the repo's local hook, resolving it with `git rev-parse
--git-path hooks/prepare-commit-msg` — and that honours `core.hooksPath`, so it
resolves to the shim. Shim execs global, global calls local, local execs global. A
global hook's own "don't call myself" check compares against `$0` and cannot see a
two-hop cycle. Unguarded, this forks until the process table fills; it did, at
roughly 3000 processes in two minutes, before the guard existed. The
`GIT_HOOK_CHAIN_DCONTROL400` variable breaks the cycle on second entry and lets the
global hook proceed.

If you add another hook here, it needs the same shim and the same guard, or the
global copy stops firing.

`esphome/secrets.yaml` (Wi-Fi creds + `api_encryption_key`) is gitignored and must be created locally.

## Frame structure

Decoded 2026-09-06 from a differential campaign — beep and shock, channels A and
B, all 20 shock levels, 42 distinct frames pulled from raw IQ and cross-checked
against hand-cut URH transcriptions.

Every frame is **88 runs** of 1T or 2T, T = 208.647 µs, **run-length coded**. The
levels carry no information whatsoever: two adjacent runs at the same level would
merge into one, so the level sequence is fully determined by the first one and
everything is in the durations. That rules out NRZ, Manchester, PWM and PPM
outright, and `ticks = 88 + (number of 2T runs)` is an identity on every frame
measured.

Canonical cut: the 88 runs beginning at the **last 31 unit runs before a double**.
Do not anchor on the start of the visible alternating stretch at a frame boundary
— that is the preamble plus however many short runs trail the *previous* frame, so
its length is data-dependent.

Of the 88 runs, **68 are constant** across everything this handset transmits: both
functions, both channels, all 20 levels. The other 20 are the entire command
surface. `A` = short run, `B` = long run:

```
run     field
0-30    preamble          31 short runs
31-44   constant          handset-specific
45-46   channel           AB = channel A,  BA = channel B
47-66   constant          handset-specific
67-68   function          AB = beep,       BA = shock
69-70   constant          handset-specific
71-78   level             8 runs, MSB first, long = 1
79-86   redundancy        run 79+i against run 71+i:
                            i = 0..3   always complement
                            i = 4,5    complement = shock, copy = beep
                            i = 6,7    (complement, copy) = channel A
                                       (copy, complement) = channel B
87      constant          always short
```

The redundancy block is the part worth knowing about, and it is not a checksum. It
is the level field re-emitted through a mask: half its positions always complement
the original, and the rest complement or copy depending on the channel and the
function. So each command field is stated twice — once outright, once as a
perturbation of the redundancy — and a receiver validating the two halves against
each other reads the command out of the comparison itself.

Neither the channel nor the function field ever takes `AA` or `BB`. Two readings
fit and 42 frames cannot separate them, since each field has only two observations:
two-bit fields with two spare states (room for two more functions and two more
channels), or one-bit fields in a balanced 1-of-2 code where `AA`/`BB` are invalid.
A higher model in the d-control range, which has more channels, would settle it.

The level field takes 20 distinct values across the 20 dial positions, read MSB
first as an 8-bit number. That sequence is monotone, strongly non-linear, and has
no formula behind it. **What the value means is unverified.** Nothing captured
establishes whether it is a physical quantity such as a pulse width or an opaque
index, and therefore nothing establishes that the collar performs any second
mapping of its own. Do not write either as fact; settling it means instrumenting
the collar, not capturing more RF.

**What stays encrypted in `signal_captures.txt`:** the 68 constant runs and the
level-to-value table. The schema above is protocol structure, discoverable by
anyone with this model and an SDR. The constants are one handset's identity and are
the only thing that makes a frame *this* remote's.

Three things the captures cannot settle, in descending order of what they buy:

- **Are those 68 constant runs a per-handset identifier?** They are constant under
  every axis a single handset can vary, which is a weaker claim than "identifier".
  This is the assumption the Scope section has always rested on and it has never been
  tested. **It needs a second remote** — no capture of this one can settle it. The
  diff splits the block into what differs (identity) and what agrees (framing), with
  the confounder that two handsets may also differ by firmware revision, since remote
  and collar ship as a pair. A third handset disambiguates; so does the shape, an
  identifier being likely contiguous and a revision counter likely separate and small.
- **What is the transmitted level value?** Monotone and strongly non-linear across
  the 20 dial positions, nothing recovered from the numbers alone. Whether it is a
  physical quantity — pulse width being plausible for a switched source, which would
  make the table samples of a curve — or an opaque index is unverified, and so is
  anything that follows from it about what the collar does with the value. Settling
  it means instrumenting the collar's output, so it needs a scope and the hardware
  already here rather than a second remote: the cheapest of the three. The collar
  must not be on the dog during that work.
- **Is the two-collar limit real?** *Highly optional.* Nothing in the frame enforces
  it — the channel is just two more address runs, so pairing two collars to one
  channel should make both fire. Tests the system rather than the protocol, and
  nothing depends on the answer. Recorded as a clean prediction, not as work worth
  buying hardware for.

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

The frame fits, and the numbers are now read out of the TRM (ch. 33) rather than assumed: each 32-bit RAM word holds two 16-bit pulse codes of 1-bit level plus 15-bit period, so 88 runs = **44 words** against a 48-word channel block — 96 pulses of capacity, no memory reconfiguration needed. The C3 *does* support hardware TX looping (`RMT_LL_MAX_LOOP_COUNT_PER_BATCH` = 1023); what remains open is how to build a contiguous burst from it, and what happens at the seam between loop iterations and between batches.

### 2. Decode the protocol — done 2026-09-06

Closed. The differential campaign covered beep and shock across channels A and B
and all 20 shock levels; the layout is in "Frame structure" above. What is left is
not more capture of this handset — it is the two hardware questions listed there,
needing a second remote and a second collar.

The safety asymmetry that made the work sensitive still holds: understanding the
shock frames means being able to transmit them. The collar must not be on the dog
during any of it, and beep-only remains the scope of the shipped firmware.

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

Beep only. Shock is deliberately out of scope, and since 2026-09-06 that is a
policy rather than a limitation: the frame is understood well enough to construct a
shock frame, and the repo deliberately does not carry what would be needed to. The
committed payload is specific to one physical remote and is a structural template,
not a universal key — though "each handset embeds a unique ID" remains an untested
assumption, not a finding. See "Frame structure".
