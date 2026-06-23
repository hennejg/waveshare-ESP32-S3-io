# Time source (SNTP / RTC / local-time cron) — status

**Implemented.** The device now has a real wall-clock and cron runs in local time.
This note is kept because [RULES.md](RULES.md) links here; it records what landed and the
residual caveats.

## What landed

- **SNTP** (PR #14): enabled by default, server configurable on the web *Time* page; each
  sync mirrors to the RTC. **RTC** (PCF85063, PR #14): seeds the system clock at boot,
  kept in sync by SNTP, settable from the UI. NTP takes precedence over the RTC.
- **Timezone** (this branch): a POSIX `TZ` string in the config (web *Time* page; UTC when
  empty), applied via `setenv("TZ")` + `tzset()` at boot and on change. Because QuickJS
  derives its local `Date` methods from newlib `localtime`, this also makes cron local.
- **Cron is local time**: `_cronNext`/`_cronDayOk` use the local `Date` getters/setters,
  so the JS engine applies the TZ + DST when mapping local fields ↔ epoch.
- **Suppress until valid**: cron stays unarmed until the clock is real (`_time_valid()` —
  RTC seed at boot or first SNTP sync), so it can't fire boot-relative off a 1970 clock.
- **Re-arm on sync**: the SNTP callback calls `scripting_on_time_sync()` →
  `_on_time_sync()`, which marks the clock valid and cancels + re-arms every cron trigger
  from the corrected wall-clock (the fix for the monotonic-timer step). `every(ms)`,
  `.after(ms)`, `.heldFor(ms)` are relative and untouched.
- **`_now()` binding**: explicit `gettimeofday` epoch-ms source for cron, instead of
  relying on QuickJS `Date.now()`'s own wiring. `Date(ms)` is still used for local-field
  extraction (needs only `getTimezoneOffset` → `localtime`, which the embedded build
  patches for newlib).
- Tests: `test/cron.test.js` pins `TZ=UTC` for the UTC-anchored cases and adds
  local-offset, DST-skip, and suppress/re-arm tests; the harness gained `_time_valid` +
  `timeSync()`.

## Residual caveats / limitations

- **DST edges** (inherent to local-time cron): on spring-forward a wall-clock time in the
  skipped hour never occurs, so a job scheduled then is skipped that day; on fall-back a
  repeated wall-clock time fires once. Use UTC (empty TZ) for a fixed cadence across DST.
- **TZ list maintenance** (CRA-relevant): the UI offers a curated set of POSIX `TZ` strings
  with DST rules baked in. If a jurisdiction changes its DST rules, the string (or the
  curated list) must be updated; there is no IANA tz database on-device. A custom POSIX
  string is always available as an escape hatch.
- **NTP is unauthenticated** (no NTS): a LAN MITM could spoof time and make cron act at the
  wrong moment. Don't gate safety-/security-critical actions on cron alone — combine with
  an input/interlock and treat time as advisory.
- **On-hardware check still worth doing**: confirm `_now()` advances and local `Date`
  getters reflect the configured TZ on the device (the test harness mocks `_now`, and the
  simulator uses the browser clock).
