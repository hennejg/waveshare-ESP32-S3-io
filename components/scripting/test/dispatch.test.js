'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { createEngine } = require('./harness.js');

test('an input event only re-evaluates rules referencing that input', () => {
  const e = createEngine();
  e.load(`
    rule('a').when(input(4)).then(function(){ T.a = (T.a||0)+1; });
    rule('b').when(input(7)).then(function(){ T.b = (T.b||0)+1; });
  `);
  e.input(4, true);
  assert.equal(e.T.a, 1);
  assert.equal(e.T.b, undefined);   // input(7) rule untouched by an input(4) event

  e.input(7, true);
  assert.equal(e.T.a, 1);           // input(4) rule untouched by an input(7) event
  assert.equal(e.T.b, 1);
});

test('computed facts: bare input is not wildcard, predicate rule is wildcard', () => {
  const e = createEngine();
  e.load(`
    rule('targeted').when(input(0).isOn(), output(2).isOff()).then(function(){});
    rule('wild').when(function(){ return input(0).value; }).then(function(){});
  `);
  const facts = JSON.parse(e.evalIn(`JSON.stringify({
    targeted: { facts: Object.keys(_rules[0]._facts), wild: _rules[0]._wildcard },
    wild:     { facts: Object.keys(_rules[1]._facts), wild: _rules[1]._wildcard }
  })`));
  assert.deepEqual(facts.targeted.facts.sort(), ['input:0', 'output:2']);
  assert.equal(facts.targeted.wild, false);
  assert.deepEqual(facts.wild.facts, []);
  assert.equal(facts.wild.wild, true);
});

test('a wildcard (predicate) rule is re-evaluated on every event', () => {
  const e = createEngine();
  e.load(`rule('w').when(function(){ return input(2).value || input(3).value; }).then(function(){ T.n = (T.n||0)+1; });`);

  e.input(2, true);  assert.equal(e.T.n, 1);   // fires via input(2)
  e.input(2, false); // predicate false again (n unchanged because rule body still runs? it runs only when matching)
  assert.equal(e.T.n, 1);
  e.input(3, true);  assert.equal(e.T.n, 2);   // also fires via input(3) — both inputs reach a wildcard rule
});

test('writing an output is an event that re-evaluates rules gated on that output', () => {
  const e = createEngine();
  e.load(`
    rule('writer').when(input(0).isOn()).then(function(){ output(0).on(); });
    rule('follower').when(output(0).isOn()).then(function(){ output(1).on(); });
  `);
  e.input(0, true);
  assert.equal(e.output(0), true);
  assert.equal(e.output(1), true);   // follower reacted to DO0 going high
});

test('output writes do not cascade when no rule watches outputs', () => {
  const e = createEngine();
  e.load(`rule('r').when(input(0).isOn()).then(function(){ output(5).toggle(); });`);
  assert.equal(e.evalIn('_watchOutputs'), false);
  e.input(0, true);
  assert.equal(e.output(5), true);
  assert.equal(e.evalIn('_cascade'), 0);   // no emit happened
});
