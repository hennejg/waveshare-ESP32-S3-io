'use strict';
// pwm(pad) and analog(pad): the IO1/RXD/TXD header pads.
const test   = require('node:test');
const assert = require('node:assert');
const { createEngine } = require('./harness');

test('pwm: set, read back, keep frequency, off', () => {
  const e = createEngine();
  e.setPwmPad(2, true, 1000);
  e.load(`T.r = pwm(2).set(25); T.d = pwm(2).duty; T.f = pwm(2).freq;
          pwm(2).set(60.5, 50); T.r2 = pwm(2).get(); pwm(2).off();`);
  assert.deepStrictEqual(e.T.r, { duty: 25, freq: 1000 });
  assert.strictEqual(e.T.d, 25);
  assert.strictEqual(e.T.f, 1000);
  assert.deepStrictEqual(e.T.r2, { duty: 60.5, freq: 50 });
  assert.deepStrictEqual(e.pwmState(2), { duty: 0, freq: 50 });
});

test('pwm: a pad that is not a PWM output refuses, and reads null', () => {
  const e = createEngine();
  assert.throws(() => e.load(`pwm(1).set(10)`), /not set to PWM/);
  assert.strictEqual(e.evalIn(`pwm(1).duty`), null);
  assert.strictEqual(e.evalIn(`pwm(1).get()`), null);
});

test('analog band fires on entering it, not on every reading inside', () => {
  const e = createEngine();
  e.analog(1, 1500);
  e.load(`rule('hot').when(analog(1).above(2000)).then(function(){ T.n = (T.n||0)+1; });`);
  e.analog(1, 2100);  assert.strictEqual(e.T.n, 1);
  e.analog(1, 2150);  assert.strictEqual(e.T.n, 1);   // still inside
  e.analog(1, 1900);  assert.strictEqual(e.T.n, 1);   // left
  e.analog(1, 2050);  assert.strictEqual(e.T.n, 2);   // entered again
});

test('bare analog(pad) fires on every reading', () => {
  const e = createEngine();
  e.load(`rule('log').when(analog(1)).then(function(){ T.n = (T.n||0)+1; T.v = analog(1).value; });`);
  e.analog(1, 100); e.analog(1, 140); e.analog(1, 200);
  assert.strictEqual(e.T.n, 3);
  assert.strictEqual(e.T.v, 200);
});

test('between and below narrow the band; conditions are immutable', () => {
  const e = createEngine();
  e.load(`var a = analog(1);
          rule('mid').when(a.between(1000, 2000)).then(function(){ T.mid = (T.mid||0)+1; });
          rule('low').when(a.below(500)).then(function(){ T.low = (T.low||0)+1; });
          rule('any').when(a).then(function(){ T.any = (T.any||0)+1; });`);
  e.analog(1, 1500); e.analog(1, 300); e.analog(1, 1000);   // 1000 is not strictly inside
  assert.strictEqual(e.T.mid, 1);
  assert.strictEqual(e.T.low, 1);
  assert.strictEqual(e.T.any, 3);
});

test('analog drives pwm: a simple controller', () => {
  const e = createEngine();
  e.setPwmPad(3, true, 1000);
  e.load(`rule('ctl').when(analog(1)).then(function(){ pwm(3).set(Math.min(100, analog(1).value / 33)); });`);
  e.analog(1, 1650);
  assert.strictEqual(e.pwmState(3).duty, 50);
});

test('analog band with heldFor fires once after the hold', () => {
  const e = createEngine();
  e.analog(1, 0);
  e.load(`rule('h').when(analog(1).above(2000)).heldFor(1000).then(function(){ T.n = (T.n||0)+1; });`);
  e.analog(1, 2500); e.advance(500); e.analog(1, 2600); e.advance(600);
  assert.strictEqual(e.T.n, 1);
  e.analog(1, 2700); e.advance(2000);
  assert.strictEqual(e.T.n, 1);
});

test('a pad that leaves analog mode no longer matches a band', () => {
  const e = createEngine();
  e.analog(1, 2500);
  e.load(`rule('x').when(analog(1).above(2000), input(0).isOn()).then(function(){ T.n = (T.n||0)+1; });`);
  e.input(0, true);  assert.strictEqual(e.T.n, 1);
  e.input(0, false);
  e.analogGone(1);
  e.input(0, true);  assert.strictEqual(e.T.n, 1);   // the old 2500 is forgotten
  assert.strictEqual(e.evalIn('analog(1).value'), null);
});

test('a band fired through another fact does not fire again on the next reading inside', () => {
  const e = createEngine();
  e.analog(1, 2100);
  e.load(`rule('x').when(analog(1).above(2000), input(0).isOn()).then(function(){ T.n = (T.n||0)+1; });`);
  e.input(0, true);   assert.strictEqual(e.T.n, 1);
  e.analog(1, 2130);  assert.strictEqual(e.T.n, 1);   // still inside: no new edge
  e.analog(1, 1500);  e.analog(1, 2200);
  assert.strictEqual(e.T.n, 2);                       // left and entered again
});

test('a predicate rule is edge-triggered on analog readings', () => {
  const e = createEngine();
  e.load(`rule('p').when(function(){ return input(0).value; }).then(function(){ T.n = (T.n||0)+1; });`);
  e.input(0, true);   assert.strictEqual(e.T.n, 1);
  e.analog(1, 100); e.analog(1, 200); e.analog(1, 300);
  assert.strictEqual(e.T.n, 1);                       // readings do not re-run it
});

test('pad numbers outside 1..3 are refused, whatever their size', () => {
  const e = createEngine();
  assert.throws(() => e.load(`pwm(0)`),          /pad must be 1, 2 or 3/);
  assert.throws(() => e.load(`analog(4)`),       /pad must be 1, 2 or 3/);
  assert.throws(() => e.load(`pwm(4294967297)`), /pad must be 1, 2 or 3/);
});

test('pwm frequency is checked as on the device', () => {
  const e = createEngine();
  e.setPwmPad(2, true, 1000);
  for (const bad of ['2', '50.5', '2e6', '-100'])
    assert.throws(() => e.load(`pwm(2).set(50, ${bad})`), /whole number of Hz/);
  assert.throws(() => e.load(`pwm(2).set(100.5)`), /0-100/);
  assert.deepStrictEqual(e.pwmState(2), { duty: 0, freq: 1000 });
});
