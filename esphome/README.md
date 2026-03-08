# D-Control 400 - ESPHome Integration

This directory contains the ESPHome configuration and custom C++ headers required to integrate the Dogtrace D-Control 400 remote into Home Assistant.

## 🔗 Project Links
Link back to the root of the repository for hardware and SDR data
* Main Project: [Back to Root README](../README.md)

---

## 🛠 Setup & Installation

### 1. File Structure
This integration relies on shared headers in the parent directory

```text
d-control-400-remote/
├── include/               # Shared data (pinout.h, signal.h)
└── esphome/               # This directory
    ├── d-control-400.yaml # Main configuration
    ├── cc1101.h           # C++ Radio Wrapper
    └── secrets.yaml       # Private WiFi credentials (create this!)
```

### 2. Configure Secrets
Use secrets.yaml to prevent committing sensitive data to Git

```yaml
# secrets.yaml
wifi_ssid: "Your_SSID_Here"
wifi_password: "Your_Password_Here"
```

### 3. Deployment
Command to compile and flash the ESP32-C3

```bash
esphome run d-control-400.yaml
```

---

## 🚦 Status LED Guide
The onboard RGB LED provides immediate diagnostics:

* 🔴 Solid Red: Device booting or CC1101 initialization failed.
* 🟢 Green Flash: Successful WiFi connection and Radio ready.
* 🔵 Solid Blue: Actively transmitting the RF signal burst.

---

## ⚙️ Engineering Implementation

### ⚡ Power & Stability
WiFi Power is capped at 8.5dBm to prevent LDO brownouts on the C3 Mini.
The CC1101 is kept in standby until the moment of transmission to save current.

### ⏱ RF Timing Precision
The standard ESPHome remote_transmitter component is bypassed in favor of a custom C++ loop that directly controls the CC1101 and locks the CPU to guarantee microsecond-accurate signal pulses.

### 🛡 Hardware Safety
The trigger button is guarded by a 'cc1101_is_ready' flag.
If hardware isn't detected on boot, the button is disabled to prevent SPI crashes.

```cpp
// Safety check logic in cc1101.h
if (radio->begin() == RADIOLIB_ERR_NONE) {
    cc1101_is_ready = true; // Only allow transmission if hardware is found
}
```
