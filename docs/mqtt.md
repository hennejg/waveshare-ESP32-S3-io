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
| `<prefix>/output/1` … `<prefix>/output/8` | see below | Set output state |
| `<prefix>/output/read` | any | Publish current state of all 8 outputs |

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
