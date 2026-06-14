# MQTT API

Target device: Waveshare ESP32-S3-POE-ETH-8DI-8DO

## Configuration

All MQTT settings are configured via the web UI (port 80) or the REST API (`POST /api/config`):

| Field | Description | Example |
|-------|-------------|---------|
| `mqtt_url` | Broker URI | `mqtt://192.168.1.10:1883` |
| `mqtt_user` | Broker username (optional) | `sensor-box` |
| `mqtt_password` | Broker password (optional) | — |
| `mqtt_topic_prefix` | Prepended to all topics | `boat/sensor-box` |

Supported URI schemes: `mqtt://` (plain TCP), `mqtts://` (TLS).

If no broker URL is configured, MQTT is silently disabled at boot.

## Topic Structure

```
<prefix>/<resource>/<name>
```

If `mqtt_topic_prefix` is empty, topics are published without a prefix.

Topics starting with `/` in the API calls bypass the prefix and are used as-is (absolute topics).

The client reconnects automatically with exponential backoff if the broker becomes unreachable.

---

## Digital Inputs (DI1–DI8)

The board has 8 optocoupler-isolated digital inputs on GPIO4–GPIO11.

- `true` = input active (current flowing through optocoupler, GPIO pulled low)
- `false` = input inactive
- Edge detection with 10 ms hardware debounce

### Published topics

| Topic | QoS | Retained | Payload |
|-------|-----|----------|---------|
| `<prefix>/input/1` | 0 | no | `true` or `false` |
| `<prefix>/input/2` | 0 | no | `true` or `false` |
| … | | | |
| `<prefix>/input/8` | 0 | no | `true` or `false` |

**When published:**
- On every MQTT broker connect (full state refresh of all 8 inputs)
- Whenever an input changes state (edge-triggered, 10 ms debounce)

### Subscribed topics

| Topic | Expected payload | Effect |
|-------|-----------------|--------|
| `<prefix>/input/read` | any | Immediately publishes the current state of all 8 inputs |

---

---

## Digital Outputs (DO1–DO8)

The board has 8 optocoupler-isolated digital outputs driven via a TCA9554 I²C expander (address 0x20, SDA GPIO42, SCL GPIO41, 100 kHz).

Output state is driven by bit-level writes to the TCA9554 output port register. The invert flag per output is set in the web UI config page.

### Command topics (subscribed)

| Topic | Payload | Description |
|-------|---------|-------------|
| `<prefix>/output/1/set` … `<prefix>/output/8/set` | see below | Set output state |
| `<prefix>/output/read` | any | Publish current state of all 8 outputs |

> **Note:** Commands use the `/set` suffix so the device does not receive its own state publications as commands. This matches the Home Assistant MQTT switch convention (`command_topic` / `state_topic`).

**Accepted payloads** (case-insensitive):

| Logical state | Accepted values |
|--------------|-----------------|
| On / true    | `true`, `1`, `high`, `on` |
| Off / false  | `false`, `0`, `low`, `off` |
| Toggle       | `toggle` |

### State topics (published)

| Topic | QoS | Retained | Payload |
|-------|-----|----------|---------|
| `<prefix>/output/1` … `<prefix>/output/8` | 0 | no | `true` or `false` |

**When published:**
- On every MQTT broker connect (full state refresh)
- After every successful output change (confirmation)
- When `output/read` is received

---

## LED (WS2812)

Single RGB LED on GPIO38.

### Command topic (subscribed)

| Topic | Payload | Description |
|-------|---------|-------------|
| `<prefix>/led/set` | see below | Set LED colour |

**Accepted payload formats:**

| Format | Example | Notes |
|--------|---------|-------|
| Web hex | `#ff8000` | Case-insensitive |
| JSON array | `[255,128,0]` | Spaces around values allowed |

To turn the LED off: `#000000` or `[0,0,0]`.

### State topic (published)

| Topic | QoS | Retained | Payload |
|-------|-----|----------|---------|
| `<prefix>/led` | 0 | no | `#rrggbb` |

Published on connect and after each colour change.

---

## Buzzer

Piezo buzzer on GPIO46 (LEDC PWM, 50% duty square wave).

### Command topic (subscribed)

| Topic | Payload | Description |
|-------|---------|-------------|
| `<prefix>/buzzer/beep` | JSON object | Trigger a beep |

**Payload fields:**

| Field | Type | Default | Range | Description |
|-------|------|---------|-------|-------------|
| `freq` | integer | 1000 | 100–10000 | Frequency in Hz |
| `duration` | integer | 200 | 1–5000 | Duration in ms |

**Example:** `{"freq": 2000, "duration": 300}`

If a beep is already playing when a new command arrives, the new beep queues and plays immediately after the current one finishes.
