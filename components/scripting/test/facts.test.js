'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { createEngine } = require('./harness.js');

test('fact holds its value; get()/value read it; set() returns the new value', () => {
  const e = createEngine();
  e.load(`
    var f = fact(7);
    rule('r').when(input(0).isOn()).then(function(){
      T.initial = f.value;
      T.ret = f.set(42);
      T.afterGet = f.get();
    });
  `);
  e.input(0, true);
  assert.equal(e.T.initial, 7);
  assert.equal(e.T.ret, 42);
  assert.equal(e.T.afterGet, 42);
});

test('.is(predicate) gates a rule on the fact value', () => {
  const e = createEngine();
  e.load(`
    var level = fact(0);
    rule('bump').when(input(0).isOn()).then(function(){ level.set(10); });
    rule('high').when(level.is(function(v){ return v >= 5; })).then(function(){ output(0).on(); });
  `);
  e.input(0, true);            // bump sets level=10 → re-evaluates 'high'
  assert.equal(e.output(0), true);
});

test('.is(constant) matches by equality', () => {
  const e = createEngine();
  e.load(`
    var mode = fact('idle');
    rule('go').when(input(0).isOn()).then(function(){ mode.set('run'); });
    rule('running').when(mode.is('run')).then(function(){ output(0).on(); });
  `);
  e.input(0, true);
  assert.equal(e.output(0), true);
});

test('the calculated-fact pattern: a timer updates a fact, other rules react', () => {
  const e = createEngine();
  e.load(`
    var sun = fact(20);
    rule('light on at night').when(sun.is(function(e){ return e < 5; })).then(function(){ output(1).on(); });
    rule('light off at sunrise').when(sun.is(function(e){ return e > 10; })).then(function(){ output(1).off(); });
    rule('update').when(every(60000)).then(function(){ sun.set(T.next); });
  `);
  e.T.next = 0;  e.advance(60000); assert.equal(e.output(1), true);   // night → light on
  e.T.next = 20; e.advance(60000); assert.equal(e.output(1), false);  // day   → light off
});

test('set() emits only on an actual change', () => {
  const e = createEngine();
  e.load(`
    var f = fact(0);
    rule('w').when(f.is(function(v){ return v > 0; })).then(function(){ T.n = (T.n||0)+1; });
    rule('a').when(input(0).isOn()).then(function(){ f.set(5); });
    rule('b').when(input(1).isOn()).then(function(){ f.set(5); });  // same value
  `);
  e.input(0, true); assert.equal(e.T.n, 1);   // 0→5 emits → w fires
  e.input(1, true); assert.equal(e.T.n, 1);   // 5→5 no change → no emit → w not re-fired
});

test('each fact is its own dispatch fact', () => {
  const e = createEngine();
  e.load(`
    var a = fact(0), b = fact(0);
    rule('wa').when(a.is(function(v){ return v > 0; })).then(function(){ T.a = (T.a||0)+1; });
    rule('wb').when(b.is(function(v){ return v > 0; })).then(function(){ T.b = (T.b||0)+1; });
    rule('seta').when(input(0).isOn()).then(function(){ a.set(1); });
  `);
  e.input(0, true);
  assert.equal(e.T.a, 1);
  assert.equal(e.T.b, undefined);   // setting a does not re-evaluate the rule gated on b
});

test('a bare fact in when() always matches but re-evaluates on change', () => {
  const e = createEngine();
  e.load(`
    var f = fact(0);
    rule('any').when(f).then(function(){ T.n = (T.n||0)+1; });
    rule('inc').when(input(0)).then(function(){ f.set(f.get() + 1); });
  `);
  e.input(0, true);  assert.equal(e.T.n, 1);   // f 0→1 → 'any' fires
  e.input(0, false); assert.equal(e.T.n, 2);   // f 1→2 → fires again
});

test('fact change cascades are bounded by the cascade cap', () => {
  const e = createEngine();
  e.load(`
    var f = fact(0);
    rule('loop').when(f).then(function(){ T.n = (T.n||0)+1; f.set(f.get() + 1); });
    rule('kick').when(input(0).isOn()).then(function(){ f.set(1); });
  `);
  const cap = e.evalIn('CASCADE_CAP');
  e.input(0, true);
  assert.equal(e.T.n, cap);   // self-feeding fact loop stops at the cap
});

test('.noLoop() applies to fact self-loops (one fire)', () => {
  const e = createEngine();
  e.load(`
    var f = fact(0);
    rule('loop').when(f).noLoop().then(function(){ T.n = (T.n||0)+1; f.set(f.get() + 1); });
    rule('kick').when(input(0).isOn()).then(function(){ f.set(1); });
  `);
  e.input(0, true);
  assert.equal(e.T.n, 1);   // its own fact change does not re-trigger it
});
