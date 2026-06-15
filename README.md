# Waveshare ESP32-S3-POE-ETH-8DI-8DO Firmware

ESP-IDF v6 firmware for
the [Waveshare ESP32-S3-POE-ETH-8DI-8DO](https://www.waveshare.com/wiki/ESP32-S3-POE-ETH-8DI-8DO) industrial I/O
expansion board.

## Hardware

|                     |                                                                                  |
|---------------------|----------------------------------------------------------------------------------|
| **MCU**             | ESP32-S3-WROOM-1U-N16R8 (dual-core Xtensa LX7, 240 MHz, 16 MB Flash, 8 MB PSRAM) |
| **Ethernet**        | W5500 SPI (10/100 Mbit)                                                          |
| **Digital inputs**  | 8 × optocoupler-isolated (DI1–DI8, GPIO4–GPIO11)                                 |
| **Digital outputs** | 8 × optocoupler-isolated Darlington (DO1–DO8, via TCA9554 I²C expander)          |
| **RS-485**          | Half-duplex UART (GPIO17 TX, GPIO18 RX)                                          |
| **LED**             | WS2812 RGB (GPIO38)                                                              |
| **Buzzer**          | Piezo, LEDC PWM (GPIO46)                                                         |
| **Power**           | 7–36 V DC or USB-C                                                               |

---

## Features

### Connectivity

- **WiFi provisioning** — on first boot the device opens a `Waveshare-Setup` access point with a captive portal to enter
  WiFi credentials. Hold the BOOT button (GPIO0) for ≥ 5 s to clear credentials and re-enter provisioning mode.
- **Ethernet** — W5500 SPI Ethernet runs in parallel with WiFi; whichever interface gets an IP first starts services.
- **DHCP hostname** — device registers under its configured name (default `Waveshare-ESP32`).

### Web Configuration UI

Served at `http://<device-ip>/` on port 80. Settings include:

| Section         | What you can configure                             |
|-----------------|----------------------------------------------------|
| Device          | Device name (also used as DHCP hostname), LED mode |
| MQTT Broker     | Broker URL, username/password, topic prefix        |
| Digital Inputs  | Per-input name and invert flag                     |
| Digital Outputs | Per-output name and invert flag                    |
| Modbus RTU      | Enable/disable, slave address, baudrate            |

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
| Digital outputs (bulk) | `<prefix>/output/set`               |
| LED                    | `<prefix>/led/set`                   |
| Buzzer                 | `<prefix>/buzzer/beep`               |

- Input topics use the channel name if configured, otherwise the index number (1–8).
- Output commands accept `true`/`false`/`1`/`0`/`on`/`off`/`high`/`low`/`toggle` (case-insensitive).
- The LED accepts `#RRGGBB`, a timed single step `{"color":"#RRGGBB","duration":ms}`, or a sequence array.
- The buzzer accepts a single `{"freq":Hz,"duration":ms}` or a sequence array; omit `freq` for silent pauses.

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

**IO device mode** — controlled via MQTT `led/set` and Modbus HR 40001.

**Status feedback mode** — automatic connectivity indicator:

| LED state                | Meaning                    |
|--------------------------|----------------------------|
| 30% yellow               | Booted, no network yet     |
| 30% green                | Network (WiFi or Ethernet) |
| 30% purple               | MQTT broker connected      |
| 100% red flash (100 ms)  | MQTT message received      |
| 100% blue flash (100 ms) | MQTT message published     |

### Input / Output Details

- **Invert flag** — each DI and DO has a configurable invert flag (web UI). Useful for active-low sensors or
  normally-closed contacts.
- **Named channels** — each DI/DO can be given a name (up to 20 chars, no `/`) that replaces the index number in MQTT
  topics.
- **Debounce** — digital inputs have 10 ms software debounce.
- **Coil writes via Modbus** — also publish the new state on the corresponding MQTT topic, keeping both interfaces in
  sync.

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
│   ├── main.c           Entry point, event wiring
│   ├── app_config.c/h   NVS-backed configuration store
│   ├── app_mqtt.c/h     MQTT client wrapper
│   ├── di.c/h           Digital inputs (GPIO interrupts + debounce)
│   ├── dout.c/h         Digital outputs (TCA9554 I²C)
│   ├── eth.c/h          W5500 Ethernet (SPI)
│   ├── led.c/h          WS2812 LED (RMT) + status mode
│   ├── buzzer.c/h       Piezo buzzer (LEDC PWM)
│   ├── mb_server.c/h    Modbus RTU slave (espressif/esp-modbus)
│   ├── button.c/h       BOOT button long-press WiFi reset
│   └── web_server.c/h   HTTP config UI + REST API
├── www/
│   └── index.html       Single-page web UI (served from SPIFFS)
└── docs/
    ├── mqtt.md          MQTT API reference
    └── modbus.md        Modbus RTU register map
```

---

## Component Dependencies

| Component                     | Source                        | Purpose                          |
|-------------------------------|-------------------------------|----------------------------------|
| `espressif/esp-modbus ^2.1.2` | Component Registry            | Modbus RTU slave                 |
| `espressif/mqtt ^1.0.0`       | Component Registry            | MQTT client                      |
| `espressif/led_strip ^3.0.0`  | Component Registry            | WS2812 RMT driver                |
| `espressif/cjson *`           | Component Registry            | JSON parsing                     |
| `espressif/w5500 ^1.0.1`      | Component Registry            | W5500 Ethernet PHY               |
| `esp32-wifi-bootstrap`        | Git submodule (`components/`) | WiFi captive-portal provisioning |
