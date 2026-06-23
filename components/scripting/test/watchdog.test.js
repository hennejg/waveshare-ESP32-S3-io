'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { createEngine } = require('./harness.js');

test('watchdog starts alive (grace period) and expires when unfed', () => {
  const e = createEngine();
  e.load(`
    var wd = watchdog(5000);
    rule('expired').when(wd.isExpired()).then(function(){ T.n = (T.n||0)+1; });
  `);
  assert.equal(e.T.n, undefined);
  e.advance(4999); assert.equal(e.T.n, undefined);   // still within the grace period
  e.advance(1);    assert.equal(e.T.n, 1);            // expired at 5000ms
});

test('feed() keeps it alive; it expires ms after the last feed', () => {
  const e = createEngine();
  e.load(`
    var wd = watchdog(5000);
    rule('hb').when(input(0).isOn()).then(function(){ wd.feed(); });
    rule('expired').when(wd.isExpired()).then(function(){ T.n = (T.n||0)+1; });
  `);
  e.advance(3000); e.input(0, true);   // feed at t=3000 → re-arms to t=8000
  e.advance(4999); assert.equal(e.T.n, undefined);
  e.advance(1);    assert.equal(e.T.n, 1);            // expired at 8000
});

test('feed() after expiry recovers the watchdog', () => {
  const e = createEngine();
  e.load(`
    var wd = watchdog(5000);
    rule('hb').when(input(0).isOn()).then(function(){ wd.feed(); });
    rule('down').when(wd.isExpired()).then(function(){ T.down = (T.down||0)+1; });
    rule('up').when(wd.isAlive()).then(function(){ T.up = (T.up||0)+1; });
  `);
  e.advance(5000);   assert.equal(e.T.down, 1);   // expired
  e.input(0, true);  assert.equal(e.T.up, 1);     // a feed recovers it (emits) → 'up' fires
});

test('fallback pattern: DI0 acts only while control is absent', () => {
  const e = createEngine();
  e.load(`
    var control = watchdog(90000);
    rule('heartbeat').when(mqtt('sys/heartbeat')).then(function(){ control.feed(); });
    rule('override').when(input(0).isOn(), control.isExpired()).then(function(){ output(0).toggle(); });
  `);
  e.publish('sys/heartbeat', '1');               // control is alive
  e.input(0, true);  assert.equal(e.output(0), false);   // gated out while controlled
  e.input(0, false);
  e.advance(90000);                              // heartbeat stops → control expires
  e.input(0, true);  assert.equal(e.output(0), true);    // fallback active → DI0 toggles
});

test("mqtt(topic).stale(ms): messages keep it fresh; stale after ms; recovers", () => {
  const e = createEngine();
  e.load(`rule('stale').when(mqtt('hb').stale(30000)).then(function(){ T.n = (T.n||0)+1; });`);
  e.publish('hb', '1');
  e.advance(29999); assert.equal(e.T.n, undefined);  // fresh
  e.advance(1);     assert.equal(e.T.n, 1);          // stale 30s after last message
  e.publish('hb', '2');                              // new message → fresh again
  e.advance(29999); assert.equal(e.T.n, 1);
  e.advance(1);     assert.equal(e.T.n, 2);          // stale again 30s after the new message
});

test('mqttDown() / mqttConnected() gate on the broker connection state', () => {
  const e = createEngine();
  e.load(`
    rule('lost').when(mqttDown()).then(function(){ T.lost = (T.lost||0)+1; });
    rule('back').when(mqttConnected()).then(function(){ T.back = (T.back||0)+1; });
  `);
  e.publish('$sys/mqtt', 'down'); assert.equal(e.T.lost, 1);
  e.publish('$sys/mqtt', 'up');   assert.equal(e.T.back, 1);
  e.publish('$sys/mqtt', 'down'); assert.equal(e.T.lost, 2);
});

test('reload cancels watchdog timers', () => {
  const e = createEngine();
  e.load(`var wd = watchdog(5000); rule('x').when(wd.isExpired()).then(function(){ T.n = (T.n||0)+1; });`);
  assert.equal(e.pendingTimers(), 1);
  e.reload(`rule('y').when(input(0).isOn()).then(function(){});`);
  assert.equal(e.pendingTimers(), 0);
  e.advance(10000); assert.equal(e.T.n, undefined);
});
