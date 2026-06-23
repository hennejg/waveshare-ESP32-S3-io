'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { createEngine } = require('./harness.js');

// cron is evaluated in LOCAL time. Pin this test process to UTC so the UTC-anchored
// instants below behave as before (local == UTC); the local-time/DST tests at the bottom
// switch TZ explicitly via withTZ(). Node applies process.env.TZ changes at runtime, and
// the test runner gives each file its own process, so this pin doesn't leak elsewhere.
process.env.TZ = 'UTC';

// Run a body under a specific timezone, restoring UTC afterwards. Set before any Date use
// (cron arms — and computes the first match — at load() time).
function withTZ(tz, fn) {
  process.env.TZ = tz;
  try { return fn(); } finally { process.env.TZ = 'UTC'; }
}

const MON_2024_01_01 = Date.UTC(2024, 0, 1, 0, 0, 0); // Monday 2024-01-01 00:00:00Z
const SUN_2023_12_31 = Date.UTC(2023, 11, 31, 0, 0, 0); // Sunday 2023-12-31 00:00:00Z
const HOUR = 3600000, DAY = 24 * HOUR, MIN = 60000;

test('cron rejects malformed expressions', () => {
  const e = createEngine();
  assert.throws(() => e.load(`rule('x').when(cron('* * *')).then(function(){});`));        // too few fields
  assert.throws(() => e.load(`rule('x').when(cron('99 * * * *')).then(function(){});`));   // minute out of range
  assert.throws(() => e.load(`rule('x').when(cron('* 24 * * *')).then(function(){});`));   // hour out of range
});

test("cron '* * * * *' fires every minute", () => {
  const e = createEngine();
  e.setClock(MON_2024_01_01);
  e.load(`rule('m').when(cron('* * * * *')).then(function(){ T.n = (T.n||0)+1; });`);
  e.advance(MIN); assert.equal(e.T.n, 1);
  e.advance(MIN); assert.equal(e.T.n, 2);
});

test("cron '0 * * * *' fires at the top of each hour", () => {
  const e = createEngine();
  e.setClock(MON_2024_01_01);   // exactly 00:00
  e.load(`rule('h').when(cron('0 * * * *')).then(function(){ T.n = (T.n||0)+1; });`);
  e.advance(HOUR - 1); assert.equal(e.T.n, undefined);  // 00:59:59.999
  e.advance(1);        assert.equal(e.T.n, 1);          // 01:00:00
  e.advance(HOUR);     assert.equal(e.T.n, 2);          // 02:00:00
});

test("cron '0 7 * * *' fires daily at 07:00", () => {
  const e = createEngine();
  e.setClock(MON_2024_01_01);
  e.load(`rule('d').when(cron('0 7 * * *')).then(function(){ T.n = (T.n||0)+1; });`);
  e.advance(7 * HOUR - 1); assert.equal(e.T.n, undefined);
  e.advance(1);            assert.equal(e.T.n, 1);   // 07:00 day 1
  e.advance(DAY);          assert.equal(e.T.n, 2);   // 07:00 day 2
});

test("cron '*/15 * * * *' fires four times an hour", () => {
  const e = createEngine();
  e.setClock(MON_2024_01_01);   // 00:00
  e.load(`rule('q').when(cron('*/15 * * * *')).then(function(){ T.n = (T.n||0)+1; });`);
  e.advance(HOUR);   // strictly after 00:00 → 00:15, 00:30, 00:45, 01:00
  assert.equal(e.T.n, 4);
});

test("cron 'a/n' steps from a to the field max (e.g. 5/15 → :05,:20,:35,:50)", () => {
  const e = createEngine();
  e.setClock(MON_2024_01_01);   // 00:00
  e.load(`rule('q').when(cron('5/15 * * * *')).then(function(){ T.n = (T.n||0)+1; });`);
  e.advance(HOUR);   // from 00:00 → 00:05, 00:20, 00:35, 00:50 (then 01:05 is past the hour)
  assert.equal(e.T.n, 4);
});

test("cron '30 9-17 * * *' respects an hour range", () => {
  const e = createEngine();
  e.setClock(MON_2024_01_01);
  e.load(`rule('biz').when(cron('30 9-17 * * *')).then(function(){ T.n = (T.n||0)+1; });`);
  e.advance(DAY);   // :30 of hours 9..17 inclusive → 9 fires in the day
  assert.equal(e.T.n, 9);
});

test("cron day-of-week '0 0 * * 1' fires on Mondays", () => {
  const e = createEngine();
  e.setClock(SUN_2023_12_31);   // Sunday 00:00
  e.load(`rule('mon').when(cron('0 0 * * 1')).then(function(){ T.n = (T.n||0)+1; });`);
  e.advance(DAY - 1); assert.equal(e.T.n, undefined);   // still before Mon 00:00
  e.advance(1);       assert.equal(e.T.n, 1);           // Mon 2024-01-01 00:00
  e.advance(7 * DAY); assert.equal(e.T.n, 2);           // following Monday
});

test('cron dom/dow are OR-ed when both are restricted (Vixie semantics)', () => {
  const e = createEngine();
  e.setClock(MON_2024_01_01);   // Mon Jan 1 (both the 1st AND a Monday)
  // "midnight on the 1st OR on Mondays"
  e.load(`rule('r').when(cron('0 0 1 * 1')).then(function(){ T.n = (T.n||0)+1; });`);
  e.advance(7 * DAY);   // strictly after Jan 1: next match is Jan 8 (a Monday, not the 1st)
  assert.equal(e.T.n, 1);
});

test('reload cancels cron timers', () => {
  const e = createEngine();
  e.setClock(MON_2024_01_01);
  e.load(`rule('c').when(cron('* * * * *')).then(function(){ T.n = (T.n||0)+1; });`);
  assert.equal(e.pendingTimers(), 1);
  e.reload(`rule('x').when(input(0).isOn()).then(function(){});`);
  assert.equal(e.pendingTimers(), 0);
  e.advance(10 * MIN);
  assert.equal(e.T.n, undefined);
});

// ── Local time ────────────────────────────────────────────────────────────────
test('cron fires at the configured local time, not UTC', () => {
  // Etc/GMT-2 is a fixed UTC+2 zone (no DST). "07:00 local" == 05:00 UTC.
  withTZ('Etc/GMT-2', () => {
    const e = createEngine();
    e.setClock(MON_2024_01_01);   // 2024-01-01 00:00 UTC == 02:00 local
    e.load(`rule('d').when(cron('0 7 * * *')).then(function(){ T.n = (T.n||0)+1; });`);
    e.advance(5 * HOUR - 1); assert.equal(e.T.n, undefined); // 04:59:59.999 UTC
    e.advance(1);            assert.equal(e.T.n, 1);         // 05:00 UTC == 07:00 local
    e.advance(DAY);          assert.equal(e.T.n, 2);         // next local 07:00
  });
});

// ── DST ─────────────────────────────────────────────────────────────────────
test('cron skips a wall-clock time that DST spring-forward removes', () => {
  // Europe/Berlin springs forward 2024-03-31: 02:00 CET → 03:00 CEST, so local 02:30
  // never occurs that day. A daily "02:30" job fires 03-30, is skipped 03-31, fires 04-01.
  withTZ('Europe/Berlin', () => {
    const e = createEngine();
    e.setClock(Date.UTC(2024, 2, 30, 0, 0, 0));  // 03-30 00:00 UTC == 01:00 CET
    e.load(`rule('dst').when(cron('30 2 * * *')).then(function(){ T.n = (T.n||0)+1; });`);
    // 03-30 02:30 CET == 01:30 UTC → fires once within the first 2 h.
    e.advance(2 * HOUR);         assert.equal(e.T.n, 1);
    // Next 24 h covers all of 03-31, where 02:30 local does not exist → no fire.
    e.advance(24 * HOUR);        assert.equal(e.T.n, 1);
    // 04-01 02:30 CEST == 00:30 UTC → fires again.
    e.advance(24 * HOUR);        assert.equal(e.T.n, 2);
  });
});

// ── Suppress until valid + re-arm on sync ─────────────────────────────────────
test('cron stays suppressed until the clock is valid, then arms on time-sync', () => {
  const e = createEngine({ timeValid: false });
  e.setClock(0);   // boot-relative epoch (1970)
  e.load(`rule('s').when(cron('0 7 * * *')).then(function(){ T.n = (T.n||0)+1; });`);
  assert.equal(e.pendingTimers(), 0);          // suppressed — not armed off a 1970 clock
  e.advance(8 * HOUR); assert.equal(e.T.n, undefined);

  // SNTP-style step to a real time, then sync → cron arms from the corrected clock.
  e.setClock(MON_2024_01_01);
  e.timeSync();
  assert.equal(e.pendingTimers(), 1);
  e.advance(7 * HOUR - 1); assert.equal(e.T.n, undefined);
  e.advance(1);            assert.equal(e.T.n, 1);   // 07:00 of the real day, not 7 h after boot
});

test('time-sync re-arms an already-armed cron without duplicating its timer', () => {
  const e = createEngine();                    // timeValid defaults true
  e.setClock(MON_2024_01_01);
  e.load(`rule('r').when(cron('0 * * * *')).then(function(){ T.n = (T.n||0)+1; });`);
  assert.equal(e.pendingTimers(), 1);
  e.timeSync();                                // re-sync: cancel + re-arm, not add
  assert.equal(e.pendingTimers(), 1);
  e.advance(HOUR); assert.equal(e.T.n, 1);     // still fires exactly once per hour
  e.advance(HOUR); assert.equal(e.T.n, 2);
});
