# ESPHome / Home Assistant path

The Home Assistant half of the project. It exposes the collar's beep as a template switch, keeps the BOOT button working as a manual trigger, and shares its entire radio path with the standalone PlatformIO firmware in the repository root.

Start at the [root README](../README.md) for the hardware, the wiring, the PCB and the account of how the signal was measured and decoded. This file covers only what is specific to running the device under ESPHome.

---

## What is shared, and what is not

Both firmware paths include the same headers from `../include/`, so a change to RF behaviour lands once rather than twice:

```text
d-control-400-remote/
├── include/
│   ├── pinout.h          # Pin map
│   ├── signal.h          # RF parameters + captured payload — ENCRYPTED, supply your own
│   ├── rmt_beep.h        # Transmission core — packs the frame, drives the RMT channel
│   ├── cc1101_config.h   # The six RadioLib calls that set the radio's RF personality
│   ├── led_policy.h      # Status LED palette, as levels emitted by the WS2812
│   └── reset_reason.h    # esp_reset_reason() as a printable string
└── esphome/
    ├── d-control-400.yaml  # This configuration
    ├── cc1101.h            # cc1101_ctrl — radio init, and the two halves of a transmission
    └── secrets.yaml        # Wi-Fi credentials and API key — create this, it is gitignored
```

What the two paths still own separately is the trigger surface, the LED mechanism, and how each waits for the hardware to report it has finished. `led_policy.h` is the one shared header this path does *not* include — ESPHome's YAML cannot include a C header, so the YAML repeats those numbers as literals and cites the header. The reasoning is in `led_policy.h` itself.

---

## Setup

### 1. Create `secrets.yaml`

All three keys are required; `esphome config` fails without any one of them.

```yaml
wifi_ssid: "Your_SSID"
wifi_password: "Your_Password"
api_encryption_key: "base64-key-here"     # 32 random bytes, base64 — Home Assistant shows it when adding the device
```

### 2. Supply your own `include/signal.h`

The committed copy is encrypted and is not valid C, so a fresh clone will not compile. Write your own from the template in the [root README](../README.md#-providing-your-own-signal).

### 3. Build and flash

```bash
esphome config d-control-400.yaml                              # validate, no hardware needed
esphome run d-control-400.yaml                                 # compile + flash over USB
esphome run d-control-400.yaml --device d-control-400.local    # OTA
esphome logs d-control-400.yaml
```

---

## Status LED

| Indication | Meaning |
| :--- | :--- |
| 🔴 Solid red | Booting, or CC1101 initialisation failed |
| 🟢 Green flash at boot | Wi-Fi connected and radio ready |
| 🟢 Green pulse, 150 ms every 5 s | Alive, radio OK, Wi-Fi up |
| 🟠 Amber pulse, 150 ms every 5 s | Alive, radio OK, **Wi-Fi down** |
| 🔵 Solid blue | Transmitting (~3 s); the heartbeat is suppressed |

The amber state is the one worth knowing about. A device whose Wi-Fi has dropped is alive and transmits fine from the button, but it is invisible to Home Assistant *and* to `esphome logs` — so the LED is the only witness to that failure, and a plain green pulse there would be a false all-clear in exactly the case where you walk over and look at the board.

The heartbeat carries a radio-ready condition, so a board sitting solid red and never pulsing has exactly one problem (radio init) rather than two.

> The LED is `internal: true`. Remove that to expose it to Home Assistant.

---

## Implementation notes

**The waveform is clocked by hardware, not by the CPU.** `cc1101_ctrl::begin_transmission()` puts the radio into asynchronous direct mode and hands the frame to the ESP32-C3's RMT peripheral, then returns — in about 5 ms, most of it the synthesiser settle. The RMT clocks the whole beep out of its own memory and raises an interrupt at the end. There is no timing-critical section, no `vTaskSuspendAll()`, and no watchdog exposure; an earlier bit-banged version had all three, and reset the device with `ESP_RST_TASK_WDT`. `end_transmission()` returns the radio to standby afterwards and runs in task context, because it is SPI work and cannot happen inside the completion interrupt.

**Retriggering is ignore-while-busy.** A press or a switch toggle arriving mid-beep is dropped, not queued and not restarted: the device emits one precise beep per trigger and deliberately does not reproduce the original remote's press-and-hold behaviour. The script is `mode: single` and `rmt_beep::start()` refuses independently, so the Home Assistant switch and the physical button cannot race each other.

**`rmt_symbols: 48` on the LED is mandatory.** ESPHome defaults it to 96 on the C3, which is two of the RMT's 48-word memory blocks, and a two-block channel claims its neighbour's. The C3 has exactly two TX-capable channels — the LED holds one and the radio the other — so at the default the LED consumes both and the radio has nowhere to go.

**`gamma_correct: 1.0` on the LED is deliberate**, and every brightness percentage in the YAML depends on it. ESPHome's default of 2.8 applies brightness linearly to the 8-bit channel before the gamma lookup, which crushes the bottom of the range: the Wi-Fi-down pulse emitted `(1, 0, 0)` — pure red at one count out of 255, green quantised away — so amber was never actually on screen. The curve exists to smooth a dimming sweep; this LED shows three fixed colours and never sweeps.

**Wi-Fi power is capped at 8.5 dBm.** That is a hardware fix for LDO brownout on the C3 Mini's regulator, not a tuning preference. Do not raise it.

**RadioLib is pinned exactly**, matching `platformio.ini`. It sits directly in the transmit path, so a silent minor bump is a silent change to RF behaviour — and the two paths are only comparable if they build against the same version.

**Transmission is gated on `cc1101_ctrl::is_ready()`.** If the CC1101 does not answer on SPI at boot, the flag stays false and every trigger silently no-ops, which is what prevents an SPI crash on absent hardware. Keep any new trigger path behind it.

⚠️ Triggering an animal's collar from an automation without a physical confirmation step is a bad idea. The switch exists so that a person can press it from a phone.
