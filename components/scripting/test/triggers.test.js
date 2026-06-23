'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { createEngine } = require('./harness.js');

test('every(ms) fires once per period', () => {
  const e = createEngine();
  e.load(`rule('tick').when(every(5000)).then(function(){ T.n = (T.n||0)+1; });`);
  assert.equal(e.T.n, undefined);          // nothing fires at t=0
  e.advance(4999); assert.equal(e.T.n, undefined);
  e.advance(1);    assert.equal(e.T.n, 1); // first tick at 5000ms
  e.advance(5000); assert.equal(e.T.n, 2);
  e.advance(5000); assert.equal(e.T.n, 3);
});

test('every(ms) is gated by the other when()-conditions', () => {
  const e = createEngine();
  e.load(`rule('poll').when(every(1000), input(0).isOn()).then(function(){ T.n = (T.n||0)+1; });`);
  e.advance(1000); assert.equal(e.T.n, undefined);  // DI0 low → gated out
  e.setInput(0, true);
  e.advance(1000); assert.equal(e.T.n, 1);          // now ticks through
  e.advance(2000); assert.equal(e.T.n, 3);
});

test('every() is an edge: unrelated events between ticks do not fire it', () => {
  const e = createEngine();
  e.load(`rule('t').when(every(1000)).then(function(){ T.n = (T.n||0)+1; });`);
  e.input(2, true);                         // unrelated input event
  assert.equal(e.T.n, undefined);           // not this trigger's fact → no fire
  e.advance(1000); assert.equal(e.T.n, 1);  // only the tick fires it
});

test('each every() gets its own fact (independent timers)', () => {
  const e = createEngine();
  e.load(`
    rule('a').when(every(1000)).then(function(){ T.a = (T.a||0)+1; });
    rule('b').when(every(3000)).then(function(){ T.b = (T.b||0)+1; });
  `);
  e.advance(3000);
  assert.equal(e.T.a, 3);   // 1000, 2000, 3000
  assert.equal(e.T.b, 1);   // 3000
});

test('reload cancels interval timers', () => {
  const e = createEngine();
  e.load(`rule('t').when(every(1000)).then(function(){ T.n = (T.n||0)+1; });`);
  assert.equal(e.pendingTimers(), 1);
  e.reload(`rule('x').when(input(0).isOn()).then(function(){});`);
  assert.equal(e.pendingTimers(), 0);
  e.advance(10000);
  assert.equal(e.T.n, undefined);   // the old interval stopped ticking
});

test('every(ms) is floored at the 100ms minimum', () => {
  const e = createEngine();
  e.load(`rule('fast').when(every(50)).then(function(){ T.n = (T.n||0)+1; });`);
  assert.ok(e.prints.some(p => /raised to the 100ms minimum/.test(p)), 'warns about the clamp');
  e.advance(99); assert.equal(e.T.n, undefined);   // would have ticked at 50 if not floored
  e.advance(1);  assert.equal(e.T.n, 1);            // ticks at 100ms instead
});

test('a time-triggered action can drive outputs (and cascades are bounded)', () => {
  const e = createEngine();
  e.load(`rule('blink').when(every(1000)).then(function(){ output(0).toggle(); });`);
  e.advance(1000); assert.equal(e.output(0), true);
  e.advance(1000); assert.equal(e.output(0), false);
});
