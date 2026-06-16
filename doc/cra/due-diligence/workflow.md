# Due Diligence Workflow

This document describes the Due Diligence process for third-party dependencies as required by the EU Cyber Resilience
Act (CRA).

## Approach

Due Diligence is tracked in a **manually-maintained document** in the repository:

- **Document:** `doc/cra/due-diligence/due_diligence.md`
- **Vulnerability monitoring:** Dependabot Alerts (continuous) + OSV.dev (during reviews)

The document is always visible and reviewable in the repository. Changes are tracked in Git history. Manual fields
(Funktion/Begründung) are preserved across version updates.

## Dependency Sources

This firmware project draws dependencies from three sources:

| Source | Examples | Declared in |
|--------|----------|-------------|
| **IDF Component Registry** | espressif/mqtt, espressif/w5500 | `main/idf_component.yml` |
| **Git submodule (pinned)** | espressif/esp-idf | `.gitmodules` |
| **Vendored (in-tree)** | esp32-wifi-bootstrap | `components/` |
| **GitHub Actions** | actions/checkout, softprops/action-gh-release | `.github/workflows/*.yml` |

## Risk Scoring

Each dependency receives a risk score (0–100), calculated during initial assessment or at review time.

### Maintenance Status

| Factor                                  | Score Impact |
|-----------------------------------------|--------------|
| Stale package (>12 months since update) | +25          |
| Vendored and upstream unmaintained      | +20          |
| Not on latest available version         | +10          |
| Source unavailable / private registry   | +35          |

### Vulnerabilities (OSV.dev)

| Severity | Score per Finding |
|----------|-------------------|
| CRITICAL | +40               |
| HIGH     | +20               |
| MEDIUM   | +5                |
| LOW      | +1                |

### License Risk

| Factor                             | Score Impact |
|------------------------------------|--------------|
| Unknown / NOASSERTION license      | +15          |
| Copyleft license (GPL, AGPL, SSPL) | +10          |

### Risk Levels

| Level    | Score Range | Action Required                         |
|----------|-------------|-----------------------------------------|
| CRITICAL | 70–100      | Immediate review, maintainer approval   |
| HIGH     | 50–69       | Review before merge, document rationale |
| MEDIUM   | 30–49       | Tracked by periodic risk scan           |
| LOW      | 0–29        | Standard monitoring                     |

Score is capped at 100.

## Required Fields

For each dependency these fields are mandatory:

1. **Lizenz** — SPDX license identifier
2. **Funktion / Begründung** — What it does AND why it was chosen
3. **Verantwortlich** — Person or role responsible for monitoring

For HIGH and CRITICAL risk dependencies, additionally:

4. **Bekannte Vulns** — Explicit review result, e.g. `Reviewed 2026-06-16: None` — plain `None` without a dated review
   is not sufficient.

## Workflow Steps

### Adding a New Dependency

1. Add component to `main/idf_component.yml` (or `components/` for vendored).
2. Run `idf.py build` to resolve the pinned version — check `managed_components/<name>/idf_component.yml`.
3. Add a row to `doc/cra/due-diligence/due_diligence.md`.
4. Query OSV.dev for known vulnerabilities: `https://osv.dev/list?ecosystem=&q=<package-name>`.
5. Include the updated DD document in the same PR as the dependency change.

### Updating a Dependency Version

1. Update the version constraint in `main/idf_component.yml` and rebuild to resolve.
2. Update the **Version** and **Latest Version** columns in the DD table.
3. Re-check OSV.dev; update **Bekannte Vulns** and **Score** if needed.

### Removing a Dependency

1. Remove from `main/idf_component.yml` or `components/`.
2. Remove the corresponding row from the DD table in the same PR.

## Vulnerability Monitoring

| Layer | Trigger | Scope | Purpose |
|-------|---------|-------|---------|
| **Dependabot Alerts** | Continuous (new CVE) | All declared deps | Real-time CVE detection |
| **Dependabot version updates** | Weekly/monthly | GitHub Actions, ecosystem deps | Keep deps on latest patched versions |
| **Manual review** | On each dependency change | Changed dep | DD table completeness |
