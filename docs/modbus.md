# Modbus API

Target device: Waveshare ESP32-S3-POE-ETH-8DI-8DO

The board speaks Modbus two ways, configured in the web UI (**Modbus** tab).
Changes take effect after reboot.

## RS-485

| Parameter          | Details                                   |
|--------------------|-------------------------------------------|
| Physical interface | RS-485 half-duplex (GPIO17 TX, GPIO18 RX) |
| Protocol           | Modbus RTU                                |
| Data bits          | 8                                         |
| Parity             | None                                      |
| Stop bits          | 1                                         |

The segment has one master, and the board is either it or a slave on it:

- **Slave** (default) — answers requests sent to its address. The register map
  below is what it answers with.
- **Master** — drives the segment for Modbus TCP clients. The board has no
  address of its own then, and nothing else may be master.

As master the board also needs to know how long a device may take to answer.
*MODBUS over serial line V1.02* §2.4.1 leaves the value to the application and
calls 1 s to several seconds typical at 9600 baud, which is the default here;
it is adjustable from 50 ms to 10 s. Lower it only for a segment you know is
faster — a device that would have answered is otherwise reported as
`0x0B`, not responding.

## Modbus TCP

Off by default. Enabled, the board answers on **port 502**, up to eight
connections at once.

> Modbus TCP has no authentication. Anything that can reach the board can read
> its inputs and switch its outputs — and, with RS-485 set to master,
> everything on the segment as well. Only enable it on a network you control.

Which device a request is for is decided by the MBAP unit identifier:

| Unit ID            | Answered by                                              |
|--------------------|----------------------------------------------------------|
| 247 (configurable) | This board — the register map below                       |
| 0 and 255          | This board as well; a client with nothing to address sends one of them |
| anything else      | The device with that address on RS-485, if the board is master |

The board also answers FC 43/14, Read Device Identification, for its own
unit ID — see "Read Device Identification" under the register map.

With RS-485 set to slave there is nothing to forward to, and any other unit ID
is answered with exception `0x0A`.

### Forwarded requests

Function codes 01, 02, 03, 04, 05, 06, 15 and 16 are relayed; anything else is
answered with exception `0x01`. The answer comes back as the device gave it,
including its exception code. Two codes are the gateway's own:

| Exception | Meaning                                                   |
|-----------|-----------------------------------------------------------|
| `0x0B`    | The device did not answer within the configured timeout    |
| `0x0A`    | There is no route to it — RS-485 is not in master mode     |
| `0x06`    | Every worker is busy on the segment; retry                 |

A request the board answers itself is never delayed by the segment: measured
on the bench, a local read stays at 3 ms while six requests to devices that
never answer are in flight. The segment itself carries one transaction at a
time, so forwarded requests queue behind each other.

### Limits

| | |
|---|---|
| Connections served at once | 8; a ninth is closed immediately |
| Requests forwarded at once | 4; beyond that the answer is `0x06`, retry |
| Time to receive one frame  | 200 ms from its first byte, then the connection is closed |
| Time to send one response  | 500 ms, then the connection is closed |

Neither limit is a limit on anybody else: receiving and sending are both done
a step at a time, so a connection that stalls holds up only itself.

A connection is kept open for as long as the client wants it. One whose peer
has disappeared without closing is found by TCP keepalive after about 90 s and
its slot released.

## Reading other Modbus TCP devices

Independent of everything above: whatever the RS485 side is doing, the board
can fetch values from other Modbus TCP devices by itself. Up to eight, each
configured in the web UI (**Modbus** tab) with where the value lives and how to
read it.

| Field | Meaning |
|---|---|
| Name | what the value is called; becomes the MQTT topic and the rule key |
| Host, Port | the other device; port is usually 502 |
| Unit | its Modbus unit ID |
| Register | **zero-based**, the number a master puts on the wire — not the 4xxxx form |
| holding / input | function code 3 or 4 |
| Type | `u16`, `s16`, `u32`, `s32` or `float32` |
| Swap | the two registers of a 32-bit value the other way round (CDAB) |
| Scale | the raw value is multiplied by this |
| Every | poll interval, 200 ms to a day |

Reading only. Nothing is ever written to the other device.

Unlike the settings above, this table takes effect on save — no reboot.

### Where the value goes

Each successful read publishes on MQTT as `modbus/<name>` (under the configured
topic prefix) and is handed to the rule engine under the same name. A rule picks
it up with `mqtt('modbus/<name>')`, and that works **whether or not a broker is
configured** — the value does not travel through one:

```js
rule('shed load')
  .when(mqtt('modbus/grid_power').is(function (w) { return Number(w) > 8000; }))
  .then(function () { output(0).off(); });
```

### When it goes wrong

Published values carry ten significant digits, which is exactly what a 32-bit
counter needs; the status API and the published value always agree.

A device that does not answer is retried with a widening gap — 2 s, then 4, 8,
up to a minute — rather than at its poll interval, so a switched-off device does
not dial continuously. The first failure is logged, repeats are not. The table in
the web UI shows each row's last value and age, or the reason it has none, which
is the only way to tell a wrong register from a device that is not there.
`GET /api/modbus/status` returns the same, along with the TCP server's counters.

Connections are held open between polls and dropped after a minute idle.

---

## Register Map

### Coil Registers (function codes 01 / 05 / 15) — Digital Outputs

| Coil | Address (0-based) | Modbus notation | Maps to |
|------|-------------------|-----------------|---------|
| 1    | 0                 | 00001           | DO1     |
| 2    | 1                 | 00002           | DO2     |
| …    |                   |                 |         |
| 8    | 7                 | 00008           | DO8     |

- **Read (FC 01):** returns current logical output state
- **Write (FC 05 / 15):** sets output state; the invert flag configured in the web UI is applied (same as MQTT)

---

### Discrete Input Registers (function code 02) — Digital Inputs

| DI | Address (0-based) | Modbus notation | Maps to |
|----|-------------------|-----------------|---------|
| 1  | 0                 | 10001           | DI1     |
| 2  | 1                 | 10002           | DI2     |
| …  |                   |                 |         |
| 8  | 7                 | 10008           | DI8     |

- **Read (FC 02):** returns current logical input state (invert flag applied)
- Refreshed from hardware every 10 ms

---

### Holding Registers (function codes 03 / 06 / 16)

#### HR 40001 — LED Colour (RGB252)

16-bit register encoding a colour in RGB252 format:

```
Bit 15–14  R  (2 bits, 0–3  → scales to 0 / 85 / 170 / 255)
Bit 13–9   G  (5 bits, 0–31 → scales to 0–255 via 5-bit expansion)
Bit 8–7    B  (2 bits, 0–3  → scales to 0 / 85 / 170 / 255)
Bit 6–0    unused (write as 0)
```

Writing sets the LED colour immediately. No sequence support via Modbus.

**Examples:**

| Colour | R | G  | B | Register value |
|--------|---|----|---|----------------|
| Red    | 3 | 0  | 0 | `0xC000`       |
| Green  | 0 | 31 | 0 | `0x3E00`       |
| Blue   | 0 | 0  | 3 | `0x0180`       |
| White  | 3 | 31 | 3 | `0xFF80`       |
| Off    | 0 | 0  | 0 | `0x0000`       |

#### HR 40002 — Buzzer Frequency

| Value     | Effect                                 |
|-----------|----------------------------------------|
| 0         | No action                              |
| 100–10000 | Triggers a 200 ms beep at the given Hz |

Writing a non-zero value triggers a single 200 ms beep. No sequence support via Modbus.

### Input Registers (function code 04) — Device Identification

Read-only. What the device is, which one of several it is, which firmware
it runs and how it is reachable. Addresses are zero-based; the Modbus
notation is 30001 + address. Numbers are big-endian; 32-bit values occupy
two registers, high word first. Text is two ASCII characters per register,
first character in the high byte, NUL padded. The whole block is 80
registers and fits one request. Served over TCP and, as an RTU slave, over
RS-485.

| Address | Size | Content |
|---|---|---|
| 0 | 1 | Device type: 1 = Waveshare-ESP32-S3-POE-ETH-8DI-8DO |
| 1–3 | 3 | Firmware version major, minor, patch |
| 4–6 | 3 | MAC address, six bytes — the chip's identity from the eFuse |
| 7–8 | 2 | Serial number as a 32-bit number: the last four bytes of the MAC |
| 9–10 | 2 | Uptime in seconds |
| 11–12 | 2 | IP address, four bytes in order (`192.168.1.10` reads as `0xC0A8 0x010A`) |
| 13–14 | 2 | Netmask |
| 15–16 | 2 | Gateway |
| 17 | 1 | 1 = address obtained by DHCP, 0 = static |
| 18–19 | 2 | Reserved, read as zero |
| 20–39 | 20 | Model, text: `Waveshare-ESP32-S3-POE-ETH-8DI-8DO` |
| 40–55 | 16 | Device name as configured in the web UI, text |
| 56–63 | 8 | Serial number as text: the MAC as twelve hex digits, no separators |
| 64–79 | 16 | Firmware version as text, as git describes it (`v1.1.13-3-gabc1234`) |

A read that reaches past address 79 is answered with exception `0x02`.

The serial number is the MAC because the ESP32-S3 has no serial number of
its own: the MAC is unique per chip, burned at the factory, and survives
every flash and every factory reset. The Ethernet interface uses a MAC
derived from it (base + 3), which is what shows up in a DHCP lease table.

### Read Device Identification (function code 43, MEI type 14)

The standard's own way of asking a device what it is, the one tools such as
`mbpoll`, `modpoll` and most SCADA drivers try first. Conformity level
`0x83`: basic, regular and extended objects, stream and individual access.
Answered over TCP for the board's own unit ID and over RS-485 as an RTU
slave.

| Object | Category | Content |
|---|---|---|
| `0x00` VendorName | basic | `Waveshare` |
| `0x01` ProductCode | basic | `Waveshare-ESP32-S3-POE-ETH-8DI-8DO` |
| `0x02` MajorMinorRevision | basic | firmware version, as git describes it |
| `0x03` VendorUrl | regular | the project's repository |
| `0x04` ProductName | regular | `Waveshare-ESP32-S3-POE-ETH-8DI-8DO` |
| `0x05` ModelName | regular | `ESP32-S3-POE-ETH-8DI-8DO` |
| `0x06` UserApplicationName | regular | the device name from the web UI |
| `0x80` | extended | serial number, twelve hex digits |
| `0x81` | extended | MAC, `AA:BB:CC:DD:EE:FF` |

Example, the basic objects in one request:

```
>> 2B 0E 01 00
<< 2B 0E 01 83 00 00 03  00 09 "Waveshare"  01 22 "Waveshare-ESP32-S3-POE-ETH-8DI-8DO"  02 07 "v1.1.13"
```

A request for another unit over TCP is **not relayed** to RS-485. The
stack's serial master can send a custom function code, but only with a
payload of even length and without handing the response back to the
caller, and the FC 43 request is three bytes; relaying it needs an addition
to the stack. Until then the client gets exception `0x01`, as it would
from a device without the function.

---

## Notes

- Register values are big-endian (standard Modbus).
- The Modbus stack and MQTT/Ethernet operate concurrently. A coil write via Modbus will also publish the new DO state on
  the corresponding MQTT topic.
- Modbus is disabled by default. Enable in the web UI and reboot to activate.
- The local unit ID must stay 247 while RS-485 is in master mode: a lower one
  would shadow the device that really has that address on the segment.
