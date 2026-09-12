# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Firmware for a Wemos LOLIN C3 Mini (ESP32-C3) + CC1101 868 MHz transceiver that replays a captured RF signal to trigger the beep function of a Dogtrace d-control 400 dog collar. Transmission is a raw OOK pulse-train replay, clocked onto the CC1101's GDO0 pin by the ESP32-C3's RMT peripheral in asynchronous direct mode — no packet engine, no sync word, no CRC.

The frame **is** now structurally decoded (2026-09-06, see "Frame structure" below), but nothing in the transmit path uses that. The firmware still emits a stored payload verbatim. The decode is analysis; it changed no code.

Repo also carries hardware artifacts: `pcb/gerber.zip` (fab-ready), `enclosure/` (Fusion 360 + STL/3MF), `doc/` (media). Those are binary deliverables, not build inputs.

## Two firmware paths, one shared core

Two runtimes, but since the RMT migration the radio logic itself is **not** duplicated:

| Path | Entry point | Runtime |
| :--- | :--- | :--- |
| Standalone PlatformIO | `src/main.cpp` | Arduino, button-only, no network |
| ESPHome / Home Assistant | `esphome/d-control-400.yaml` + `esphome/cc1101.h` | ESPHome, exposes a template switch |

Both include `include/pinout.h`, `include/signal.h` and `include/rmt_beep.h`. See `esphome/CLAUDE.md` for ESPHome-specific detail.

`include/rmt_beep.h` is the transmission core and is shared verbatim: it packs the payload into RMT words, creates the channel, starts a looped transmission and reports when it ends. It talks only to the ESP-IDF RMT driver and `signal.h` — no Arduino, no ESPHome, no logging, failures reported by return value — so each path can log in its own idiom. **A change to RF behaviour belongs there, once, not in both paths.**

What the paths still own separately is everything around a transmission: RadioLib setup, LED policy, the trigger surface (button and serial vs. button and a Home Assistant switch), and putting the radio back to standby when the hardware reports it is done. The bit-banged era's intentional differences — who suspends the scheduler and for how long, who feeds the watchdog, which timing engine is used — are all gone, because none of them have anything left to describe. Both paths now pass `RADIOLIB_NC` as RadioLib's GPIO argument: GDO0 belongs to the RMT and RadioLib must never touch it.

Both trigger paths are **ignore-while-busy**. A press arriving during a beep is dropped, not queued or restarted: the device emits one precise beep per press and deliberately does not reproduce the original remote's press-and-hold behaviour. `rmt_beep::start()` enforces this on its own, so the ESPHome script's `mode: single` and the standalone `txActive` flag are belt-and-braces rather than the only guard.

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

`signal.h` must define: `CARRIER_FREQUENCY`, `OUTPUT_POWER`, `BIT_RATE`, `RX_BANDWIDTH`, `SYMBOL_TICKS`, `RMT_RESOLUTION_HZ`, `BEEP_DURATION_MS`, and `SIGNAL_BEEP_TICKS`. All are used by both paths.

`SIGNAL_BEEP_TICKS` is a brace-enclosed initializer of signed **run lengths in symbol periods** — not absolute microseconds. **Positive = RF ON (GDO0 HIGH), negative = RF OFF**; the duration of element `i` is `SYMBOL_TICKS * abs(ticks[i])` RMT channel ticks. `rmt_beep.h` consumes it as `constexpr int32_t[]` and derives length via `sizeof`.

This encoding is deliberate: the payload's elements are all ±1 or ±2, so the entire 88-element frame is parameterised by the single scalar `SYMBOL_TICKS`. That is what makes the symbol-period sweep (serial `k` command) possible without a re-capture. Do not "simplify" this back to absolute durations — it would destroy the ability to calibrate.

The scalar changed identity in the RMT migration and got more accurate doing so. `BASE_TICK_US` was 209, an integer number of microseconds and the finest step the bit-banged engine had, 1692 ppm off the measured 208.647 µs. `SYMBOL_TICKS` is 78 channel ticks of exactly 2.675 µs, which is 208.65 µs — **+14.38 ppm**. Moving the scalar into the hardware's own units is what bought the ~100× improvement, and it required no change to the payload.

`RMT_RESOLUTION_HZ` is a *request*, not a rate: the driver picks the nearest integer channel divider (214 off an 80 MHz group clock) and warns about "channel resolution loss" when the division is not exact. That warning is expected and cosmetic.

Two properties of `SIGNAL_BEEP_TICKS` are enforced by `static_assert` in `rmt_beep.h`, because the peripheral cannot loop a frame that violates either:
- **Even run count.** Each RMT word holds two entries and the hardware replays each entry's stored level bit verbatim, so an odd-length frame would put two same-level runs against each other at the loop seam and merge them.
- **At most 94 runs** (47 words), leaving room for the one word the driver appends as the end marker inside a 48-word channel block. Loop mode cannot refill the block as it plays, so the whole frame must be resident.

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

## Contiguity — the rule that actually decides whether it works

The single most important property of this firmware, and the least obvious.

**Frames must be emitted back-to-back with no silence between them.** The real remote never inserts a gap mid-transmission: a tap is 7 contiguous frames (159.3 ms), a 4.14 s hold is 180 contiguous frames, and in neither case is there an OFF run longer than 2 ticks anywhere inside. Verified against three captures, 13 presses, 2026-08-11.

A build that sent **one** frame followed by a 5 ms gap produced **zero** beeps, while the older, badly mistimed payload that happened to contain 2.5 frames per burst worked ~70% of the time. The likely mechanisms are consecutive-frame validation in the collar's decoder (it wants the next frame immediately, and a gap expires that timer) and AGC settling (5 ms of no carrier lets the receiver's gain walk off and corrupt the next frame's preamble). Either way, contiguity is the requirement. The historic 70% reliability was never a timing problem — it was burst structure, and the old payload only ever worked by accident.

Under RMT this stops being a knob at all. The frame sits in the channel's own memory and the hardware loops it, so a beep is one contiguous run of frames by construction. The three layered constants the bit-banged firmware needed (`FRAMES_PER_BURST`, `TRANSMIT_GAP_US`, `TRANSMIT_REPEAT`) collapse into `BEEP_DURATION_MS`, which the firmware rounds to a whole number of frames — 132 frames of 22.743 ms at 3000 ms — and loads into the RMT loop counter. All three existed only because the CPU cannot hold `vTaskSuspendAll()` indefinitely; `TRANSMIT_GAP_US` in particular was a ceiling on damage, not a value with a right answer.

`RMT_TX_LOOP_NUM_CHn` is ten bits, so one batch covers up to 1023 frames (~23 s). Past that the driver chains a second batch by stopping and restarting the channel, and that seam has never been measured — `rmt_beep::set_duration_ms()` refuses a duration that would need one.

Two facts about the loop seam are settled and worth not rediscovering:
- **The frame survives looping because its run count is even.** 88 runs means it starts ON and ends OFF, so each repeat begins with a real OFF→ON edge, and that edge is what gives the last run its duration. On the final frame that run is lost to silence — a transmission ending on a LOW run always loses it, since silence never supplies the closing rising edge — which costs nothing here and is exactly what the real remote does on button release.
- **What the peripheral itself does at the seam is unmeasured.** The TRM says only that the transmitter "starts transmitting the first data again". A decode cannot detect it (the decoder rounds anything under half a symbol away); a frame-start-to-frame-start period comparison across a seam-free span versus a seam-crossing one can.

## TODO

### 1. Move transmission from the CPU to the RMT peripheral — code written 2026-09-12, NOT yet validated on hardware

Both paths build. Nothing has been on the air. Until a board has been flashed and the
collar has beeped, treat this as untested code, not a working feature.

The first thing to check on hardware is not RF at all: a boot where the **LED still
lights and `rmt_beep::init()` returns `ESP_OK`** proves the group clock source, the
group prescale and the channel count all agree in practice — the whole stack of
assumptions behind the migration, in one observation. Only then does the collar
matter.

What remains after that is the seam measurement described in "Contiguity" above.

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
- **The symbol period is 208.647 µs, realised as 78 channel ticks of 2.675 µs** — not 200, and no longer the rounded 209 the bit-banged engine was stuck with. Measured frame-start to frame-start: 272910 samples across exactly 654 ticks (417.294 samples/tick), with the six intermediate estimates agreeing to within 0.02%. Measure it this way rather than across the preamble or across the whole message — both endpoints are then rising edges at the same structural position, so rise-time bias cancels, and there is no ambiguity about whether the truncated final tick counts. Do not trust URH's *Autodetect parameters* here — it reports 400 samples/symbol, wrong by 9%, because it fits a symbol length rather than measuring one.
- **`BEEP_DURATION_MS` controls beep length.** The collar beeps as long as it keeps receiving; this is the duration knob, not a reliability-only retry. The firmware converts it to a frame count for the RMT loop counter.

## The transmit path is no longer timing-critical

Nothing in the firmware generates the waveform any more. `rmt_beep::start()` fills a
transmit descriptor, calls `rmt_transmit()` and returns; the RMT peripheral clocks
the whole beep out of its own memory and raises an interrupt at the end. There is no
suspended region, no deadline loop, and nothing that a preemption can stretch.

What replaced the old constraints:

- **No scheduler suspension, so no watchdog exposure.** The task watchdog was a live
  failure while the frame was bit-banged — a 159 ms burst plus a 5 ms gap reset the
  ESPHome device with `ESP_RST_TASK_WDT` (confirmed 2026-08-11), because the idle
  task cannot run under `vTaskSuspendAll()` and Wi-Fi and lwIP outrank it in the gap.
  Neither path suspends anything now, so a `TASK_WDT` reset would mean something new
  rather than something known. `d-control-400.yaml` still logs `esp_reset_reason()`
  on boot and on every API client connect, which is how a watchdog reset was
  distinguished from a brownout — the two look identical from the outside and have
  opposite fixes.
- **Completion arrives in an ISR, so both paths need a poll.** The C3 has no loop
  auto-stop, so the driver stops the channel from its own interrupt handler and
  `rmt_beep::on_done()` clears the busy flag there. Putting the radio back to standby
  is SPI work and must happen in task context: `pollTransmit()` in `loop()` on the
  standalone path, `wait_until` plus `cc1101_ctrl::end_transmission()` in the ESPHome
  script. Do not move that into the callback.
- **GDO0 belongs to the RMT.** Both paths pass `RADIOLIB_NC` as RadioLib's GPIO
  argument and neither calls `pinMode`/`digitalWrite` on the pin. The CC1101 side is
  configured over SPI by `transmitDirect()`, which needs no MCU pin. The resting
  level is forced from a register (`eot_level`, which the driver also writes to
  `RMT_IDLE_OUT_LV`) rather than taken from the end marker, which is what keeps the
  pin low — and therefore the PA gated off — even when the driver aborts the loop
  mid-frame.
- **The 5 ms synthesiser settle survives.** It is still needed between
  `transmitDirect()` and the first edge, and it is ordinary blocking code outside any
  critical section.

In `esphome/cc1101.h`, the `#undef`/`#define` block that routes
`delay`/`delayMicroseconds`/`millis`/`micros`/`yield` through `esphome::` must stay at
the top of the file, before the RadioLib include, or the Arduino SDK macros win.
`rmt_beep.h` deliberately uses none of them.

### The RMT group is shared with the status LED

This is the constraint that replaces the watchdog as the thing most likely to bite.

The C3 has one RMT group, 192 words of RAM partitioned 48 per channel, and exactly
two TX-capable channels. The status LED holds one and the radio the other, with
nothing spare. Both the clock source and the group prescale belong to the **group**,
so the first channel created fixes them for every later one:

- The LED asks for `RMT_CLK_SRC_DEFAULT`, which is APB on the C3, so `rmt_beep.h`
  asks for `RMT_CLK_SRC_APB` explicitly rather than relying on init order. A mismatch
  is a hard `ESP_ERR_INVALID_ARG` ("group clock conflict", `rmt_common.c`), not a
  fallback. Because both ask for APB, either init order works and both firmware paths
  emit a bit-identical waveform.
- Under ESPHome the LED **must** carry `rmt_symbols: 48`. ESPHome defaults it to 96
  on the C3, which is two memory blocks, and a two-block channel claims its
  neighbour's — leaving the radio nowhere to go.

Sweeping the symbol period therefore means changing `SYMBOL_TICKS` (the period fields
in the frame buffer), never the channel resolution. One `SYMBOL_TICKS` step is 1/78 =
1.28% of the symbol period and the range is bounded only by the RMT's 15-bit period
field. The channel divider would give finer steps (1/214 = 0.47%) but it is 8 bits
and already at 214, so it can only lengthen the period by ~19.6% before saturating.

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
  `src/main.cpp` now needs the same guard, and that is a change the RMT migration
  forced. The old `triggerTransmit()` blocked `loop()`, so the heartbeat and the
  transmission were mutually exclusive by construction. The beep now runs in
  hardware while `loop()` keeps turning, so `pollHeartbeat()` returns early while
  `txActive` — guarding the start of a pulse alone would still let a pulse that
  began just before the trigger blank the LED for the whole beep.

## Scope

Beep only. Shock is deliberately out of scope, and since 2026-09-06 that is a
policy rather than a limitation: the frame is understood well enough to construct a
shock frame, and the repo deliberately does not carry what would be needed to. The
committed payload is specific to one physical remote and is a structural template,
not a universal key — though "each handset embeds a unique ID" remains an untested
assumption, not a finding. See "Frame structure".
