# Fan-RPM-Battery-Logger

ESP32-C3 Hall effect sensor RPM logger and battery monitor with web dashboard.

## Summary

Fan-RPM-Battery-Logger is an embedded telemetry system designed for monitoring fan rotation speed and battery discharge rates. Powered by an ESP32-C3 microcontroller, it uses an A3144/HW-477 Hall effect sensor to calculate precise RPM and total rotation count, paired with an ADC voltage divider to track battery drain over time. It serves an interactive real-time web dashboard over Wi-Fi.

## Features

- High-precision interrupt-driven RPM calculation using Hall effect magnetic pulse detection.
- Battery voltage monitoring with dual 10k resistor divider and ADC calibration.
- Flash wear-leveling storage using ESP32 Preferences and LittleFS for persistent lifetime stats (total revolutions, runtime hours, battery cycles).
- Embedded responsive web dashboard accessible over local Wi-Fi or mDNS (`http://esp32-hall.local`).
- REST endpoints and WebSocket/AJAX polling for live graphs of RPM, voltage, and system RSSI.

## Hardware Requirements

- Microcontroller: ESP32-C3 SuperMini development board
- Hall Effect Sensor: A3144 or HW-477 digital Hall sensor module
- Magnet: Small neodymium magnet attached to the rotating fan blade or hub
- Voltage Divider: Two 10k ohm 1% metal film resistors for battery voltage scaling
- Indicator: Onboard status LED

## Pinout Table

| Component | ESP32-C3 Pin | Function |
| --- | --- | --- |
| Battery Voltage Divider | GPIO 0 | Analog battery input (ADC1_CH0) |
| Hall Effect Sensor OUT | GPIO 4 | Digital interrupt pulse input |
| Onboard Status LED | GPIO 8 | Activity status indicator (Active LOW) |

## Secrets Configuration

1. In the `src/` folder, copy `secrets.example.h` to `secrets.h`.
2. Configure your Wi-Fi SSID, network password, and static IP address inside `secrets.h`.
3. `secrets.h` is excluded via `.gitignore` to keep credentials private.
4. If `secrets.h` is omitted, `secrets.example.h` is used with default placeholder values.

## Build and Flash Steps

1. Open a terminal in the repository directory.
2. Build the firmware binary using PlatformIO:
   ```bash
   pio run
   ```
3. Connect your ESP32-C3 board via USB and flash:
   ```bash
   pio run --target upload
   ```
4. Open the assigned IP address or `http://esp32-hall.local` in your browser to view the live dashboard.
