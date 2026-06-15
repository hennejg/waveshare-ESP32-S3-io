# CAN Bus API

Target device: Waveshare ESP32-S3-POE-ETH-8DI-8DO  
Interface: ESP32-S3 TWAI peripheral — GPIO2 (TX), GPIO3 (RX)

## Operation Modes

Three modes are available, selected in the web UI (CAN Bus section). Changes take effect after reboot.

| Mode | Frame format | Bit rate | Description |
|------|-------------|----------|-------------|
| **Off** | — | — | CAN peripheral disabled |
| **Basic** | 11-bit standard | Configurable | Simple custom frame map |
| **NMEA2000** | 29-bit extended | 250 kbit/s (fixed) | Standard marine network |

---

## Common Configuration

| Field | Applies to | Description | Default |
|-------|-----------|-------------|---------|
| TX interval | Both | Periodic DI transmit in ms; 0 = on-change only | 1000 ms |

---

## Basic Mode

### Configuration

| Field | Description | Default |
|-------|-------------|---------|
| Base CAN ID | 11-bit base address (0x000–0x7FF) | `0x100` |
| Bit rate | 125 / 250 / 500 / 1000 kbit/s | 250 kbit/s |

### Frame Map

| CAN ID | Dir | DLC | Content |
|--------|-----|-----|---------|
| `base + 0` | TX | 1 | **Status** — `0x01` = alive, `0x03` = MQTT broker connected |
| `base + 1` | TX | 1 | **Digital inputs** — bit 0 = DI1 … bit 7 = DI8 |
| `base + 2` | RX + TX | 2 | **Digital outputs** — see below |
| `base + 3` | RX | 3 | **LED** — `[R][G][B]` (IO mode only) |
| `base + 4` | RX | 2–3 | **Buzzer** — `[freqLo][freqHi][dur×10ms]` |
| `base + 5` | RX | 0 | **Read request** — triggers TX of IDs 0–2 |

**base+1 — Digital inputs (TX):** single byte, bit-packed. Sent on change (≤ 50 ms latency), periodically at TX interval, and on read request. Invert flags applied.

**base+2 — Digital outputs (RX + TX):**

Two bytes: `[opcode][bitmask]` on receive; `[0x00][state]` echo on transmit.

| Opcode | Operation | Effect |
|--------|-----------|--------|
| `0` WRITE | `state = mask` | Set all outputs to exact bitmask |
| `1` SET | `state \|= mask` | Turn on selected, leave others unchanged |
| `2` CLEAR | `state &= ~mask` | Turn off selected, leave others unchanged |
| `3` TOGGLE | `state ^= mask` | Flip selected, leave others unchanged |

A single-bit mask gives individual output control: `SET 0x04` turns on DO3 only.

**base+3 — LED:** `[R][G][B]`, 0–255. Ignored when LED is in Status mode.

**base+4 — Buzzer:** `[freqLo][freqHi][dur×10ms]`. DLC=2 uses 200 ms default.

**base+5 — Read request:** DLC=0 triggers immediate TX of frames base+0 to base+2.

---

## NMEA2000 Mode

Implements a subset of NMEA2000 sufficient for an 8-channel I/O gateway.

### Configuration

| Field | Description | Default |
|-------|-------------|---------|
| Source address | Preferred node address (1–251) | 80 (`0x50`) |

Bit rate is always 250 kbit/s (NMEA2000 standard).

### Device Identity

| Parameter | Value |
|-----------|-------|
| Industry group | 4 — Marine |
| Device class | 0x19 — Sensor Bus |
| Device function | 0x80 — I/O Gateway |
| Manufacturer code | 0x7FF (development / unregistered) |
| Identity number | Derived from ESP32 base MAC |
| Arbitrary address capable | Yes |

### Address Claiming (PGN 60928)

On startup the device transmits an ISO Address Claim for its preferred source address. After a 250 ms contention window with no challenge, the address is considered claimed.

If another node claims the same address with a numerically lower NAME, the device automatically tries the next address, repeating until it succeeds (up to address 251).

### PGN Summary

| PGN | Direction | Description |
|-----|-----------|-------------|
| 60928 | TX | ISO Address Claim — on startup and in response to ISO Request |
| 59904 | RX | ISO Request — device responds with claim or bank status |
| 126993 | TX | Heartbeat — fast-packet, 1 s interval |
| 127501 | TX | Binary Switch Bank Status — DI bank (inst. 0) and DO bank (inst. 1) |
| 127502 | RX | Switch Bank Control — commands DO bank (inst. 1) |
| 126720 | RX + TX | Manufacturer Proprietary Fast-Packet — LED and buzzer |

### PGN 127501 — Binary Switch Bank Status

Transmitted on every DI/DO state change, at the configured TX interval, and on ISO Request. Byte 0 = bank instance; remaining bytes = 2 bits per switch (LSB first within each byte).

| Value | Meaning |
|-------|---------|
| `0b00` | Off |
| `0b01` | On |
| `0b10` | Error |
| `0b11` | Not available |

Switches 9–28 are transmitted as `0b11` (not available). DI states are on bank instance 0; DO states on bank instance 1.

### PGN 127502 — Switch Bank Control

Addressed to bank instance 1 (digital outputs). Per-switch 2-bit control:

| Value | Effect |
|-------|--------|
| `0b00` | Set output Off |
| `0b01` | Set output On |
| `0b10` or `0b11` | No change |

Individual output control: set one switch to `0b01`/`0b00` and all others to `0b11`.

### PGN 126720 — Manufacturer Proprietary Fast-Packet

Manufacturer code `0x7FF` (bytes 0–1 of payload). Sub-function byte at offset 2:

| Sub-fn | Direction | Payload after sub-fn | Description |
|--------|-----------|---------------------|-------------|
| `0x01` | RX + TX | `[R][G][B]` | LED colour (IO mode only) |
| `0x02` | RX | `[freqLo][freqHi][dur×10ms]` | Buzzer beep |

Both fit in a single fast-packet frame (6 payload bytes in first frame).

### Interaction with MQTT and Modbus

- **DO writes via PGN 127502** call the same `dout_set()` path as MQTT and Modbus, so the new state is automatically published on `output/N` MQTT topics.
- **DI state** (PGN 127501) is read from the same hardware state used by all three interfaces.
- MQTT, Modbus, and CAN (either mode) operate simultaneously if all are configured.

---

## Cross-mode coexistence

Both basic and N2k frames coexist on the same physical bus — basic uses 11-bit standard frames (`ide=0`), NMEA2000 uses 29-bit extended frames (`ide=1`). The firmware dispatches received frames by frame type, so a CAN bus can carry both protocols simultaneously if needed.
