# ESP-IDF REST LED Driver

This project is a high-performance LED driver for ESP-IDF (specifically targeting ESP32-C6) that provides a RESTful API to control LED brightness and power status over WiFi. It is designed to drive high-density COB strips or standard LEDs with granular precision and configurable hardware parameters.

## Features

- **REST API Control**: Simple HTTP interface to adjust brightness and toggle power.
- **Granular PWM**: Configurable frequency (default 5 kHz) and resolution (up to 13-bit / 8,192 steps) for smooth, flicker-free dimming.
- **WiFi Connectivity**: Station mode support with automatic reconnection logic.
- **Flexible Hardware Configuration**:
    - Configurable GPIO pin (default D2/GPIO 2).
    - Adjustable GPIO drive strength (up to 40mA) to sharpen signal edges.
    - Support for both Push-Pull and Open-Drain output modes.
- **Startup Diagnostics**: Logs the "Real URL" of the device's API endpoints upon successful WiFi connection.

## Hardware Requirements

- **ESP32-C6 Development Board** (or compatible ESP32 variant).
- **LED/LED Strip**:
    - For standard LEDs: Push-Pull mode (default).
    - For high-power strips via optocouplers/MOSFETs: Open-Drain mode may be required (requires external pull-up).
- **Default Pin**: GPIO 2 (labeled D2 on many boards).

## Getting Started

### 1. Configure the Project

Before building, you must configure your WiFi credentials and LED settings:

```bash
idf.py menuconfig
```

Navigate to:
- **Example Connection Configuration**: Set your WiFi SSID and Password.
- **LED Configuration**:
    - **LED GPIO Pin**: Set the output pin.
    - **GPIO Output Mode**: Choose Push-Pull or Open-Drain.
    - **GPIO Drive Strength**: Adjust to reduce signal ramping.
    - **PWM Frequency (Hz)**: 5000 Hz is recommended for COB strips.
    - **PWM Resolution (bits)**: 13 bits provides high-precision dimming.

### 2. Build and Flash

```bash
idf.py build
idf.py flash monitor
```

## REST API Documentation

Once connected, the device will log its IP address and the specific URLs to use.

| Endpoint | Method | Parameter | Description |
| :--- | :--- | :--- | :--- |
| `/api/brightness` | `POST` | `value=0-100` | Set brightness percentage. |
| `/api/brightness` | `GET` | *None* | Get current brightness level. |
| `/api/power` | `POST` | `value=on\|off` | Toggle LED power. |
| `/api/power` | `GET` | *None* | Get current power status. |
| `/api/led_info` | `GET` | *None* | Get configured LED GPIO pin. |

### Examples

**Set brightness to 60%:**
```http
POST http://<device_ip>/api/brightness?value=60
```

**Turn LED off:**
```http
POST http://<device_ip>/api/power?value=off
```

## Project Structure

- `main/led-driver.c`: Core logic for PWM, WiFi, and REST handlers.
- `main/Kconfig.projbuild`: Kconfig definitions for project settings.
- `led.http`: IntelliJ/CLion HTTP Client templates for testing the API.
