# Modbus RTU API

Target device: Waveshare ESP32-S3-POE-ETH-8DI-8DO

## Connection

| Parameter          | Details                                   |
|--------------------|-------------------------------------------|
| Physical interface | RS-485 half-duplex (GPIO17 TX, GPIO18 RX) |
| Protocol           | Modbus RTU                                |
| Data bits          | 8                                         |
| Parity             | None                                      |
| Stop bits          | 1                                         |

Slave address and baudrate are configured in the web UI (**Modbus RTU** section). Changes take effect after reboot.

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

---

## Notes

- Register values are big-endian (standard Modbus).
- The Modbus stack and MQTT/Ethernet operate concurrently. A coil write via Modbus will also publish the new DO state on
  the corresponding MQTT topic.
- Modbus is disabled by default. Enable in web UI and reboot to activate.
