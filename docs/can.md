# CAN Bus API

Target device: Waveshare ESP32-S3-POE-ETH-8DI-8DO  
Interface: ESP32-S3 TWAI peripheral — GPIO2 (TX), GPIO3 (RX)

## Configuration

Set in the web UI (CAN Bus section). Changes take effect after reboot.

| Field       | Description                                             | Default    |
|-------------|---------------------------------------------------------|------------|
| Enable      | On/off                                                  | off        |
| Base CAN ID | 11-bit base address (0x000–0x7FF)                       | `0x100`    |
| Bit rate    | 125 / 250 / 500 / 1000 kbit/s                           | 250 kbit/s |
| TX interval | Periodic DI transmit interval in ms; 0 = on-change only | 1000 ms    |

250 kbit/s is the standard bit rate for NMEA2000.

---

## Frame Map

All frames use **11-bit standard CAN**, CAN 2.0B.

| CAN ID     | Dir     | DLC | Content                                                      |
|------------|---------|-----|--------------------------------------------------------------|
| `base + 0` | TX      | 1   | **Status** — `0x01` = alive, `0x03` = MQTT broker connected  |
| `base + 1` | TX      | 1   | **Digital inputs** — bit 0 = DI1 … bit 7 = DI8               |
| `base + 2` | RX + TX | 2   | **Digital outputs** — see below                              |
| `base + 3` | RX      | 3   | **LED** — `[R][G][B]` (IO mode only; ignored in Status mode) |
| `base + 4` | RX      | 2–3 | **Buzzer** — `[freqLo][freqHi][dur×10ms]`                    |
| `base + 5` | RX      | 0   | **Read request** — triggers TX of IDs 0–2                    |

---

### base+1 — Digital inputs (TX)

Single byte, bit-packed. Sent:

- On any input state change (within 50 ms polling latency)
- Periodically at the configured TX interval (if > 0)
- On read request (base+5)

Invert flags configured in the web UI are applied before transmit.

---

### base+2 — Digital outputs (RX + TX)

**Receive (command):** 2 bytes

| Byte | Field   | Description                                       |
|------|---------|---------------------------------------------------|
| 0    | Opcode  | `0` = WRITE, `1` = SET, `2` = CLEAR, `3` = TOGGLE |
| 1    | Bitmask | bit 0 = DO1 … bit 7 = DO8                         |

Opcode semantics:

| Opcode     | Operation        | Effect                                            |
|------------|------------------|---------------------------------------------------|
| `0` WRITE  | `state = mask`   | Set all outputs to exact bitmask                  |
| `1` SET    | `state \|= mask` | Turn on selected outputs, leave others unchanged  |
| `2` CLEAR  | `state &= ~mask` | Turn off selected outputs, leave others unchanged |
| `3` TOGGLE | `state ^= mask`  | Flip selected outputs, leave others unchanged     |

A single-bit mask gives individual output control: `SET 0x04` turns on DO3 only.

**Transmit (echo):** Sent after every successful command. Format: opcode `0x00` (WRITE) + current full state bitmask, so
any receiver can reconstruct the complete output state from a single frame.

Invert flags are applied to the hardware write but the echo carries the logical state.

---

### base+3 — LED (RX)

Three bytes: `[R][G][B]`, 0–255 each. Sets the LED colour immediately.

- Ignored when LED is in **Status feedback** mode.
- Does not support sequences or durations; use MQTT for those.

---

### base+4 — Buzzer (RX)

| Byte | Field                 | Description                                                  |
|------|-----------------------|--------------------------------------------------------------|
| 0    | freq low byte         | Frequency in Hz, little-endian                               |
| 1    | freq high byte        |                                                              |
| 2    | duration *(optional)* | Duration in units of 10 ms. Omit (DLC=2) for 200 ms default. |

Examples: `0xE8 0x03 0x0A` = 1000 Hz for 100 ms. `0x50 0x06` = 1616 Hz for 200 ms.

---

### base+5 — Read request (RX)

Send a frame with DLC = 0 to request an immediate TX of frames base+0, base+1, and base+2.

---

## Interaction with MQTT and Modbus

- **DO writes via CAN** call the same `dout_set()` path as MQTT, so changes are automatically published on the
  corresponding MQTT `output/N` topic.
- **DI state** is read from the same hardware state used by MQTT and Modbus.
- All three interfaces (MQTT, Modbus, CAN) are active simultaneously if configured.

---

## NMEA2000 roadmap

The current frame map uses 11-bit standard frames for simplicity. The migration path to NMEA2000:

| Current                | NMEA2000 equivalent                    |
|------------------------|----------------------------------------|
| base+1 (DI)            | PGN 127501 — Binary Switch Bank Status |
| base+2 (DO)            | PGN 127502 — Switch Bank Control       |
| base+0 (status)        | PGN 126993 — Heartbeat                 |
| base+3/4 (LED, buzzer) | PGN 126720 — Manufacturer Proprietary  |

This requires switching to 29-bit extended frames and implementing NMEA2000 address claiming.
