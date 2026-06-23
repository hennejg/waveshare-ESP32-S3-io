'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { createEngine } = require('./harness.js');

// ── led() ─────────────────────────────────────────────────────────────────────
test('led().set(r,g,b) drives the LED', () => {
  const e = createEngine();
  e.load(`rule('l').when(input(0).isOn()).then(function(){ led().set(10, 20, 30); });`);
  e.input(0, true);
  assert.deepEqual(e.ledState(), { r: 10, g: 20, b: 30 });
});

test('led().set("#rrggbb") parses hex (with or without #)', () => {
  const e = createEngine();
  e.load(`rule('a').when(input(0).isOn()).then(function(){ led().set('#ff8000'); });
          rule('b').when(input(1).isOn()).then(function(){ led().set('00ff10'); });`);
  e.input(0, true); assert.deepEqual(e.ledState(), { r: 255, g: 128, b: 0 });
  e.input(1, true); assert.deepEqual(e.ledState(), { r: 0, g: 255, b: 16 });
});

test('led().off() clears the LED to black', () => {
  const e = createEngine();
  e.load(`rule('l').when(input(0).isOn()).then(function(){ led().set(255,255,255); led().off(); });`);
  e.input(0, true);
  assert.deepEqual(e.ledState(), { r: 0, g: 0, b: 0 });
});

test('led(): a bad hex string is ignored (keeps the previous colour) with a warning', () => {
  const e = createEngine();
  e.load(`rule('l').when(input(0).isOn()).then(function(){ led().set(1,2,3); led().set('nope'); });`);
  e.input(0, true);
  assert.deepEqual(e.ledState(), { r: 1, g: 2, b: 3 });
  assert.ok(e.prints.some(p => /bad colour/.test(p)), 'expected a "bad colour" warning');
});

// ── buzzer() ──────────────────────────────────────────────────────────────────
test('buzzer().set(freq) plays a tone; .off() silences', () => {
  const e = createEngine();
  e.load(`rule('on').when(input(0).isOn()).then(function(){ buzzer().set(2000); });
          rule('off').when(input(0).isOff()).then(function(){ buzzer().off(); });`);
  e.input(0, true);  assert.equal(e.buzzerFreq(), 2000);
  e.input(0, false); assert.equal(e.buzzerFreq(), 0);
});

// ── modbus() / can() command health ────────────────────────────────────────────
test('modbus(ms) is alive during the grace window, then expires when quiet', () => {
  const e = createEngine();
  e.load(`rule('mb').when(modbus(5000).isExpired()).then(function(){ T.n = (T.n||0)+1; });`);
  e.advance(4999); assert.equal(e.T.n, undefined);   // still within grace → alive
  e.advance(1);    assert.equal(e.T.n, 1);            // 5 s with no command → expired
});

test('modbus activity keeps the link alive and recovers it after a silence', () => {
  const e = createEngine();
  e.load(`rule('down').when(modbus(1000).isExpired()).then(function(){ T.n = (T.n||0)+1; });`);
  e.advance(500); e.activity('modbus');   // command at t=500 → re-arms to t=1500
  e.advance(999); assert.equal(e.T.n, undefined);
  e.advance(1);   assert.equal(e.T.n, 1);  // t=1500: expired
  e.activity('modbus');                    // command → recover (alive again)
  e.advance(1000); assert.equal(e.T.n, 2); // quiet for another window → expires again
});

test('modbus(ms).isAlive() gates a rule while commands flow', () => {
  const e = createEngine();
  e.load(`rule('ok').when(input(0).isOn(), modbus(1000).isAlive())
                     .then(function(){ T.n = (T.n||0)+1; });`);
  e.input(0, true);                 // within grace → alive → fires
  assert.equal(e.T.n, 1);
  e.advance(1000);                  // link expires
  e.input(0, false); e.input(0, true);  // re-trigger while expired → gated out
  assert.equal(e.T.n, 1);
});

test('modbus() and can() are independent sources', () => {
  const e = createEngine();
  e.load(`rule('mb').when(modbus(1000).isExpired()).then(function(){ T.mb = (T.mb||0)+1; });
          rule('cn').when(can(1000).isExpired()).then(function(){ T.cn = (T.cn||0)+1; });`);
  e.advance(500);
  e.activity('modbus');   // feed modbus only
  e.advance(500);         // t=1000: can never fed → expires; modbus alive (fed at 500)
  assert.equal(e.T.cn, 1);
  assert.equal(e.T.mb, undefined);
});

test('reload drops modbus()/can() health watchdogs', () => {
  const e = createEngine();
  e.load(`rule('mb').when(modbus(1000).isExpired()).then(function(){ T.n = (T.n||0)+1; });`);
  assert.equal(e.pendingTimers(), 1);
  e.reload(`rule('x').when(input(0).isOn()).then(function(){});`);
  assert.equal(e.pendingTimers(), 0);
  e.advance(5000);
  assert.equal(e.T.n, undefined);
});
