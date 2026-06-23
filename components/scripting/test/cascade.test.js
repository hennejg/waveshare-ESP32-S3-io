'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { createEngine } = require('./harness.js');

test('output -> output chaining works', () => {
  const e = createEngine();
  e.load(`
    rule('fault->alarm').when(output(7).isOn()).then(function(){ output(6).on(); });
    rule('fault->shed').when(output(7).isOn()).then(function(){ output(0).off(); });
    rule('raise').when(input(0).isOn()).then(function(){ output(7).on(); });
  `);
  e.setOutput(0, true);   // a load is on
  e.input(0, true);       // raise the fault
  assert.equal(e.output(7), true);
  assert.equal(e.output(6), true);    // alarm
  assert.equal(e.output(0), false);   // load shed
});

test('the cascade cap bounds a self-feeding loop', () => {
  const e = createEngine();
  e.load(`
    rule('loop').when(output(0)).then(function(){ T.n = (T.n||0)+1; output(0).toggle(); });
    rule('kick').when(input(0).isOn()).then(function(){ output(0).on(); });
  `);
  const cap = e.evalIn('CASCADE_CAP');
  e.input(0, true);
  assert.equal(e.T.n, cap);   // fired exactly CASCADE_CAP times, then the cascade stopped
  assert.ok(e.prints.some(p => /cascade limit/.test(p)), 'logs a cascade-limit warning');
});

test('.noLoop() stops a rule re-firing from its own consequence (one fire)', () => {
  const e = createEngine();
  e.load(`
    rule('loop').when(output(0)).noLoop().then(function(){ T.n = (T.n||0)+1; output(0).toggle(); });
    rule('kick').when(input(0).isOn()).then(function(){ output(0).on(); });
  `);
  e.input(0, true);
  assert.equal(e.T.n, 1);     // exactly once — its own toggle does not re-trigger it
});

test('.noLoop() still fires on later, independent events', () => {
  const e = createEngine();
  e.load(`rule('nl').when(input(0)).noLoop().then(function(){ T.n = (T.n||0)+1; });`);
  e.input(0, true);  assert.equal(e.T.n, 1);
  e.input(0, false); assert.equal(e.T.n, 2);   // independent event → fires again
});

test('.noLoop() does not block other rules reacting to its writes', () => {
  const e = createEngine();
  e.load(`
    rule('writer').when(input(0).isOn()).noLoop().then(function(){ output(0).on(); });
    rule('reader').when(output(0).isOn()).then(function(){ output(1).on(); });
  `);
  e.input(0, true);
  assert.equal(e.output(0), true);
  assert.equal(e.output(1), true);   // reader fired off the writer's output
});
