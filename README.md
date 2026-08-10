# ESP-IDF REST LED Driver

This project is a high-performance LED driver for ESP-IDF (specifically targeting ESP32-C6) that provides a RESTful API to control LED brightness and power status over WiFi. It is designed to drive high-density COB strips or standard LEDs with granular precision and configurable hardware parameters.

## Features

- **6-Channel Support**: Independent control for up to 6 LED strips (specifically optimized for HL-SAB-NPN-6).
- **REST API Control**: Simple HTTP interface to adjust brightness and toggle power per channel.
- **Web UI**: Modern dashboard to control all 6 channels visually (includes custom favicon).
- **Multi-Mode WiFi**: Supports secure provisioning via SoftAP if station connection fails.
- **Security**: WiFi passwords are masked in system logs and stored securely in NVS.
- **Granular PWM**: Configurable frequency (default 5 kHz) and resolution (up to 13-bit / 8,192 steps) for smooth, flicker-free dimming.
- **Flexible Hardware Configuration**:
    - Configurable GPIO pins for all 6 channels (defaults D0-D5).
    - Adjustable GPIO drive strength (up to 40mA) to sharpen signal edges.
    - Support for both Push-Pull and Open-Drain output modes.
    - **Signal Inversion**: Support for active-low drivers (e.g., NPN optocouplers) via hardware signal inversion.
- **Startup Diagnostics**: Logs the "Real URL" of the device's API endpoints upon successful WiFi connection.

## Hardware Requirements

- **ESP32-C6 Development Board** (specifically XIAO ESP32-C6 for 6-channel header layout).
- **LED/LED Strip**:
    - Supports up to 6 strips via the **HL-SAB-NPN-6** MOSFET board.
    - For standard LEDs: Push-Pull mode (default).
    - For high-power strips via optocouplers/MOSFETs: Open-Drain mode may be required (requires external pull-up).
- **Default Pins**: D0-D5 (GPIOs 0, 1, 2, 3, 4, 5).

## Getting Started

### 1. Configure the Project

Hardware settings can be adjusted via the build system:

```bash
idf.py menuconfig
```

Navigate to:
- **LED Configuration**:
    - **LED GPIO Pin**: Set the output pins for each channel.
    - **GPIO Output Mode**: Choose Push-Pull or Open-Drain.
    - **GPIO Drive Strength**: Adjust to reduce signal ramping.
    - **PWM Frequency (Hz)**: 5000 Hz is recommended for COB strips.
    - **PWM Resolution (bits)**: 13 bits provides high-precision dimming.
    - **Invert PWM Logic**: Enable if using an active-low driver or optocoupler (e.g., HL-SAB-NPN-6).

### 2. WiFi Provisioning

The project supports secure WiFi provisioning without hardcoding credentials:
1. **Initial Boot**: If no credentials are saved (or connection fails), the device starts in **Access Point (AP) Mode** on Channel 1.
2. **Connect to AP**: Connect your phone/computer to the WiFi network `myssid` (Password: `mypassword`).
3. **Configure**: Open `http://192.168.4.1/` in your browser.
4. **Save**: Enter your WiFi SSID and Password in the dashboard and click **Save & Restart**.

Credentials are stored securely in NVS. You can still set "fallback" credentials in `idf.py menuconfig` under `Example Connection Configuration`.

### 3. Build and Flash

```bash
idf.py build
idf.py flash monitor
```

## REST API Documentation

Once connected, the device will log its IP address and the specific URLs to use.

### Web UI

Navigate to `http://<device_ip>/` to access the graphical control interface.

### REST Endpoints
| :--- | :--- | :--- | :--- |
| `/api/brightness` | `POST` | `ch=0-5`, `value=0-100` | Set brightness percentage for a channel. |
| `/api/brightness` | `GET` | `ch=0-5` (optional) | Get brightness (single channel or all). |
| `/api/power` | `POST` | `ch=0-5`, `value=on\|off` | Toggle power for a channel. |
| `/api/power` | `GET` | `ch=0-5` (optional) | Get power status (single channel or all). |
| `/api/led_info` | `GET` | *None* | Get configured LED GPIO pins. |
| `/api/wifi` | `POST` | `ssid=xxx`, `password=yyy` | Save WiFi credentials and restart. |

### Examples

**Set brightness of Channel 2 to 60%:**
```http
POST http://<device_ip>/api/brightness?ch=2&value=60
```

**Turn Channel 0 off:**
```http
POST http://<device_ip>/api/power?ch=0&value=off
```

## Project Structure

- `main/led-driver.c`: Core logic for PWM, WiFi, and REST handlers.
- `main/Kconfig.projbuild`: Kconfig definitions for project settings.
- `led.http`: IntelliJ/CLion HTTP Client templates for testing the API.
