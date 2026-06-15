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

- `true` = input active (current flowing through optocoupler, GPIO pulled low)
- `false` = input inactive
- Edge detection with 10 ms hardware debounce

### Published topics

| Topic              | QoS | Retained | Payload           |
|--------------------|-----|----------|-------------------|
| `<prefix>/input/1` | 0   | no       | `true` or `false` |
| `<prefix>/input/2` | 0   | no       | `true` or `false` |
| …                  |     |          |                   |
| `<prefix>/input/8` | 0   | no       | `true` or `false` |

**When published:**

- On every MQTT broker connect (full state refresh of all 8 inputs)
- Whenever an input changes state (edge-triggered, 10 ms debounce)

### Subscribed topics

| Topic                 | Expected payload | Effect                                                  |
|-----------------------|------------------|---------------------------------------------------------|
| `<prefix>/input/read` | any              | Immediately publishes the current state of all 8 inputs |

---

---

## Digital Outputs (DO1–DO8)

The board has 8 optocoupler-isolated digital outputs driven via a TCA9554 I²C expander (address 0x20, SDA GPIO42, SCL
GPIO41, 100 kHz).

Output state is driven by bit-level writes to the TCA9554 output port register. The invert flag per output is set in the
web UI config page.

### Command topics (subscribed)

| Topic                                             | Payload   | Description                            |
|---------------------------------------------------|-----------|----------------------------------------|
| `<prefix>/output/1/set` … `<prefix>/output/8/set` | see below | Set individual output state            |
| `<prefix>/outputs/set`                            | see below | Set all outputs at once (bulk)         |
| `<prefix>/output/read`                            | any       | Publish current state of all 8 outputs |

**Bulk set payloads (`outputs/set`):**

*Single value* — applied to all 8 outputs:

```
true   1   high   on     → all ON
false  0   low    off    → all OFF
toggle                   → flip all
```

*JSON array* — one element per output (in order DO1…DO8); accepts the same values as individual outputs plus JSON booleans and numbers:

```json
[true, false, 1, 0, "on", "off", "toggle", true]
```

Array may be shorter than 8; unspecified outputs are left unchanged.

> **Note:** Commands use the `/set` suffix so the device does not receive its own state publications as commands. This
> matches the Home Assistant MQTT switch convention (`command_topic` / `state_topic`).

**Accepted payloads** (case-insensitive):

| Logical state | Accepted values            |
|---------------|----------------------------|
| On / true     | `true`, `1`, `high`, `on`  |
| Off / false   | `false`, `0`, `low`, `off` |
| Toggle        | `toggle`                   |

### State topics (published)

| Topic                                     | QoS | Retained | Payload           |
|-------------------------------------------|-----|----------|-------------------|
| `<prefix>/output/1` … `<prefix>/output/8` | 0   | no       | `true` or `false` |

**When published:**

- On every MQTT broker connect (full state refresh)
- After every successful output change (confirmation)
- When `output/read` is received

---

## LED (WS2812)

Single RGB LED on GPIO38.

### Command topic (subscribed)

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

### State topic (published)

| Topic          | QoS | Retained | Payload   |
|----------------|-----|----------|-----------|
| `<prefix>/led` | 0   | no       | `#rrggbb` |

Published on connect and after each colour change (including at the end of a sequence).

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
