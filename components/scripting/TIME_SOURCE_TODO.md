# Time source (SNTP / RTC) — follow-up notes

The rule engine has cron triggers (`cron(...)`), but the device has **no real
wall-clock yet**: no SNTP, and the hardware RTC is unused. Today the system clock starts
at the Unix epoch (1970-01-01) on every reboot and counts up, and `cron` is evaluated
against that boot-relative time in **UTC** (see `cron(...)` in [RULES.md](RULES.md)).
This note collects what to keep in mind when wiring up real time, so cron becomes
correct without surprises.

## Firmware (the actual time source)

- **SNTP**: initialise (`esp_netif_sntp_init` / `esp_sntp`) after the network is up
  (Ethernet or WiFi). Pick NTP server(s); consider making them configurable in the web UI.
  Matter can also supply time — decide whether to use it or run our own SNTP.
- **RTC**: the board has an RTC that's currently unused. Use it to (a) seed system time at
  boot before/without network, and (b) be updated from SNTP so time survives reboots and
  network outages. Set the system clock from RTC early in boot; write SNTP results back.
- **Timezone**: set `TZ` (`setenv("TZ", ...)` + `tzset()`); decide the device timezone
  (likely a web-UI setting). Until then cron is UTC.

## Rule engine (cron) impact — the things that actually bite

1. **Clock discontinuity on first sync — re-arm cron.** This is the #1 gotcha.
   `cron` arms an `esp_timer` for `next_match - now` ms, computed from the *current* epoch.
   `esp_timer` is monotonic, so when SNTP later steps the wall-clock from ~1970 to the real
   date, the already-armed one-shot still fires after its original *relative* delay — but
   that delay was computed against the wrong epoch, so it fires at the wrong real time.
   **On the first successful sync (and on any large step), re-arm all cron timers** — the
   simplest path is to trigger a rule reload (`scripting_reload` / `EVT_RELOAD`, which calls
   `_reset_rules()` and re-evaluates, re-arming cron from the corrected clock). A targeted
   "re-arm cron only" hook is a nicer alternative if reload is too heavy.

2. **Suppress cron until time is valid.** Otherwise rules fire boot-relative (e.g.
   `0 7 * * *` fires ~7 h after boot). Options: a "time valid" flag that gates cron arming,
   armed/re-armed on the sync event; or just accept boot-relative until sync and rely on
   the re-arm in (1). Decide explicitly.

3. **UTC → local time.** Cron currently evaluates in UTC (`_cronNext` uses `getUTC*`).
   Users usually expect local time. Once `TZ` is configured, decide whether to switch cron
   to local-time `Date` getters. If so: handle **DST** gaps/overlaps (a local minute can be
   skipped or repeated — UTC sidesteps this entirely), and update the UTC-anchored cron
   tests + the RULES.md caveat.

4. **`_now()` binding.** Cron reads "now" via `_now_ms()` → host `_now()` if present, else
   QuickJS `Date.now()`. Decide: add an explicit `_now()` C binding returning synced epoch
   ms, or keep relying on `Date.now()`. Either way, **verify on hardware that QuickJS
   `Date.now()` actually advances from `gettimeofday`** under ESP-IDF/newlib — the test
   harness mocks `_now()`, so this is the one piece the suite can't cover.

## NOT affected (don't touch)

`every(ms)`, `.after(ms)`, `.heldFor(ms)` use **relative** `esp_timer` delays (monotonic),
so an SNTP step does **not** disturb them. Only `cron` depends on wall-clock.

## Security / robustness (CRA / NIS2 angle)

- NTP is unauthenticated and spoofable; a bad/forged time could make time-based rules act
  at the wrong moment. Don't gate safety- or security-critical actions on cron alone —
  combine with an input/interlock, and treat time as advisory until validated.
- Validate that a sync actually happened before trusting cron (see suppress-until-valid).

## When done, update

- `components/scripting/RULES.md` — the `cron(...)` caveat (drop "boot-relative", state the
  timezone behaviour).
- `components/scripting/dsl.js` — the `_now_ms()` comment.
- `components/scripting/test/cron.test.js` — if cron switches to local time, re-anchor the
  UTC expectations (pin a test timezone) and add a re-arm-on-sync test.

## Quick checklist

- [ ] SNTP init after network up; NTP server configurable
- [ ] RTC: seed system time at boot, update from SNTP
- [ ] `TZ` set + `tzset()`; timezone configurable
- [ ] Re-arm cron on first sync / large clock step
- [ ] Decide + implement cron suppression until time valid
- [ ] Decide UTC vs local cron; if local, handle DST + update tests/docs
- [ ] Verify QuickJS `Date.now()` advances on-device (or add `_now()` C binding)
- [ ] Update RULES.md / dsl.js comment / cron tests
