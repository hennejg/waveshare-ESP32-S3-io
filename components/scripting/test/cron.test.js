'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { createEngine } = require('./harness.js');

// cron is evaluated in UTC. Anchor tests to known UTC instants.
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
