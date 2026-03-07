# Dogtrace d-control 400 remote clone (ESP32-C3 + CC1101)

This project replicates the control signal for a Dogtrace d-control 400 electric dog collar using a Wemos LOLIN C3 Mini (ESP32-C3) and a CC1101 transceiver module.

⚠️ IMPORTANT PROJECT SCOPE & DISCLAIMERS:
* Unknown Protocol: The underlying communication protocol is not publicly available, so the precise byte frame to be used, the precise RF parameters or checksum logic used by Dogtrace is unknown. This project does not attempt to reverse engineer the protocol or implement a true "clone" of the remote.
* Raw Replay: The signal in this code is a raw, fixed-code payload captured using an SDR (Software Defined Radio), cleaned up, fine-tuned and repeated. The RF parameters and signal timings used here are fine-tuned to work, but may not be the exact parameters used by the original remote.
* Device Specific: The signal included in this code is specific to the remote used for the SDR capture. It is not a universal signal for all Dogtrace d-control 400 remotes, as each remote likely has a unique identifier embedded in the signal to prevent cross-interference between devices. Therefore this code cannot be used out of the box for any other remote. If other remote is to be used, it must be first captured by the SDR and replaced in the code.
* Beep Only: This repository currently only contains the captured signal for the sound beep function. It could easily be extended to include the shock function by capturing that specific button press, but that is not a scope of this project at the moment.

---

## Hardware Requirements
* Microcontroller: Wemos LOLIN C3 Mini (ESP32-C3).
* Radio: CC1101 Transceiver Module (Must be the 868 MHz version, 26 MHz crystal).
* Antenna: 868 MHz tuned antenna (~8.2 cm quarter-wave).

---

## 🔌 Wiring Diagram

The project uses a custom SPI bus routed to specific pins on the ESP32-C3. The physical BOOT button on GPIO 9 is used to trigger the transmission.

```mermaid
graph LR
    subgraph Wemos LOLIN C3 Mini
        3V3[3.3V Power]
        GND[Ground]
        G0[GPIO 0]
        G4[GPIO 4]
        G5[GPIO 5]
        G3[GPIO 3]
        G1[GPIO 1]
    end

    subgraph CC1101 Module
        VCC_C[VCC]
        GND_C[GND]
        MOSI[MOSI]
        SCK[SCK]
        MISO[MISO]
        GDO0[GDO0]
        CSN[CSN]
    end

    3V3 ---|Power| VCC_C
    GND ---|Ground| GND_C
    G0 ---|SPI Data In| MOSI
    G1 ---|Chip Select| CSN
    G3 ---|Async TX Bit-Bang| GDO0
    G4 ---|SPI Clock| SCK
    G5 ---|SPI Data Out| MISO
```

---

## 🚀 Technical Specifics

### 1. 20x Bitrate Oversampling
The actual data rate of the 200 µs pulses is 5.0 kBaud. However, the CC1101's internal data rate register is deliberately cranked to 100.0 kbps.
* In Asynchronous mode, the CC1101 samples the GDO0 pin based on its internal clock. At 5 kbps, it only samples every 200 µs, causing massive timing jitter. Oversampling at 100 kbps forces the chip to sample the pin every 10 µs, eliminating jitter and forcing the frequency synthesizer to slew between frequencies instantly, creating crisp square waves.

### 2. Bandwidth Tuning
Receiver bandwidth (RxBandwidth) is set to 116.0 kHz even though the project only implements a transmitter. This ensures the internal clock tree and frequency synthesizer have enough "room" to instantly shift the 47.6 kHz deviation without clipping or rounding the signal edges.

### 3. FreeRTOS CPU Locking
The ESP32 is a dual-core chip running a real-time OS (FreeRTOS) that handles background tasks. Background interrupts may distort the precise 200 µs pulse timings.
* The Fix: The `vTaskSuspendAll()` function is used to lock the CPU for the entire duration of the signal transmission.
* Watchdog Bypass: Because the entire sequence takes less than the standard 5-second Task Watchdog Timer (TWDT) limit, the sequence completes and resumes normal OS operations without triggering a panic reboot. If longer sequences are needed, the TWDT limit can be configured or it can be fed within the locked section.

---

## 🛠️ Software Setup

This project is built using PlatformIO. The RadioLib library is used for CC1101 control, and Adafruit NeoPixel is used for the onboard RGB LED status indicator.

To upload the code run `pio run --target upload`.

---

## 🚥 Status Indication
* After the board is powered up, the onboard RGB LED will flash Green if the CC1101 initializes correctly over SPI. If it fails to initialize, the LED will turn and stay Red intead, indicating a wiring or hardware issue.
* Pressing the BOOT button on the C3 Mini triggers the signal transmission. During this transmission the LED will turn Blue, after the full seqeuence is transmitted the LED will turn off.

* 🟢 **Solid Green (0.5s):** Power on / CC1101 Radio initialized successfully.
* 🔴 **Solid Red:** Radio initialization failed (Check your SPI wiring).
* 🔵 **Solid Blue:** Actively transmitting the RF signal (CPU locked).
* ⚫ **LED Off:** Standby mode / Ready for input.

## 🎮 Usage
1. Power up the board and ensure the LED flashes Green, indicating successful CC1101 initialization
2. Press the BOOT button to transmit the signal. The LED will turn Blue during transmission and then turn off once complete.
