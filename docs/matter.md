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

Endpoint numbering follows the order of creation: root endpoint is 0, DO1–DO8 are endpoints 1–8, DI1–DI8 are
endpoints 9–16.

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
