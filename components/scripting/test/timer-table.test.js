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

// ── refused arms without an event of their own (every, cron, watchdog, .after) ──

test('an .after() step refused by a full table runs late, not never', () => {
  const e = createEngine({ maxTimers: 2 });
  e.load(`
    rule('pulse').when(input(0).isOn()).then(function(){ T.on = (T.on || 0) + 1; })
                 .after(1000).then(function(){ T.off = (T.off || 0) + 1; });
    rule('tick').when(every(300)).then(function(){ T.t = (T.t || 0) + 1; });`);
  for (let i = 0; i < 3; i++) { e.input(0, true); e.input(0, false); }   // one slot left: two steps refused
  e.advance(5000);
  assert.equal(e.T.on, 3);
  assert.equal(e.T.off, 3, 'the steps that switch the output off must not be dropped');
  assert.ok(e.T.t >= 5, 'every() keeps ticking while the two share the slot: ' + e.T.t);
});

test('an every() refused at load starts once the table has room', () => {
  const e = createEngine({ maxTimers: 0 });
  e.load(`rule('tick').when(every(500)).then(function(){ T.t = (T.t || 0) + 1; });`);
  e.advance(2000);
  assert.equal(e.T.t, undefined);
  e.setMaxTimers(Infinity);
  e.input(3, true);              // any event: the refused arm is tried again
  e.advance(2000);
  assert.equal(e.T.t, 4, 'every() must recover, not stay dead for the boot');
});

test('a watchdog whose arm was refused still expires', () => {
  const e = createEngine({ maxTimers: 0 });
  e.load(`
    var wd = watchdog(1000);
    rule('feed').when(input(0).isOn()).then(function(){ wd.feed(); });
    rule('fallback').when(wd.isExpired()).then(function(){ T.expired = true; });`);
  e.input(0, true);              // fed while the table is full: nothing armed
  e.setMaxTimers(Infinity);
  e.input(5, true);              // any event: armed now
  e.advance(1500);
  assert.equal(e.T.expired, true, 'the fallback is exactly what a watchdog exists for');
});

test('refused arms do not pile up and are dropped by a reload', () => {
  const e = createEngine({ maxTimers: 0 });
  e.load(`rule('a').when(every(500)).then(function(){}); rule('b').when(every(500)).then(function(){});`);
  const n = e.evalIn('_retryArms.length');
  assert.equal(n, 2, 'two every() conditions, one entry each');
  for (let i = 0; i < 10; i++) e.input(1, i % 2 === 0);   // each event retries, each refusal re-queues
  assert.equal(e.evalIn('_retryArms.length'), 2, 'retrying must not grow the queue');
  e.reload(`rule('c').when(input(2).isOn()).then(function(){});`);
  assert.equal(e.evalIn('_retryArms.length'), 0);
});

test('waiting .after() steps are capped under sustained overload', () => {
  const e = createEngine({ maxTimers: 0 });
  e.load(`rule('p').when(input(0).isOn()).then(function(){}).after(100).then(function(){ T.n = (T.n || 0) + 1; });`);
  for (let i = 0; i < 400; i++) { e.input(0, true); e.input(0, false); }
  assert.equal(e.evalIn('_retryArms.length'), 256);
  assert.ok(e.prints.some(p => /256 timers waiting/.test(p)), 'the drop is logged');
});

test('every() gets a freed slot ahead of waiting .after() steps', () => {
  const e = createEngine({ maxTimers: 4 });
  e.load(`
    rule('p').when(input(0).isOn()).then(function(){}).after(5000).then(function(){ T.off = (T.off || 0) + 1; });
    rule('tick').when(every(1000)).then(function(){ T.t = (T.t || 0) + 1; });`);
  for (let i = 0; i < 20; i++) { e.input(0, true); e.input(0, false); }   // 3 slots held, 17 steps waiting
  e.advance(10000);
  assert.ok(e.T.t >= 8, 'every(1000) keeps (nearly) its pace while the steps wait: ' + e.T.t);
  e.advance(60000);
  assert.equal(e.T.off, 20, 'and every step still runs');
});

test('a retry pass stops at the first refusal instead of trying the whole queue', () => {
  const e = createEngine({ maxTimers: 0 });
  e.load(`rule('p').when(input(0).isOn()).then(function(){}).after(100).then(function(){ T.n = (T.n || 0) + 1; });`);
  for (let i = 0; i < 50; i++) { e.input(0, true); e.input(0, false); }
  const calls = e.evalIn('(function(){ var n = 0, s = _set_timer; _set_timer = function(a, b){ n++; return s(a, b); }; _retry_arms(); _set_timer = s; return n; })()');
  assert.equal(calls, 1, 'one refused attempt, not one per waiting step');
  e.setMaxTimers(Infinity);
  e.input(1, true);
  e.advance(1000);
  assert.equal(e.T.n, 50, 'all waiting steps ran once the table had room');
});
