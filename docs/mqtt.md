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

With `mqtts://` the broker's certificate is checked against ESP-IDF's built-in bundle of public root CAs, so
a broker with a certificate from a public CA (Let's Encrypt and the like) connects. A broker with a
self-signed certificate or one from a private CA is refused (TLS alert "bad certificate"); there is no
setting for a custom CA yet. Before this check was configured, `mqtts://` never connected at all.

A changed broker URL takes effect after a reboot.

The retained message on the prefix itself (`<prefix>`) is the device's health, refreshed every 60 s:
`{"online":true,"device":"…","uptime_s":…,"heap_internal_free":…,"heap_internal_min":…,"stack_free":{"di":…,…}}`.
`heap_internal_free`/`_min` are the internal RAM free now and the least since boot -- what runs out first. `stack_free` is
the least stack each task that publishes has had left since boot, in bytes -- with `mqtts://` the
publish runs the TLS record write on that task's stack. The last will sets it to `{"online":false}`.

If no broker URL is configured, MQTT is silently disabled at boot.

## Topic Structure

```
<prefix>/<resource>/<name>
```

If `mqtt_topic_prefix` is empty, topics are published without a prefix.

Topics starting with `/` in the API calls bypass the prefix and are used as-is (absolute topics).

Incoming commands are matched on the whole topic, prefix removed: `<prefix>/output/set`
is the bulk command, `<prefix>/rules/output/set` is not — it reaches the device through
the rule engine's `rules/#` subscription and is a rule topic, nothing else.

The client reconnects automatically with exponential backoff if the broker becomes unreachable.

---

## Digital Inputs

The board has 8 optocoupler-isolated digital inputs on GPIO4–GPIO11 (DI1–DI8).

Three more, non-isolated, on GPIO1/GPIO44/GPIO43 are published once switched to input in the web UI (`APP_AUX_DI_ENABLE`); a pad left in its standard function is not published. They are called AUX1–AUX3
in the web UI and are published like any other input — the topic index continues after DI8:

| Web UI | Pad | GPIO   | DI index | Default topic       |
|--------|-----|--------|----------|---------------------|
| AUX1   | IO1 | GPIO1  | 8        | `<prefix>/input/9`  |
| AUX2   | RXD | GPIO44 | 9        | `<prefix>/input/10` |
| AUX3   | TXD | GPIO43 | 10       | `<prefix>/input/11` |

The three pads start in their standard function, so a board that has not been configured publishes 8 inputs. See the
README for the wiring caveats.

- `true` = input active
- `false` = input inactive
- Edge detection with 10 ms software debounce
- Default sense: active = GPIO high. Set the **invert flag** in the web UI to reverse (active = GPIO low).

Each input can be given a **name** in the web UI config page (max 20 characters, no `/`, `+` or `#`, not `read`). Names must be unique among all inputs, and that includes the numbers of unnamed inputs: DI1 cannot be called `2` while DI2 has no name. The name replaces the index number in the MQTT topic.

### Published topics

| Topic                           | QoS | Retained | Payload           |
|---------------------------------|-----|----------|-------------------|
| `<prefix>/input/<name-or-1..8>` | 0   | no       | `true` or `false` |

`<name-or-1..8>` becomes `<name-or-1..11>` when `APP_AUX_DI_ENABLE` is set.

**When published:**

- On every MQTT broker connect (full state refresh of every input)
- Whenever an input changes state (edge-triggered, 10 ms debounce)

### Subscribed topics

| Topic                 | Expected payload | Effect                                                  |
|-----------------------|------------------|---------------------------------------------------------|
| `<prefix>/input/read` | any              | Immediately publishes the current state of every input  |

---

## Digital Outputs (DO1–DO8)

The board has 8 optocoupler-isolated digital outputs driven via a TCA9554 I²C expander (address 0x20, SDA GPIO42, SCL GPIO41, 100 kHz). The invert flag and an optional name per output are set in the web UI.

Each output can be given a **name** (max 20 characters, no `/`, `+` or `#`, not `set` or `read`, which are the bulk command topics). Names must be unique among all outputs, the pads' outputs 9–11 included, and that includes the numbers of unnamed outputs. A stored name that breaks these rules, from an older firmware, is dropped at boot and the channel answers to its number again. The name replaces the index number in all output-related topics.

> **Note:** Commands use a `/set` suffix so the device never receives its own state publications as commands. This matches the Home Assistant MQTT switch convention (`command_topic` / `state_topic`).

### Accepted payload values (all commands, case-insensitive)

| Logical state | Accepted values            |
|---------------|----------------------------|
| On / true     | `true`, `1`, `high`, `on`  |
| Off / false   | `false`, `0`, `low`, `off` |
| Toggle        | `toggle`                   |

The payload is the word and nothing else: a NUL byte or anything after the word refuses the command
(`on\0off` is not `on`). JSON payloads (bulk outputs, LED, buzzer, PWM) are one JSON value, then only
white space -- `50 off` or an object with bytes appended is refused, not read as the value in front -- and a string
in it must not contain `\u0000` (`["on\u0000off"]` is refused, not read as `on`).

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

## Header pads IO1 / RXD / TXD

The three non-isolated header pads are set to a mode on the web UI's Device tab. A pad set to *digital input*
publishes like any other input (`input/9` to `input/11`, see above). For the other modes, `<n>` is 1 for IO1, 2 for
RXD, 3 for TXD:

| Topic | Direction | Payload |
|-------|-----------|---------|
| `<prefix>/aux/<n>/mode` | published, retained | `standard`, `input`, `pwm` or `analog` |
| `<prefix>/aux/<n>/pwm` | published, retained | `{"duty":25.0,"freq":1000}` — the PWM as set; an empty retained payload once the pad is no longer a PWM output |
| `<prefix>/aux/<n>/pwm/set` | subscribed | `25` (duty in %) or `{"duty":25,"freq":1000}`; either field may be left out, but a field that is present must be a number |
| `<prefix>/aux/1/analog` | published | IO1 analog input in millivolts, whenever it moves by 20 mV or more |
| `<prefix>/aux/read` | subscribed | anything — publishes all of the above |

A pad set to *digital output* behaves like DO1–DO8, numbered after them: `<prefix>/output/<name or 9..11>` is
published (`true`/`false`) when it changes, `<prefix>/output/<name or 9..11>/set` takes the same payloads as a DO
(`true`/`on`/`1`, `false`/`off`/`0`, `toggle`), and `<prefix>/output/read` publishes it too. The bulk command
`output/set` covers DO1–DO8 only.

A `pwm/set` takes effect at once and is not stored: after a reboot the pad starts with the configured values. Only a
change is republished. A
command for a pad that is not a PWM output, or with a value out of range (duty 0–100, frequency 5–1000000 Hz), is
ignored and logged.

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

Sets the LED immediately and permanently. Case-insensitive hex, exactly seven characters (`#ff0000xyz` or
`#zzzzzz` is refused). Example: `#ff8000`. It ends a running sequence -- as does a colour set over Modbus, CAN,
a rule or the web page.

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
