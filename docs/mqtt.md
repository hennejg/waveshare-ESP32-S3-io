# MQTT API

Target device: Waveshare ESP32-S3-POE-ETH-8DI-8DO

## Configuration

All MQTT settings are configured via the web UI (port 80) or the REST API (`POST /api/config`):

| Field               | Description                | Example                    |
|---------------------|----------------------------|----------------------------|
| `mqtt_url`          | Broker URI                 | `mqtt://192.168.1.10:1883` |
| `mqtt_user`         | Broker username (optional) | `sensor-box`               |
| `mqtt_password`     | Broker password (optional) | —                          |
| `mqtt_topic_prefix` | Prepended to all topics    | `boat/sensor-box`          |

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

- `true` = input active
- `false` = input inactive
- Edge detection with 10 ms software debounce
- Default sense: active = GPIO high. Set the **invert flag** in the web UI to reverse (active = GPIO low).

Each input can be given a **name** in the web UI config page (max 20 characters, no `/`, unique among all inputs). The name replaces the index number in the MQTT topic.

### Published topics

| Topic                           | QoS | Retained | Payload           |
|---------------------------------|-----|----------|-------------------|
| `<prefix>/input/<name-or-1..8>` | 0   | no       | `true` or `false` |

**When published:**

- On every MQTT broker connect (full state refresh of all 8 inputs)
- Whenever an input changes state (edge-triggered, 10 ms debounce)

### Subscribed topics

| Topic                 | Expected payload | Effect                                                  |
|-----------------------|------------------|---------------------------------------------------------|
| `<prefix>/input/read` | any              | Immediately publishes the current state of all 8 inputs |

---

## Digital Outputs (DO1–DO8)

The board has 8 optocoupler-isolated digital outputs driven via a TCA9554 I²C expander (address 0x20, SDA GPIO42, SCL GPIO41, 100 kHz). The invert flag and an optional name per output are set in the web UI.

Each output can be given a **name** (max 20 characters, no `/`, unique among all outputs). The name replaces the index number in all output-related topics.

> **Note:** Commands use a `/set` suffix so the device never receives its own state publications as commands. This matches the Home Assistant MQTT switch convention (`command_topic` / `state_topic`).

### Accepted payload values (all commands, case-insensitive)

| Logical state | Accepted values            |
|---------------|----------------------------|
| On / true     | `true`, `1`, `high`, `on`  |
| Off / false   | `false`, `0`, `low`, `off` |
| Toggle        | `toggle`                   |

### Individual output commands

| Topic                                                           | Payload           | Description   |
|-----------------------------------------------------------------|-------------------|---------------|
| `<prefix>/output/<name-or-1..8>/set` (one per output)          | value (see above) | Set one output |

Example: `toggle` → `<prefix>/output/3/set` flips DO3; if DO3 is named `pump`, use `<prefix>/output/pump/set`.

### Bulk output command

| Topic                  | Payload          | Description                     |
|------------------------|------------------|---------------------------------|
| `<prefix>/output/set` | value or array   | Set all outputs in one message  |

**Single value** — applies the same state to all 8 outputs:

```
true   on   1   high   → all ON
false  off  0   low    → all OFF
toggle                 → flip all
```

**JSON array** — one element per output, DO1 first. Elements may be JSON booleans, numbers, or strings from the table above (including `"toggle"` per element). A shorter array leaves the remaining outputs unchanged.

```json
[true, false, 1, 0, "on", "off", "toggle", true]
```

### Read command

| Topic                  | Payload | Effect                                  |
|------------------------|---------|-----------------------------------------|
| `<prefix>/output/read` | any     | Publish current state of all 8 outputs  |

### State topics (published)

| Topic                                      | QoS | Retained | Payload           |
|--------------------------------------------|-----|----------|-------------------|
| `<prefix>/output/<name-or-1..8>` per output | 0   | no       | `true` or `false` |

Published on broker connect, after every successful state change, and when `output/read` is received.

---

## LED (WS2812)

Single RGB LED on GPIO38. Operates in one of two modes set in the web UI:

| Mode | Description |
|------|-------------|
| **IO device** | Fully controlled via MQTT and Modbus HR 40001 |
| **Status feedback** *(default)* | Shows device connectivity state automatically (see below) |

### Status feedback mode

| State | Colour | Effect |
|-------|--------|--------|
| Boot / no network | Yellow | 30% solid |
| WiFi provisioning portal active | Blue | 30% slow blink (800 ms period) |
| Network (WiFi or Ethernet) | Green | 30% solid |
| MQTT broker connected | Purple | 30% solid |
| MQTT message received | Red | 100% flash, 100 ms |
| MQTT message published | Blue | 100% flash, 100 ms |

Transitions are automatic: the AP blink stops and becomes solid green the moment an IP is obtained; losing MQTT falls back to green (if network is up) or yellow; losing the network falls back to yellow.

In Status mode, LED commands from MQTT, Modbus, and CAN are ignored.

---

### IO mode — Command topic (subscribed)

| Topic              | Payload   | Description               |
|--------------------|-----------|---------------------------|
| `<prefix>/led/set` | see below | Set colour or run sequence |

**Accepted payload formats:**

#### Instant colour

```
#RRGGBB
```

Sets the LED immediately and permanently. Case-insensitive hex. Example: `#ff8000`

Turn off: `#000000`

#### Single timed step

```json
{"color": "#RRGGBB", "duration": <ms>}
```

Shows the colour for the given duration (1–30 000 ms), then the LED stays at that colour.

Example: `{"color": "#ff0000", "duration": 500}`

#### Sequence (up to 16 steps)

```json
[
  {"color": "#RRGGBB", "duration": <ms>},
  {"color": "#RRGGBB", "duration": <ms>}
]
```

Steps play in order. After the last step the LED stays at the final colour. Sending a new command while a sequence is running replaces the pending sequence; the current step completes first.

To turn off at the end of a sequence, make the last step `{"color": "#000000", "duration": <ms>}`.

**Example — single 500 ms red flash, then off:**
```json
[{"color": "#ff0000", "duration": 500}, {"color": "#000000", "duration": 1}]
```

**Example — slow blue–green blink:**
```json
[
  {"color": "#0000ff", "duration": 400},
  {"color": "#000000", "duration": 200},
  {"color": "#00ff00", "duration": 400},
  {"color": "#000000", "duration": 200}
]
```

### IO mode — State topic (published)

| Topic          | QoS | Retained | Payload   |
|----------------|-----|----------|-----------|
| `<prefix>/led` | 0   | no       | `#rrggbb` |

Published on connect and after each colour change (including at the end of a sequence). Not published in Status mode.

---

## Buzzer

Piezo buzzer on GPIO46 (LEDC PWM, 50% duty square wave).

### Command topic (subscribed)

| Topic                  | Payload   | Description                   |
|------------------------|-----------|-------------------------------|
| `<prefix>/buzzer/beep` | see below | Trigger a beep or run sequence |

**Accepted payload formats:**

#### Single beep

```json
{"freq": <Hz>, "duration": <ms>}
```

| Field      | Type    | Range      | Description                          |
|------------|---------|------------|--------------------------------------|
| `freq`     | integer | 100–10 000 | Frequency in Hz                      |
| `duration` | integer | 1–5 000    | Duration in ms                       |

Example: `{"freq": 2000, "duration": 300}`

#### Sequence (up to 16 steps)

```json
[
  {"freq": <Hz>, "duration": <ms>},
  {"duration": <ms>},
  {"freq": <Hz>, "duration": <ms>}
]
```

Steps play in order. Omit `freq` (or set it to `0`) for a silent pause. The buzzer is always silenced after the sequence ends. Sending a new command replaces the pending sequence; the current step completes first.

**Example — two-tone alert with pause:**
```json
[
  {"freq": 880, "duration": 150},
  {"duration": 80},
  {"freq": 1320, "duration": 150}
]
```

**Example — ascending scale:**
```json
[
  {"freq": 262, "duration": 120},
  {"freq": 330, "duration": 120},
  {"freq": 392, "duration": 120},
  {"freq": 523, "duration": 200}
]
```
