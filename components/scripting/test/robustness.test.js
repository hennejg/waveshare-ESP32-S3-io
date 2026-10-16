'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { createEngine } = require('./harness.js');

test('when() accepts a boolean gate', () => {
  const e = createEngine();
  e.load(`
    rule('t').when(input(0).isOn(), true).then(function(){ T.t = (T.t||0)+1; });
    rule('f').when(input(0).isOn(), false).then(function(){ T.f = (T.f||0)+1; });
  `);
  e.input(0, true);
  assert.equal(e.T.t, 1);
  assert.equal(e.T.f, undefined);   // a false constant gate blocks
});

test('when() treats null/undefined as non-blocking', () => {
  const e = createEngine();
  e.load(`rule('r').when(input(0).isOn(), null, undefined).then(function(){ T.n = (T.n||0)+1; });`);
  e.input(0, true);
  assert.equal(e.T.n, 1);   // stray empty args never silently block a rule
});

test('a throwing condition blocks its own rule but not the engine', () => {
  const e = createEngine();
  e.load(`
    rule('boom').when(function(){ throw new Error('boom'); }).then(function(){ T.boom = true; });
    rule('safe').when(input(0).isOn()).then(function(){ T.safe = true; });
  `);
  assert.doesNotThrow(() => e.input(0, true));
  assert.equal(e.T.boom, undefined);   // boom's condition threw → rule blocked
  assert.equal(e.T.safe, true);        // an unrelated rule still fires
  assert.ok(e.prints.some(p => /condition error/.test(p)));
});

test('a throwing action is caught and logged', () => {
  const e = createEngine();
  e.load(`rule('r').when(input(0).isOn()).then(function(){ throw new Error('kaboom'); });`);
  assert.doesNotThrow(() => e.input(0, true));
  assert.ok(e.prints.some(p => /kaboom/.test(p)));
});

test('reload clears the rule set and cancels pending timers', () => {
  const e = createEngine();
  e.load(`
    rule('p').when(input(0).isOn())
      .then(function(){ output(0).on(); })
      .after(9000).then(function(){ T.off = (T.off||0)+1; });
  `);
  e.input(0, true);
  assert.equal(e.pendingTimers(), 1);
  assert.equal(e.evalIn('_rules.length'), 1);

  e.reload(`rule('q').when(input(1).isOn()).then(function(){ output(1).on(); });`);
  assert.equal(e.pendingTimers(), 0);            // the .after timer was cancelled
  assert.equal(e.evalIn('_rules.length'), 1);    // only the new rule remains

  e.advance(9000);
  assert.equal(e.T.off, undefined);              // the stale follow-up step never ran
});

test('a rule throwing a Symbol leaves no stale pass and later rules still run', () => {
  const { createEngine } = require('./harness');
  const e = createEngine();
  e.load(`
    rule('bad').when(function(){ throw Symbol('x'); }).then(function(){});
    rule('later').when(input(0).isOn()).then(function(){ T.n = (T.n || 0) + 1; });
    rule('loud').when(input(1).isOn()).noLoop().then(function(){ T.l = (T.l || 0) + 1; throw { toString: function(){ throw new Error('no'); } }; });`);
  for (let i = 0; i < 5; i++) { e.input(0, true); e.input(0, false); }
  assert.equal(e.evalIn('_passes.length'), 0, 'every pass taken off the stack');
  assert.equal(e.T.n, 5, 'the rules after the throwing one see every event');
  e.input(1, true); e.input(1, false); e.input(1, true);
  assert.equal(e.T.l, 2, 'a noLoop rule whose error cannot be printed runs again');
});

test('channel numbers far out of range are refused, not wrapped', () => {
  const { createEngine } = require('./harness');
  const e = createEngine();
  assert.throws(() => e.load(`output(4294967296).on();`), /out of range/);
  assert.throws(() => e.load(`rule('x').when(input(-1).isOn()).then(function(){});`), /out of range/);
});
