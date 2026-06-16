# Due Diligence — Third-Party Dependencies

> This document is maintained manually. Whenever a dependency changes in `main/idf_component.yml`,
> `.gitmodules`, `components/`, or `.github/workflows/*.yml`, this document must be updated accordingly.
>
> Last review: 2026-06-16

## Legend

- **PURL**: Package URL — machine-readable identifier (SPDX/CycloneDX standard)
- **Funktion / Begründung**: What the component does AND why it was chosen
- **Herkunft**: IDF Component Registry, Git submodule, vendored in-tree, or GitHub Actions
- **Maintainer**: Who maintains the component (supply chain risk)
- **Bekannte Vulns**: Snapshot of known vulnerabilities at review time (OSV.dev)
- **Risiko**: LOW / MEDIUM / HIGH / CRITICAL (see `workflow.md` for scoring formula)
- **Stale**: `true` if >12 months since last upstream release/commit, `false` otherwise
- **Outdated**: `true` if the pinned version is not the latest available, `false` otherwise
- **Score**: Numeric risk score (0–100), see `workflow.md`
- **Verantwortlich**: Person/role responsible for monitoring this dependency

## Firmware Dependencies

| Component | Version | PURL | Lizenz | Funktion / Begründung | Herkunft | Maintainer | Last Update | Bekannte Vulns | Alternative | Risiko | Stale | Outdated | Score | Verantwortlich |
|-----------|---------|------|--------|-----------------------|----------|-----------|------------|----------------|-------------|--------|-------|----------|-------|----------------|
| espressif/esp-idf | v6.0.1 | pkg:github/espressif/esp-idf@v6.0.1 | Apache-2.0 | ESP-IDF framework — RTOS, peripheral drivers, networking, build system; chosen as the only supported SDK for the ESP32-S3 | Git submodule (pinned) | Espressif Systems | 2026-04 | Reviewed 2026-06-16: None | None (sole supported SDK) | LOW | false | false | 0 | Jörg Henne |
| espressif/mqtt | 1.0.0 | pkg:idf/espressif/mqtt@1.0.0 | Apache-2.0 | MQTT client — publishes DI/DO state and subscribes to control topics; chosen for native IDF integration and PAHO lineage | IDF Component Registry | Espressif Systems | 2026-04 | Reviewed 2026-06-16: None | Eclipse Paho C (heavier) | LOW | false | false | 0 | Jörg Henne |
| espressif/led_strip | 3.0.3 | pkg:idf/espressif/led_strip@3.0.3 | Apache-2.0 | WS2812 RMT driver — controls the on-board NeoPixel LED; the only IDF-native driver with RMT-v2 support | IDF Component Registry | Espressif Systems | 2026-04 | Reviewed 2026-06-16: None | Bit-bang implementation | LOW | false | false | 0 | Jörg Henne |
| espressif/cjson | 1.7.19~2 | pkg:idf/espressif/cjson@1.7.19~2 | MIT | JSON parsing/generation — REST API request and response bodies; chosen over nlohmann because this is C, not C++ | IDF Component Registry | Espressif (wrapping DaveGamble/cJSON) | 2026-04 | Reviewed 2026-06-16: None | Jansson, jsmn | LOW | false | false | 0 | Jörg Henne |
| espressif/w5500 | 1.0.1 | pkg:idf/espressif/w5500@1.0.1 | Apache-2.0 | WIZnet W5500 SPI Ethernet PHY driver — required for on-board Ethernet; W5500 removed from IDF core in v5+, this component restores it | IDF Component Registry | Espressif Systems | 2026-04 | Reviewed 2026-06-16: None | None (hardware-specific) | LOW | false | false | 0 | Jörg Henne |
| espressif/esp-modbus | 2.1.2 | pkg:idf/espressif/esp-modbus@2.1.2 | Apache-2.0 | Modbus RTU slave — exposes DI/DO state and LED/buzzer control over RS-485; chosen as the reference Modbus implementation for IDF | IDF Component Registry | Espressif Systems | 2026-04 | Reviewed 2026-06-16: None | FreeModbus directly | LOW | false | false | 0 | Jörg Henne |
| esp32-wifi-bootstrap | n/a (vendored) | pkg:github/AchimPieters/esp32-wifi-bootstrap | MIT | WiFi captive-portal provisioning — opens a setup AP on first boot so users can enter WiFi credentials; chosen for its small footprint and captive-portal DNS intercept | Vendored in `components/` (patched from upstream) | AchimPieters (community) | 2025-07 | Reviewed 2026-06-16: None | WiFiProvisioner, SmartConfig | LOW | false | false | 0 | Jörg Henne |

### Notes on vendored component

`esp32-wifi-bootstrap` is maintained in-tree at `components/esp32-wifi-bootstrap/` rather than as a submodule.
Three patches were applied to the upstream v1.0.1 source:

1. Removed hardcoded `SO_BINDTODEVICE "en1"` (macOS interface name invalid on ESP-IDF — caused DNS failures).
2. Increased DNS receive buffer 96 → 512 bytes (Android 8+ appends EDNS OPT records; old limit dropped queries).
3. Added `/eth-only` HTTP endpoint + callback for the "Use Ethernet only" provisioning option.

The upstream project is a small community component. We own and control the in-tree copy, so upstream inactivity does
not introduce supply-chain risk for the running code; it only means patches must be applied manually if the upstream
is ever updated.

## CI/CD Dependencies (GitHub Actions)

| Component | Version | PURL | Lizenz | Funktion / Begründung | Herkunft | Maintainer | Last Update | Bekannte Vulns | Risiko | Stale | Outdated | Score | Verantwortlich |
|-----------|---------|------|--------|-----------------------|----------|-----------|------------|----------------|--------|-------|----------|-------|----------------|
| actions/checkout | v6 | pkg:github/actions/checkout@v6 | MIT | Checks out repository source in CI/CD runners | GitHub Actions Marketplace | GitHub (Microsoft) | 2026-05 | Reviewed 2026-06-16: None | None | LOW | false | false | 0 | Jörg Henne |
| actions/cache | v5 | pkg:github/actions/cache@v5 | MIT | Caches `managed_components/` between runs to reduce build time | GitHub Actions Marketplace | GitHub (Microsoft) | 2026-04 | Reviewed 2026-06-16: None | None | LOW | false | false | 0 | Jörg Henne |
| actions/upload-artifact | v7 | pkg:github/actions/upload-artifact@v7 | MIT | Uploads firmware build artifacts from CI runs | GitHub Actions Marketplace | GitHub (Microsoft) | 2026-05 | Reviewed 2026-06-16: None | None | LOW | false | false | 0 | Jörg Henne |
| softprops/action-gh-release | v3 | pkg:github/softprops/action-gh-release@v3 | MIT | Creates GitHub releases and attaches firmware binaries on version tags | GitHub Actions Marketplace | softprops (James Landrum) | 2026-05 | Reviewed 2026-06-16: None | GitHub CLI `gh release create` | LOW | false | false | 0 | Jörg Henne |
