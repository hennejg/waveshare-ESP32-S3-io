# Waveshare ESP32-S3-POE-ETH-8DI-8DO Firmware

ESP-IDF v6 firmware for the [Waveshare ESP32-S3-POE-ETH-8DI-8DO](https://www.waveshare.com/wiki/ESP32-S3-POE-ETH-8DI-8DO) industrial I/O expansion board.

## Hardware

|                     |                                                                                  |
|---------------------|----------------------------------------------------------------------------------|
| **MCU**             | ESP32-S3-WROOM-1U-N16R8 (dual-core Xtensa LX7, 240 MHz, 16 MB Flash, 8 MB PSRAM) |
| **Ethernet**        | W5500 SPI (10/100 Mbit)                                                          |
| **Digital inputs**  | 8 × optocoupler-isolated (DI1–DI8, GPIO4–GPIO11)                                 |
| **Digital outputs** | 8 × optocoupler-isolated Darlington (DO1–DO8, via TCA9554 I²C expander)          |
| **CAN bus**         | ESP32-S3 TWAI peripheral (GPIO2 TX, GPIO3 RX)                                    |
| **RS-485**          | Half-duplex UART (GPIO17 TX, GPIO18 RX)                                          |
| **LED**             | WS2812 RGB (GPIO38)                                                              |
| **Buzzer**          | Piezo, LEDC PWM (GPIO46)                                                         |
| **Power**           | 7–36 V DC or USB-C                                                               |

---

## Features

### Connectivity

- **WiFi provisioning** — on first boot the device opens a `Waveshare-Setup` access point with a captive portal to enter WiFi credentials. Hold the BOOT button (GPIO0) for ≥ 5 s to clear credentials and re-enter provisioning mode.
- **Ethernet** — W5500 SPI Ethernet runs in parallel with WiFi; whichever interface gets an IP first starts services.
- **DHCP hostname** — device registers under its configured name (default `Waveshare-ESP32`).

### Web Configuration UI

Served at `http://<device-ip>/` on port 80. Settings include:

| Section         | What you can configure                               |
|-----------------|------------------------------------------------------|
| Device          | Device name (DHCP hostname), LED mode                |
| MQTT Broker     | Broker URL, username/password, topic prefix          |
| Digital Inputs  | Per-input name and invert flag                       |
| Digital Outputs | Per-output name and invert flag                      |
| CAN Bus         | Mode (Off / Basic / NMEA2000), address, bit rate     |
| Modbus RTU      | Enable/disable, slave address, baudrate              |

The UI also provides **Save**, **Reboot**, and **Factory Reset** buttons.

#### REST API

`GET /api/config` and `POST /api/config` — JSON, same fields as UI.  
`POST /api/reboot` — restart immediately.  
`POST /api/factory-reset` — erase all NVS settings and restart.

### MQTT

Full details in [`docs/mqtt.md`](docs/mqtt.md).

| Resource               | Topics                               |
|------------------------|--------------------------------------|
| Digital inputs (read)  | `<prefix>/input/<name-or-1..8>`      |
| Digital outputs (set)  | `<prefix>/output/<name-or-1..8>/set` |
| Digital outputs (bulk) | `<prefix>/output/set`                |
| LED                    | `<prefix>/led/set`                   |
| Buzzer                 | `<prefix>/buzzer/beep`               |

- Input/output topics use a configured channel name, or the index number (1–8) if no name is set.
- Output commands accept `true`/`false`/`1`/`0`/`on`/`off`/`high`/`low`/`toggle` (case-insensitive).
- The LED accepts `#RRGGBB`, a timed single step `{"color":"#RRGGBB","duration":ms}`, or a multi-step sequence array.
- The buzzer accepts a single `{"freq":Hz,"duration":ms}` or a sequence array; omit `freq` for silent pauses.

### CAN Bus

Full details in [`docs/can.md`](docs/can.md).

Three selectable modes:

| Mode | Frame type | Bit rate | Description |
|------|-----------|----------|-------------|
| **Off** | — | — | Disabled |
| **Basic** | 11-bit standard | Configurable | Simple custom frame map |
| **NMEA2000** | 29-bit extended | 250 kbit/s | Standard marine network |

**Basic mode** — a compact 6-frame protocol covering DI state, DO set/echo (WRITE/SET/CLEAR/TOGGLE opcodes + bitmask), LED colour, buzzer, and a read-request trigger.

**NMEA2000 mode** implements:
- ISO address claiming with automatic conflict resolution
- PGN 126993 Heartbeat
- PGN 127501 Binary Switch Bank Status (DI on bank 0, DO on bank 1)
- PGN 127502 Switch Bank Control (DO commands)
- PGN 126720 Manufacturer Proprietary fast-packet (LED + buzzer)

### Modbus RTU

Full details in [`docs/modbus.md`](docs/modbus.md).

RS-485, configurable baud rate and slave address. Register map:

| Register    | Type                  | Description                           |
|-------------|-----------------------|---------------------------------------|
| 1–8         | Coil (RW)             | DO1–DO8                               |
| 10001–10008 | Discrete input (RO)   | DI1–DI8                               |
| 40001       | Holding register (RW) | LED colour (RGB252, 16-bit)           |
| 40002       | Holding register (RW) | Buzzer: write frequency → 200 ms beep |

### LED Modes

The WS2812 LED has two operating modes (configurable in the web UI):

**IO device mode** — controlled via MQTT `led/set`, Modbus HR 40001, and CAN base+3 / NMEA2000 PGN 126720.

**Status feedback mode** — automatic connectivity indicator:

| LED state                | Meaning                    |
|--------------------------|----------------------------|
| 30% yellow               | Booted, no network yet     |
| 30% green                | Network (WiFi or Ethernet) |
| 30% purple               | MQTT broker connected      |
| 100% red flash (100 ms)  | MQTT message received      |
| 100% blue flash (100 ms) | MQTT message published     |

In Status mode all LED commands from MQTT, Modbus, and CAN are ignored.

### Input / Output Details

- **Invert flag** — each DI and DO has a configurable invert flag (web UI). Applied consistently across MQTT, Modbus, and CAN.
- **Named channels** — each DI/DO can be given a name (up to 20 chars, no `/`) that replaces the index number in MQTT topics.
- **Debounce** — digital inputs have 10 ms software debounce.
- **Unified state** — a DO write via any interface (MQTT, Modbus, CAN) is reflected immediately on all others. MQTT confirmation is published, Modbus coil is updated, CAN echoes the new state.

---

## Build & Flash

### Prerequisites

- ESP-IDF v6.0.1 installed as a git submodule under `esp-idf/`

```sh
# First-time setup (after clone)
git submodule update --init --recursive --depth 1
./esp-idf/install.sh esp32s3
```

### Build

```sh
. ./esp-idf/export.sh
idf.py build
```

### Flash

```sh
# Full flash (app + SPIFFS web UI + partition table)
idf.py -p /dev/ttyUSBx flash

# App only (fastest — use when only firmware changed)
idf.py -p /dev/ttyUSBx app-flash

# SPIFFS only (use when www/ content changed)
idf.py -p /dev/ttyUSBx spiffs-flash
```

---

## Project Structure

```
├── main/
│   ├── main.c           Entry point, event/callback wiring
│   ├── app_config.c/h   NVS-backed configuration store
│   ├── app_mqtt.c/h     MQTT client wrapper
│   ├── can_server.c/h   CAN bus (Basic + NMEA2000 modes)
│   ├── di.c/h           Digital inputs (GPIO interrupts + debounce)
│   ├── dout.c/h         Digital outputs (TCA9554 I²C)
│   ├── eth.c/h          W5500 Ethernet (SPI)
│   ├── led.c/h          WS2812 LED (RMT) — IO and Status modes
│   ├── buzzer.c/h       Piezo buzzer (LEDC PWM) — single beep + sequences
│   ├── mb_server.c/h    Modbus RTU slave
│   ├── button.c/h       BOOT button long-press WiFi reset
│   └── web_server.c/h   HTTP config UI + REST API
├── www/
│   └── index.html       Single-page web UI (served from SPIFFS)
└── docs/
    ├── can.md           CAN bus API (Basic and NMEA2000 modes)
    ├── mqtt.md          MQTT API reference
    └── modbus.md        Modbus RTU register map
```

---

## Component Dependencies

| Component | Source | Purpose |
|-----------|--------|---------|
| `espressif/esp-modbus ^2.1.2` | Component Registry | Modbus RTU slave |
| `espressif/mqtt ^1.0.0` | Component Registry | MQTT client |
| `espressif/led_strip ^3.0.0` | Component Registry | WS2812 RMT driver |
| `espressif/cjson *` | Component Registry | JSON parsing |
| `espressif/w5500 ^1.0.1` | Component Registry | W5500 Ethernet PHY |
| `esp32-wifi-bootstrap` | Git submodule (`components/`) | WiFi captive-portal provisioning |
