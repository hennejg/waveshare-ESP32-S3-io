'use strict';
// Channel numbers reach the host as integers. The DSL builds its dispatch keys
// from the same value, so a non-integer would read one pin and then listen for
// a fact nothing emits.

const { test } = require('node:test');
const assert   = require('node:assert');
const { createEngine } = require('./harness');

test('a fractional channel is rejected instead of silently never firing', () => {
  const e = createEngine();
  assert.throws(
    () => e.load(`rule('x').when(input(3.7).isOn()).then(function(){ T.n = 1; });`),
    /whole number/);
});

test('a whole channel still works', () => {
  const e = createEngine();
  e.load(`rule('x').when(input(3).isOn()).then(function(){ T.n = (T.n || 0) + 1; });`);
  e.input(3, true);
  assert.equal(e.T.n, 1);
});

test('output channels are checked the same way', () => {
  const e = createEngine();
  assert.throws(
    () => e.load(`rule('y').when(output(2.5).isOn()).then(function(){});`),
    /whole number/);
});
