# Generic Waveshare ESP32-S3-POE-ETH-8DI-8DO and S3-POE-ETH-8DI-8RO Firmware

[![CI](https://github.com/hennejg/waveshare-ESP32-S3-io/actions/workflows/ci.yml/badge.svg)](https://github.com/hennejg/waveshare-ESP32-S3-io/actions/workflows/ci.yml)
[![Latest release](https://img.shields.io/github/v/release/hennejg/waveshare-ESP32-S3-io)](https://github.com/hennejg/waveshare-ESP32-S3-io/releases/latest)

**[⬇ Download latest release](https://github.com/hennejg/waveshare-ESP32-S3-io/releases/latest)** · **[⚡ Flash via browser](https://hennejg.github.io/waveshare-ESP32-S3-io/)** (Chrome/Edge + USB)

Generic firmware based on ESP-IDF for
the [Waveshare ESP32-S3-POE-ETH-8DI-8DO](https://www.waveshare.com/wiki/ESP32-S3-POE-ETH-8DI-8DO)
and [Waveshare ESP32-S3-POE-ETH-8DI-8RO](https://www.waveshare.com/wiki/ESP32-S3-ETH-8DI-8RO) industrial I/O expansion
modules.

The firmware that comes with those devices is relatively limited and hard to use without modifying and flashing it.
Having open hardware that lets one tinker with it is great and something to like about the Waveshare products.
However, sometimes you want something that just works out of the box — and this is where this project might come in handy.

The project ships **two firmware flavors**. The Matter/CHIP SDK consumes too much DMA-capable RAM to coexist with the
full feature set in a single binary, so the features are split across two separate applications that share the same
board hardware abstraction layer (`components/board/`):

| Flavor | Binary | What it includes |
|--------|--------|-----------------|
| **Full** (`apps/full/`) | `waveshare_esp32s3_full.bin` | Rule engine, MQTT, CAN bus, Modbus RTU, full web UI — **no Matter** |
| **Matter** (`apps/matter/`) | `waveshare_esp32s3_matter.bin` | WiFi provisioning + Matter (Apple Home / Google Home), minimal web UI — no rule engine, no MQTT |

**If you are not sure which to use, start with the full firmware** — it is what the CI builds and the releases contain.

## Hardware

|                     |                                                                                                         |
|---------------------|---------------------------------------------------------------------------------------------------------|
| **MCU**             | ESP32-S3-WROOM-1U-N16R8 (dual-core Xtensa LX7, 240 MHz, 16 MB Flash, 8 MB PSRAM)                        |
| **Ethernet**        | W5500 SPI (10/100 Mbit)                                                                                 |
| **Digital inputs**  | 8 × optocoupler-isolated (DI1–DI8, GPIO4–GPIO11)                                                        |
| **Digital outputs** | (ESP32-S3-POE-ETH-8DI-8DO only) 8 × optocoupler-isolated Darlington (DO1–DO8, via TCA9554 I²C expander) |
| **Relay outputs**   | (ESP32-S3-POE-ETH-8DI-8RO only) 8 × 1NO 1NC; ≤10A 250V AC or ≤10A 30V DC                                |
| **CAN bus**         | ESP32-S3 TWAI peripheral (GPIO2 TX, GPIO3 RX)                                                           |
| **RS-485**          | Half-duplex UART (GPIO17 TX, GPIO18 RX)                                                                 |
| **LED**             | WS2812 RGB (GPIO38)                                                                                     |
| **Buzzer**          | Piezo, LEDC PWM (GPIO46)                                                                                |
| **Power**           | 7–36 V DC or USB-C                                                                                      |

---

## Features

### Connectivity

- **WiFi provisioning** — on first boot the device opens a `Waveshare-Setup` access point. Connect to it on any device;
  a captive portal appears automatically to enter WiFi credentials. The LED blinks 30% blue while the portal is active.
- **ETH-only mode** — if you have an Ethernet cable, tap **"Use Ethernet only →"** on the provisioning portal to skip
  WiFi entirely. The preference is stored and survives reboots.
- **Ethernet** — W5500 SPI Ethernet always runs in parallel (or alone in ETH-only mode); whichever interface gets an IP
  first starts the services.
- **DHCP hostname** — the device registers under its configured name (default `Waveshare-ESP32`).
- **Return to WiFi provisioning** — hold the BOOT button (GPIO0) for ≥ 5 s at any time. This clears WiFi credentials,
  clears the ETH-only flag, and reboots into the provisioning portal.

### Authentication

The web UI and REST API are password-protected (CRA-compliant: no hardcoded default password).

**Initial password setup:**

1. Open `http://<device-ip>/` — a setup screen appears because no password has been set yet.
2. Click **"Set up password…"** — the device LED blinks yellow rapidly.
3. Briefly press the BOOT button on the device within 30 s to confirm physical access.
4. Enter and confirm a password (min. 8 characters).

**Subsequent logins:** the browser prompts for the password. Credentials are stored in `sessionStorage` (cleared when
the tab is closed).

**Password reset** (forgotten password): click **"Forgot password?"** on the login screen and repeat the BOOT button
flow — no prior authentication required, physical access is sufficient.

### Web Configuration UI

Served at `http://<device-ip>/` on port 80. Settings include (full firmware):

| Section          | What you can configure                                                      |
|------------------|-----------------------------------------------------------------------------|
| Device           | Device name (DHCP hostname), LED mode                                       |
| MQTT Broker      | Broker URL, username/password, topic prefix                                 |
| Digital Inputs   | Per-input name and invert flag                                              |
| Digital Outputs  | Per-output name and invert flag                                             |
| CAN Bus          | Mode (Off / Basic / NMEA2000), address, bit rate                            |
| Modbus RTU       | Enable/disable, slave address, baudrate                                     |
| Automation Rules | JavaScript rule script (see [Automation Rules](#automation-rules))          |

The UI also provides **Save**, **Reboot**, and **Factory Reset** buttons.

#### REST API (full firmware)

All endpoints except the auth flow require an `Authorization: Basic base64(:<password>)` header when a password is set.

| Endpoint                 | Method | Auth required | Description                                      |
|--------------------------|--------|---------------|--------------------------------------------------|
| `/api/auth/status`       | GET    | No            | `{"password_set": bool}`                         |
| `/api/auth/begin`        | POST   | No            | Start token flow (LED blinks, 30 s)              |
| `/api/auth/token?s=<id>` | GET    | No            | Poll: `waiting` / `ready` / `timeout`            |
| `/api/auth/set-password` | POST   | No            | `{"token":"…","password":"…"}`                   |
| `/api/config`            | GET    | Yes           | Read full configuration as JSON                  |
| `/api/config`            | POST   | Yes           | Update configuration from JSON                   |
| `/api/reboot`            | POST   | Yes           | Restart immediately                              |
| `/api/factory-reset`     | POST   | Yes           | Erase all settings and restart                   |
| `/api/rules`             | GET    | Yes           | Read the current rule script as `{"script":"…"}` |
| `/api/rules`             | POST   | Yes           | Save + hot-reload rules from `{"script":"…"}`    |

The matter firmware exposes a smaller API: `/api/auth/*`, `/api/config` (subset), `/api/reboot`,
`/api/factory-reset`, `/api/matter/pairing` (GET — QR code + commissioning status), and
`/api/matter/decommission` (POST — erase fabrics and reboot).

### Automation Rules

A built-in rule engine wires digital inputs, digital outputs, and MQTT messages together
with a small JavaScript DSL. Rules are edited in the **Rules** tab of the web UI — a
CodeMirror editor with JavaScript syntax highlighting and line numbers (self-hosted, so it
works on an isolated network) — or via `POST /api/rules`, saved to NVS, and hot-reloaded
without a reboot.

See the **[Rule Language Reference](components/scripting/RULES.md)** for the full syntax —
rule structure, the `input` / `output` / `mqtt` facts and their helpers, time-based rules
(`.heldFor` / `.after`), and rule chaining, with examples throughout. You can prototype and
test rules in the browser with the simulator in [`simulator/`](simulator/) — run
`cd simulator && npm start` and open <http://localhost:3000> (see
[`simulator/README.md`](simulator/README.md)). It runs the live engine source.

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

| Mode         | Frame type      | Bit rate     | Description             |
|--------------|-----------------|--------------|-------------------------|
| **Off**      | —               | —            | Disabled                |
| **Basic**    | 11-bit standard | Configurable | Simple custom frame map |
| **NMEA2000** | 29-bit extended | 250 kbit/s   | Standard marine network |

**Basic mode** — a compact 6-frame protocol covering DI state, DO set/echo (WRITE/SET/CLEAR/TOGGLE opcodes + bitmask),
LED colour, buzzer, and a read-request trigger.

**NMEA2000 mode** implements ISO address claiming, PGN 126993 Heartbeat, PGN 127501/127502 Binary Switch Banks (DI bank
0, DO bank 1), and PGN 126720 Manufacturer Proprietary fast-packet (LED + buzzer).

### Matter (Apple Home / Google Home / Home Assistant) — separate firmware

Full details in [`docs/matter.md`](docs/matter.md).

Matter support is provided by the **`apps/matter/`** firmware flavor (see [Build & Flash — Matter firmware](#matter-firmware)).
It is **not** present in the full firmware because the CHIP SDK exhausts DMA-capable RAM when combined with
the rule engine and MQTT stack.

Flash `apps/matter/` if you need to integrate the device into Apple Home, Google Home, or Home Assistant via the
Matter protocol. You give up the rule engine, MQTT, CAN bus, and Modbus RTU in exchange.

| Feature               | Description                                                                               |
|-----------------------|-------------------------------------------------------------------------------------------|
| **8 Digital Outputs** | Exposed as *On/Off Plug-in Unit* endpoints (controllable)                                 |
| **8 Digital Inputs**  | Exposed as *Contact Sensor* endpoints (read-only state)                                   |
| **Pairing code**      | Unique passcode + discriminator generated on first boot; survives reboot and decommission |
| **QR code**           | Shown in the web UI; scan with your home app to pair                                      |
| **Decommission**      | Erase all fabric data via `POST /api/matter/decommission` or the minimal web UI           |
| **Identify**          | LED blinks cyan at 1 Hz when any endpoint's Identify cluster is triggered                 |

**Important — Contact Sensor = Door/Window in most home apps.** Home Assistant, HomeKit, and Google Home all render
Contact Sensor endpoints as door or window sensors (open/closed), not as generic binary sensors. This is correct per the
Matter spec but can look surprising. See [`docs/matter.md`](docs/matter.md) for details on renaming endpoints and
alternative device types.

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

The WS2812 LED has two operating modes (configurable in the web UI). **Status feedback is the default.**

**IO device mode** — fully controlled via MQTT `led/set`, Modbus HR 40001, and CAN base+3 / NMEA2000 PGN 126720.

**Status feedback mode** — automatic connectivity indicator:

| LED state                | Meaning                              |
|--------------------------|--------------------------------------|
| 30% yellow (solid)       | Booted, no network yet               |
| 30% blue (slow blink)    | WiFi provisioning portal active      |
| 30% green (solid)        | Network (WiFi or Ethernet) connected |
| 30% purple (solid)       | MQTT broker connected                |
| 100% red flash (100 ms)  | MQTT message received                |
| 100% blue flash (100 ms) | MQTT message published               |

Transitions are evaluated automatically: losing MQTT falls back to green; losing the network falls back to yellow. In
Status mode all LED commands from MQTT, Modbus, and CAN are ignored.

### Input / Output Details

- **Invert flag** — each DI and DO has a configurable invert flag (web UI). Applied consistently across MQTT, Modbus,
  and CAN.
- **Named channels** — each DI/DO can be given a name (up to 20 chars, no `/`, unique per type) that replaces the index
  number in MQTT topics.
- **Debounce** — digital inputs have 10 ms software debounce.
- **Unified state** — a DO write via any interface (MQTT, Modbus, CAN) is reflected immediately on all others. MQTT
  confirmation is published, Modbus coil is updated, CAN echoes the new state.

---

## Build & Flash

### Prerequisites

- ESP-IDF v5.5.4 — pinned as a git submodule under `esp-idf/`

```sh
# First-time setup (after clone) — installs toolchains for both firmware flavors
git submodule update --init --depth 1 esp-idf components/esp32-wifi-bootstrap components/quickjs/extern/quickjs
./esp-idf/install.sh esp32s3
```

The Matter firmware also requires the `esp-matter` submodule (large — ~1 GB):

```sh
git submodule update --init --recursive esp-matter
```

A convenience wrapper `idf.sh` at the project root sources `esp-idf/export.sh` and forwards all arguments to
`idf.py -C apps/full`. Use it to avoid repeating the source step.

### Full firmware

The full firmware (`apps/full/`) includes the rule engine, MQTT, CAN bus, Modbus RTU, and the full web UI.
It does **not** require the `esp-matter` submodule.

```sh
# Build
./idf.sh build

# Flash everything (app + SPIFFS web UI + partition table)
./idf.sh -p /dev/ttyUSBx flash

# App only (fastest — use when only firmware changed)
./idf.sh -p /dev/ttyUSBx app-flash

# SPIFFS only (use when www/ content changed)
./idf.sh -p /dev/ttyUSBx spiffs-flash
```

Alternatively, without the wrapper:

```sh
. ./esp-idf/export.sh
idf.py -C apps/full build
idf.py -C apps/full -p /dev/ttyUSBx flash
```

### Matter firmware

The matter firmware (`apps/matter/`) requires the `esp-matter` submodule to be initialized first (see Prerequisites).

```sh
. ./esp-idf/export.sh
idf.py -C apps/matter build
idf.py -C apps/matter -p /dev/ttyUSBx flash
```

> **Note:** The matter firmware has no SPIFFS partition. There is no `spiffs-flash` target.

### Flashing a release binary

Download `waveshare-esp32s3-<version>-full-flash.zip` from the
[Releases](https://github.com/hennejg/waveshare-ESP32-S3-io/releases/latest) page, unzip, then from the
extracted folder:

```sh
# Full install (first time or after partition layout changes)
python -m esptool --chip esp32s3 -p /dev/ttyUSBx -b 460800 \
  --before default-reset --after hard-reset \
  write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB \
  @flash_args

# App update only
python -m esptool --chip esp32s3 -p /dev/ttyUSBx -b 460800 \
  --before default-reset --after hard-reset \
  write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x20000 waveshare_esp32s3_full-<version>.bin
```

### First boot after fresh flash

1. Connect to the `Waveshare-Setup` WiFi AP (or plug in an Ethernet cable and tap "Use Ethernet only" on the portal).
2. After getting an IP, open `http://<device-ip>/`.
3. Follow the password setup flow: click "Set up password…", press BOOT button, set password.

---

## Project Structure

```
├── apps/
│   ├── full/                     Full firmware (rule engine, MQTT, CAN, Modbus — no Matter)
│   │   ├── CMakeLists.txt
│   │   ├── sdkconfig.defaults
│   │   ├── partitions.csv        App + SPIFFS layout
│   │   └── main/
│   │       ├── main.c            Entry point, event/callback wiring
│   │       ├── app_mqtt.c/h      MQTT client wrapper
│   │       ├── app_time.c/h      Timezone / local-time helpers
│   │       ├── can_server.c/h    CAN bus (Basic + NMEA2000 modes)
│   │       ├── mb_server.c/h     Modbus RTU slave
│   │       └── web_server.c/h    HTTP config UI + full REST API
│   └── matter/                   Matter firmware (WiFi + Matter — no rule engine, no MQTT)
│       ├── CMakeLists.txt
│       ├── sdkconfig.defaults
│       ├── partitions.csv        App-only layout (no SPIFFS)
│       └── main/
│           ├── main.c            Thin entry point
│           ├── matter.cpp/h      Matter integration (esp-matter, 8 DO + 8 DI endpoints)
│           └── web_server.c/h    Minimal HTTP UI + matter/status endpoints
├── components/
│   ├── board/                    Shared hardware abstraction layer (both firmware flavors)
│   │   ├── app_config.c/h        NVS-backed configuration store
│   │   ├── auth.c/h              Web UI authentication (password + BOOT button token flow)
│   │   ├── button.c/h            BOOT button — long-press WiFi reset, short-press auth token
│   │   ├── buzzer.c/h            Piezo buzzer (LEDC PWM) — single beep + sequences
│   │   ├── di.c/h                Digital inputs (GPIO interrupts + debounce)
│   │   ├── dout.c/h              Digital outputs (TCA9554 I²C)
│   │   ├── eth.c/h               W5500 Ethernet (SPI)
│   │   ├── i2c_bus.c/h           Shared I²C bus init
│   │   ├── led.c/h               WS2812 LED (RMT) — IO and Status modes
│   │   ├── rtc.c / app_rtc.h     DS3231 battery-backed RTC
│   │   └── sntp_sync.c/h         SNTP client + RTC write-back
│   ├── scripting/                Rule engine (QuickJS DSL — full firmware only)
│   │   └── RULES.md              Rule language reference
│   ├── quickjs/                  QuickJS JS engine (git submodule)
│   └── esp32-wifi-bootstrap/     WiFi captive-portal provisioning (git submodule)
├── www/
│   └── index.html                Single-page web UI (served from SPIFFS, full firmware only)
├── simulator/                    Browser-based rule engine simulator
│   └── README.md
├── docs/
│   ├── can.md                    CAN bus API (Basic and NMEA2000 modes)
│   ├── matter.md                 Matter integration — device model, commissioning, known quirks
│   ├── mqtt.md                   MQTT API reference
│   └── modbus.md                 Modbus RTU register map
├── esp-idf/                      ESP-IDF v5.5.4 (git submodule)
├── esp-matter/                   Matter SDK / connectedhomeip (git submodule, matter firmware only)
└── idf.sh                        Convenience wrapper: sources export.sh, defaults to apps/full
```

---

## Component Dependencies

### Shared (`components/board/`)

| Component                    | Source                        | Purpose                          |
|------------------------------|-------------------------------|----------------------------------|
| `espressif/led_strip ^3.0.0` | Component Registry            | WS2812 RMT driver                |
| `esp32-wifi-bootstrap`       | Git submodule (`components/`) | WiFi captive-portal provisioning |

### Full firmware (`apps/full/`)

| Component                     | Source             | Purpose          |
|-------------------------------|--------------------|------------------|
| `espressif/esp-modbus ^2.1.2` | Component Registry | Modbus RTU slave |
| `espressif/mqtt ^1.0.0`       | Component Registry | MQTT client      |
| `espressif/cjson *`           | Component Registry | JSON parsing     |

### Matter firmware (`apps/matter/`)

| Component    | Source                        | Purpose                      |
|--------------|-------------------------------|------------------------------|
| `esp-matter` | Git submodule (`esp-matter/`) | Matter SDK (connectedhomeip) |
