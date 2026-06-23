'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { createEngine } = require('./harness.js');

test('.after() pulse: on now, off after the delay', () => {
  const e = createEngine();
  e.load(`
    rule('pulse').when(input(4).isOn())
      .then(function(){ output(4).on(); })
      .after(5000).then(function(){ output(4).off(); });
  `);
  e.input(4, true);
  assert.equal(e.output(4), true);     // immediate step
  assert.equal(e.pendingTimers(), 1);  // off scheduled

  e.advance(4999); assert.equal(e.output(4), true);   // not yet
  e.advance(1);    assert.equal(e.output(4), false);  // fires at 5000ms
  assert.equal(e.pendingTimers(), 0);
});

test('.after() follow-up fires regardless of the condition dropping', () => {
  const e = createEngine();
  e.load(`
    rule('pulse').when(input(4).isOn())
      .then(function(){ output(4).on(); })
      .after(5000).then(function(){ output(4).off(); });
  `);
  e.input(4, true);
  e.input(4, false);                    // condition drops before the delay elapses
  e.advance(5000);
  assert.equal(e.output(4), false);     // off still runs (fire-and-forget)
});

test('multi-step .after timeline runs in order with relative delays', () => {
  const e = createEngine();
  e.load(`
    rule('seq').when(input(0).isOn())
      .then(function(){ T.log = (T.log||''); T.log += 'a'; })
      .after(100).then(function(){ T.log += 'b'; })
      .after(100).then(function(){ T.log += 'c'; });
  `);
  e.input(0, true);
  assert.equal(e.T.log, 'a');
  e.advance(100); assert.equal(e.T.log, 'ab');
  e.advance(100); assert.equal(e.T.log, 'abc');
});

test('.heldFor() fires only after the conditions hold for the duration', () => {
  const e = createEngine();
  e.load(`rule('h').when(input(5).isOn()).heldFor(5000).then(function(){ output(5).on(); });`);
  e.input(5, true);
  assert.equal(e.output(5), false);    // armed, not fired
  assert.equal(e.pendingTimers(), 1);

  e.advance(4999); assert.equal(e.output(5), false);
  e.advance(1);    assert.equal(e.output(5), true);   // fires at 5000ms of sustained high
});

test('.heldFor() is cancelled if the condition drops before the delay', () => {
  const e = createEngine();
  e.load(`rule('h').when(input(5).isOn()).heldFor(5000).then(function(){ output(5).on(); });`);
  e.input(5, true);
  assert.equal(e.pendingTimers(), 1);
  e.input(5, false);                   // drop before elapse → cancel
  assert.equal(e.pendingTimers(), 0);
  e.advance(5000);
  assert.equal(e.output(5), false);    // never fired
});

test('.heldFor() fires once per sustained period (re-arms after drop/rise)', () => {
  const e = createEngine();
  e.load(`rule('h').when(input(5).isOn()).heldFor(1000).then(function(){ T.n = (T.n||0)+1; });`);
  e.input(5, true);  e.advance(1000); assert.equal(e.T.n, 1);
  e.advance(5000);   assert.equal(e.T.n, 1);          // stays high, no re-fire without a fresh period
  e.input(5, false); e.input(5, true); e.advance(1000);
  assert.equal(e.T.n, 2);                              // dropped and rose → fires again
});
