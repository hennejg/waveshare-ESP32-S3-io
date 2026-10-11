'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { createEngine } = require('./harness.js');

// Counts the engine's timer arms (each stale() feed re-creates its timer).
function countSchedules(e) {
  e.evalIn(`var __arms = 0, __sched = _schedule;
            _schedule = function(ms, fn) { __arms++; return __sched(ms, fn); };`);
  return () => e.evalIn('__arms');
}

test('a cron() no rule uses holds no timer, before or after a time sync', () => {
  const e = createEngine({ timeValid: false });
  e.load(`var spare = cron('* * * * *');`);
  e.timeSync();
  assert.equal(e.pendingTimers(), 0);
});

test('a cron() registered before the clock is valid is armed by the time sync', () => {
  const e = createEngine({ timeValid: false });
  e.setClock(Date.UTC(2024, 0, 1, 0, 0, 0));
  e.load(`rule('m').when(cron('* * * * *')).then(function(){ T.n = (T.n||0)+1; });`);
  assert.equal(e.pendingTimers(), 0);
  e.timeSync();
  assert.equal(e.pendingTimers(), 1);
  e.advance(60000);
  assert.equal(e.T.n, 1);
});

test('a stale() shared by several rules is fed once per message', () => {
  const e = createEngine();
  e.load(`var s = mqtt('hb').stale(30000);
          rule('a').when(s).then(function(){ T.a = (T.a||0)+1; });
          rule('b').when(s).then(function(){ T.b = (T.b||0)+1; });
          rule('c').when(s).then(function(){ T.c = (T.c||0)+1; });`);
  const arms = countSchedules(e);
  e.publish('hb', '1');
  assert.equal(arms(), 1);
  e.advance(30001);
  assert.deepEqual([e.T.a, e.T.b, e.T.c], [1, 1, 1]);
});

test('messages reach only the conditions of their topic; a once() shared by rules fires each', () => {
  const e = createEngine();
  e.load(`var go = mqtt('go').once();
          rule('a').when(go).then(function(){ T.a = (T.a||0)+1; });
          rule('b').when(go).then(function(){ T.b = (T.b||0)+1; });
          rule('v').when(mqtt('val').is(function(v){ return v === 'on'; })).then(function(){ T.v = (T.v||0)+1; });`);
  e.publish('other', 'on');
  assert.equal(e.T.v, undefined);
  e.publish('go', 'x');
  assert.deepEqual([e.T.a, e.T.b], [1, 1]);
  e.publish('val', 'on');
  assert.equal(e.T.v, 1);
  assert.deepEqual([e.T.a, e.T.b], [1, 1]);   // the edge is disarmed after its pass
});

test('topics named like Object members are plain topics', () => {
  const e = createEngine();
  e.load(`rule('c').when(mqtt('constructor')).then(function(){ T.c = (T.c||0)+1; });`);
  e.publish('__proto__', 'x');
  e.publish('toString', 'x');
  assert.equal(e.T.c, undefined);
  e.publish('constructor', 'x');
  assert.equal(e.T.c, 1);
});

test('a reload drops the old rules from the topic index', () => {
  const e = createEngine();
  e.load(`rule('old').when(mqtt('t')).then(function(){ T.old = (T.old||0)+1; });`);
  e.reload(`rule('new').when(mqtt('u')).then(function(){ T.nu = (T.nu||0)+1; });`);
  e.publish('t', '1');
  e.publish('u', '1');
  assert.equal(e.T.old, undefined);
  assert.equal(e.T.nu, 1);
});
