'use strict';
// The device answers -1 from _set_timer when its timer table (MAX_TIMERS in
// scripting.c) is full. These cover what the DSL does with that answer.

const { test } = require('node:test');
const assert   = require('node:assert');
const { createEngine } = require('./harness');

test('a refused timer is not recorded as pending', () => {
  const e = createEngine({ maxTimers: 0 });
  e.load(`rule('hold').when(input(0).isOn()).heldFor(1000).then(function(){ T.n = (T.n || 0) + 1; });`);
  e.input(0, true);
  assert.equal(e.pendingTimers(), 0);
  assert.equal(e.evalIn('_pending.length'), 0, '-1 must not be tracked as a live timer');
});

test('heldFor re-arms once the timer table frees up', () => {
  const e = createEngine({ maxTimers: 0 });
  e.load(`rule('hold').when(input(0).isOn()).heldFor(1000).then(function(){ T.n = (T.n || 0) + 1; });`);

  e.input(0, true);        // condition matches, arming is refused
  e.advance(5000);
  assert.equal(e.T.n, undefined, 'nothing can fire while the table is full');

  e.setMaxTimers(Infinity);
  e.input(0, true);        // same condition, still matching — must try again
  e.advance(1000);
  assert.equal(e.T.n, 1, 'the rule has to recover, not stay dead for the boot');
});

test('a refused timer does not disturb a later healthy one', () => {
  const e = createEngine({ maxTimers: 0 });
  e.load(`rule('a').when(input(0).isOn()).heldFor(500).then(function(){ T.a = true; });`);
  e.input(0, true);
  e.setMaxTimers(Infinity);
  e.load(`rule('b').when(input(1).isOn()).heldFor(500).then(function(){ T.b = true; });`);
  e.input(1, true);
  e.advance(500);
  assert.equal(e.T.b, true);
});
