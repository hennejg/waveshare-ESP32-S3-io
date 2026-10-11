# Matter Integration

The firmware exposes the 8 digital outputs and 8 digital inputs as Matter endpoints over Wi-Fi, making the device
controllable from Apple Home, Google Home, Home Assistant (Matter integration), and any other Matter-certified
controller.

---

## Device model

| Endpoints | Count | Matter device type          | Cluster              | Direction  |
|-----------|-------|-----------------------------|----------------------|------------|
| 1–8       | 8     | On/Off Plug-in Unit (0x010A) | OnOff (0x0006)       | Read/Write |
| 9–16      | 8     | Contact Sensor (0x0015)      | BooleanState (0x0045)| Read-only  |
| 17…       | 0–3   | by pad mode, see below       |                      |            |
| …         | 0–8   | by Modbus row, see below     |                      | Read-only  |

Endpoint numbering follows the order of creation: root endpoint is 0, DO1–DO8 are endpoints 1–8, DI1–DI8 are
endpoints 9–16, then one endpoint per header pad that has one, in pad order, then one per Modbus sensor row, in
row order. `CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT` (28) holds all of them at once; `matter_mbm.cpp`
checks that at compile time.

### Modbus values (RS-485 master)

With RS-485 set to master in the **Modbus** tab, the board reads up to eight values from devices on the RS-485
segment -- the table described in `docs/modbus.md` (*Reading other Modbus devices*), RS-485 rows only. Each row
with a Matter type becomes a sensor endpoint; the scale has to bring the value into the unit given here:

| Matter type | Matter device type                | Attribute                              | Scale to | Reported as      |
|-------------|-----------------------------------|----------------------------------------|----------|------------------|
| temperature | Temperature Sensor (0x0302)       | MeasuredValue                          | °C       | 0.01 °C          |
| humidity    | Humidity Sensor (0x0307)          | MeasuredValue                          | %        | 0.01 %           |
| pressure    | Pressure Sensor (0x0305)          | MeasuredValue                          | hPa      | 1 hPa (0.1 kPa)  |
| flow        | Flow Sensor (0x0306)              | MeasuredValue                          | m³/h     | 0.1 m³/h         |
| power       | Electrical Sensor (0x0510)        | ElectricalPowerMeasurement.ActivePower | W        | mW               |
| energy      | Electrical Sensor (0x0510)        | ElectricalEnergyMeasurement.CumulativeEnergyImported | kWh (meter reading) | mWh |

A value outside what the attribute can hold (e.g. a temperature above 327.67 °C, a negative humidity) is not
reported rather than clipped; the attribute keeps its last value. A row without a Matter type is read and shown
in the web UI only.

Register, scale and interval apply on save; a different Matter type, or a row switched on or off, changes the
endpoint list and answers `reboot_required`. The full firmware keeps the Matter type of each row when it saves
the table, so switching firmware does not lose it.

Electrical power measurement requires an accuracy entry. The board does not know the meter's accuracy and
reports ±0 over the full range -- a placeholder, not a statement about the meter. Whether a certification test
accepts that has not been checked.

Verified on the bench (Eastron SDM630 twin on RS-485, chip-tool): all six types read back on endpoints 17–22,
each within 1 % of the value the poll table shows at the same moment (the twin's values drift between reads), a
scale change reported without reboot.

### Header pads (IO1, RXD, TXD)

The three non-isolated 3.3 V pads are set up in the **Pads** tab of the web UI. Each becomes an endpoint by the
mode it has at boot:

| Pad mode  | Matter device type                 | Clusters                 | Notes                                      |
|-----------|------------------------------------|--------------------------|--------------------------------------------|
| input     | Contact Sensor (0x0015)            | BooleanState             | DI channel 8–10                            |
| output    | On/Off Plug-in Unit (0x010A)       | OnOff                    | starts off after every boot                |
| pwm       | Dimmable Plug-in Unit (0x010B)     | OnOff, LevelControl      | off = duty 0; level 1–254 = duty 0.4–100 % |
| analog    | —                                  | —                        | no device type home apps show; web UI only |
| standard  | —                                  | —                        | UART0 / unused                             |

A mode change switches the pad at once, its endpoint after a reboot (the save answers `reboot_required`). The
endpoint numbers of the pads after it shift when a pad gains or loses its endpoint; the controller sees the
endpoint list change. Whether a given home app copes with that without re-pairing has not been tested.

Every active pad endpoint costs internal RAM (1–2.6 KB measured), see *Memory and build notes*.

### Digital Outputs (DO1–DO8)

Controllers can read and toggle outputs. A write from a Matter controller (e.g. turning a plug on/off in Apple Home)
is applied via `dout_set()` and reflected immediately in MQTT, Modbus, and CAN.

The current output state is kept in sync via `matter_do_update()` so that manual changes via MQTT/Modbus/CAN are
visible to the Matter controller without requiring a separate read.

### Digital Inputs (DI1–DI8) — Contact Sensors

The Matter `contact_sensor` device type is used for digital inputs. It maps a boolean (open/closed) directly to
the `StateValue` attribute of the BooleanState cluster.

**Important — how this appears in home apps:**

Most home automation controllers render Contact Sensor endpoints as door or window sensors:

| Platform          | Appearance                                               |
|-------------------|----------------------------------------------------------|
| Apple Home / HomeKit | "Door" or "Window" sensor (open = contact lost)       |
| Home Assistant    | Binary sensor, device class `door` or `opening`         |
| Google Home       | Door/window sensor                                       |

This is correct behaviour per the Matter spec — Contact Sensor is the lightest-weight boolean sensor type in Matter
and the spec maps it to the `door` device class in most controller implementations. There is no generic "binary
sensor" device type in Matter 1.x.

If the door/window presentation is confusing for your use case, you can rename each endpoint in the home app after
pairing (e.g. call it "Bilge pump running" or "Shore power present"). The underlying data is just a boolean, so
renaming and adding appropriate icon/type in the home app works around the presentation.

---

## Commissioning

### Pairing codes

A unique passcode (6–8 digits) and 12-bit discriminator are generated using `esp_random()` on first boot and
stored in the `chip-factory` NVS namespace. They are **not** reset by decommission — the same code is valid
for re-pairing after decommission.

The codes are shown in the web UI under the **Matter** section (QR code + numeric manual pairing code) and
printed to the serial log at startup.

### Pairing procedure

1. Open the web UI (`http://<device-ip>/`) and scroll to the **Matter** section.
2. Scan the QR code with your home app, or enter the numeric manual pairing code.
3. The home app will find the device via Bluetooth LE (BLE is kept active for commissioning). Once commissioned,
   BLE advertising is no longer needed and the device operates over Wi-Fi.
4. All 16 endpoints (8 DO + 8 DI) appear in the home app. Assign rooms and names as appropriate.

### Endpoint naming

Matter does not propagate the per-channel names configured in the web UI to controllers — those names are only
used for MQTT topics and Modbus labels. Name the endpoints in the home app after pairing.

---

## Decommission (remove from home)

The **Remove from Home** button in the web UI (Matter section, visible only when commissioned) calls
`/api/matter/decommission`. This:

1. Removes all fabrics and sends Leave notifications to active controllers.
2. Erases the following NVS namespaces: `chip-config`, `chip-counters`, `CHIP_KVS`, `esp_matter_kvs`, `node`.
3. Reboots.

The `chip-factory` namespace (passcode/discriminator) is **intentionally preserved** so the same pairing code
remains valid for re-commissioning.

After reboot the device is ready to commission again with the same QR code.

---

## Identify

When any endpoint's Identify cluster is triggered (e.g. via "Identify" in a home app to locate the physical
device), the RGB LED blinks **cyan** at 1 Hz for the duration of the identify action. All 16 endpoints share
the same identify callback and produce the same LED behaviour.

---

## REST API

| Endpoint                   | Method | Auth | Description                                                  |
|----------------------------|--------|------|--------------------------------------------------------------|
| `/api/matter/pairing`      | GET    | Yes  | Returns `{"qr_code":"…","manual_code":"…","commissioned":bool}`. Returns 404 if Matter is not compiled in. |
| `/api/matter/decommission` | POST   | Yes  | Removes all fabrics, erases Matter NVS state, reboots.       |

---

## Memory and build notes

The firmware uses `CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC=y` (rather than the default
`CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y`) to allow mbedTLS BIGNUM/ECC operations to fall back to PSRAM when
internal DRAM is exhausted. This is required for reliable re-commissioning with 16 endpoints loaded —
P-256 key operations during the NOC chain verification (AddNOC step) need a large workspace that doesn't
fit in internal SRAM alongside the rest of the stack. BIGNUM/ECC operations are pure CPU; no DMA is
involved, so PSRAM is safe.

Similarly, `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_DEFAULT=y` allows NimBLE advertising buffers to use PSRAM
fallback during commissioning.

See `sdkconfig.defaults` for the full set of memory-related overrides.

### Internal RAM, measured

Internal RAM is the limit of this firmware. `GET /api/version` reports it (`heap_internal_free`,
`heap_internal_min` = lowest since boot, `heap_internal_largest`, `heap_dma_free`). Measured on the bench
board (Ethernet up, setup AP on, BLE advertising, not commissioned):

| Firmware                                             | free    | lowest  | largest block | lowest through commissioning |
|------------------------------------------------------|---------|---------|---------------|------------------------------|
| before time, buzzer and pads                         | 19.9 KB | 13.2 KB | 7.7 KB        | 12.4 KB                      |
| with time, buzzer and pads, all pads standard        | 16.7 KB | 10.1 KB | 7.7 KB        | 9.5 KB                       |
| same, three pads with endpoints (output, input, pwm) | 13.7 KB | 7.0 KB  | 5.4 KB        | 6.8 KB                       |

Commissioning was done on-network over Ethernet with chip-tool and succeeded in every case; afterwards the
setup AP and BLE are off and 56–73 KB are free. Commissioning over BLE with a phone was not measured.

These figures predate the two changes below, which together free about 78 KB of internal RAM:

- `main/linker.lf` places the static data (`.bss`) of `libCHIP.a` and `libesp_matter.a` in PSRAM -- 59.6 KB,
  as documented in esp-matter's FAQ ("Moving BSS Segments to PSRAM"). Needs
  `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`. None of this data is touched from an ISR, and atomics on PSRAM
  are covered by `CONFIG_STDATOMIC_S32C1I_SPIRAM_WORKAROUND`.
- `CONFIG_ESP_WIFI_IRAM_OPT` and `CONFIG_ESP_WIFI_RX_IRAM_OPT` off -- 18.1 KB of Wi-Fi code goes from IRAM to
  flash, at the cost of Wi-Fi throughput the setup portal and Matter control traffic do not need.

Measured after both changes (all pads standard, Modbus enabled):

| Phase                                                   | internal free | lowest   | DMA-capable lowest |
|---------------------------------------------------------|---------------|----------|--------------------|
| not commissioned, Modbus waiting                        | 84.0 KB       | 76.8 KB  | 76.2 KB            |
| through commissioning                                   | --            | 74.0 KB  | 66.2 KB            |
| commissioned, Modbus running                            | 109.4 KB      | 72.8 KB  | --                 |
| 4 Modbus TCP clients, 656 requests in 15 s              | --            | 110.5 KB | 102.8 KB           |
| second fabric commissioned with Modbus running          | --            | 121.5 KB | 113.6 KB           |

The RS-485 master adds 2.6 KB of static internal RAM (126.6 KB in all); its poll task runs on a PSRAM stack. With
six Modbus sensor endpoints, before commissioning: 73.3 KB free (lowest 66.7 KB, DMA-capable 65.5 KB); after
commissioning and polling: 119.7 KB free, 58.0 KB at the lowest.

### Modbus

Modbus (RTU slave or RS-485 master on RS-485, Modbus TCP server with 4 clients) is in this firmware but starts only
once the device is commissioned: at boot when it already is, otherwise 5 s after commissioning completes. Measured RAM in the full
firmware: RTU slave 19 KB internal, with the TCP server 25 KB -- more than the 9–10 KB left before and during
commissioning, well within the 50–70 KB free afterwards. Its tasks run on PSRAM stacks, the TCP connection table
lives in PSRAM, and `CONFIG_RINGBUF_PLACE_FUNCTIONS_INTO_FLASH` keeps the UART driver's ring buffer code out of
IRAM.

Measured with Modbus in the build (all pads standard, Modbus enabled), before the Matter stack's static data
moved to PSRAM (see above for the figures after):

| Phase                                                   | internal free (lowest) |
|---------------------------------------------------------|------------------------|
| not commissioned, Modbus waiting                        | 14.6 KB (7.5 KB)       |
| through commissioning                                   | 8.0 KB, DMA-capable 0.6 KB at the lowest |
| commissioned, Modbus running                            | 40–50 KB               |
| 4 Modbus TCP clients, 620 requests in 15 s              | 41.6 KB at the lowest  |
| second fabric commissioned with Modbus running          | 43.8 KB at the lowest  |

The Modbus code costs about 2 KB of internal RAM before commissioning even while Modbus is not running (static
data and the ring buffer's ISR functions in IRAM).

The buzzer and pad tasks run on PSRAM stacks to keep this low. Endpoint data is allocated in small blocks,
which `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` (1024) keeps in internal RAM.

### Known quirks

- **16 Identify entries in home apps** — Identify is a mandatory cluster on both `on_off_plug_in_unit` and
  `contact_sensor`. All 16 endpoints therefore appear as separate "identify" targets in home apps. This is
  a Matter spec requirement and cannot be reduced.

- **Contact sensors appear as doors/windows** — see the [Digital Inputs](#digital-inputs-di1di8--contact-sensors)
  section above.

- **BLE stays active until first commissioning** — the device advertises via BLE until a controller completes
  the commissioning flow. After that BLE is idle but the NimBLE stack remains initialised (required by the
  esp-matter SDK). This has no practical impact on Wi-Fi throughput.

- **Re-commissioning after decommission** — the web UI polling loop (3 s initial + 1.5 s intervals) handles
  the reboot cleanly and reloads the page when the device is back online.
