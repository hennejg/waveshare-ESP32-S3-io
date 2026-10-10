'use strict';
// Found by a review of the rule engine; each test failed on the DSL before the fix.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { createEngine } = require('./harness.js');

test('the cascade cap counts hops, not changes: one action may set 20 watched facts', () => {
  const e = createEngine();
  let src = 'var f = [];\nfor (var i = 0; i < 20; i++) f.push(fact(0));\n';
  src += 'for (var i = 0; i < 20; i++) (function(k){ rule("w" + k).when(f[k].is(function(v){ return v > 0; }))' +
         '.then(function(){ T.n = (T.n || 0) + 1; }); })(i);\n';
  src += 'rule("all").when(input(0).isOn()).then(function(){ for (var i = 0; i < 20; i++) f[i].set(1); });\n';
  e.load(src);
  e.input(0, true);
  assert.equal(e.T.n, 20);
  assert.ok(!e.prints.some(p => /cascade limit/.test(p)), 'no cascade warning for a flat fan-out');
});

test('a script may set more than 16 facts at the top level while it loads', () => {
  const e = createEngine();
  let src = 'var f = [];\nfor (var i = 0; i < 20; i++) f.push(fact(0));\n';
  src += 'for (var i = 0; i < 20; i++) (function(k){ rule("w" + k).when(f[k].is(function(v){ return v > 0; }))' +
         '.then(function(){ T.n = (T.n || 0) + 1; }); })(i);\n';
  src += 'for (var i = 0; i < 20; i++) f[i].set(1);\n';
  e.load(src);
  assert.equal(e.T.n, 20);
});

test('.noLoop() also covers the rule\'s own delayed .after() step', () => {
  const e = createEngine();
  e.load(`
    rule('blink').when(input(0).isOn(), output(0).isOff()).noLoop()
      .then(function(){ T.n = (T.n || 0) + 1; output(0).on(); })
      .after(1000).then(function(){ output(0).off(); });
  `);
  e.input(0, true);
  e.advance(10000);
  assert.equal(e.T.n, 1, 'fired again from its own off step');
  assert.equal(e.output(0), false);
});

test('a delayed next step is armed before the current step runs', () => {
  const e = createEngine();
  e.load(`
    rule('pulse').when(input(0).isOn())
      .then(function(){ T.pending = _pending.length; output(0).on(); })
      .after(1000).then(function(){ output(0).off(); });
  `);
  e.input(0, true);
  assert.equal(e.T.pending, 1, 'the off step was not yet armed while step 0 ran (an interrupt there lost it)');
  e.advance(1000);
  assert.equal(e.output(0), false);
});

test('heldFor() with an edge condition is refused when the rule is built', () => {
  for (const cond of ["mqtt('a/b').once()", 'every(1000)', "cron('* * * * *')"]) {
    for (const order of ['heldFor-first', 'then-first']) {
      const e = createEngine();
      const src = order === 'heldFor-first'
        ? `rule('r').when(${cond}).heldFor(500).then(function(){});`
        : `rule('r').when(${cond}).then(function(){}).heldFor(500);`;
      assert.throws(() => e.load(src), /heldFor\(\) cannot be combined/, `${cond}, ${order}`);
    }
  }
  const e = createEngine();
  e.load(`rule('ok').when(input(0).isOn()).heldFor(500).then(function(){ T.n = 1; });`);
  e.input(0, true); e.advance(500);
  assert.equal(e.T.n, 1);
});
