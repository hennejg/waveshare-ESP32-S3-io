'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { createEngine } = require('./harness.js');

// ── mqtt(topic).publish() ─────────────────────────────────────────────────────

test('publish sends to the correct topic with default QoS 0 non-retained', () => {
  const e = createEngine();
  e.load(`rule('p').when(input(0).isOn()).then(function(){ mqtt('status/out').publish('hello'); });`);
  e.input(0, true);
  assert.equal(e.published.length, 1);
  assert.deepEqual(e.published[0], { topic: 'status/out', payload: 'hello', qos: 0, retain: false });
});

test('publish respects explicit qos and retain', () => {
  const e = createEngine();
  e.load(`rule('p').when(input(0).isOn()).then(function(){
    mqtt('sensors/temp').publish('22.5', { qos: 1, retain: true });
  });`);
  e.input(0, true);
  assert.equal(e.published.length, 1);
  assert.deepEqual(e.published[0], { topic: 'sensors/temp', payload: '22.5', qos: 1, retain: true });
});

test('publish coerces payload to string', () => {
  const e = createEngine();
  e.load(`rule('p').when(input(0).isOn()).then(function(){ mqtt('t').publish(42); });`);
  e.input(0, true);
  assert.equal(e.published[0].payload, '42');
});

test('publish fires on each rule activation', () => {
  const e = createEngine();
  e.load(`rule('p').when(input(0).isOn()).then(function(){ mqtt('t').publish('x'); });`);
  e.input(0, true);
  e.input(0, false);
  e.input(0, true);
  assert.equal(e.published.length, 2);
});

test('publish can forward an incoming mqtt payload', () => {
  const e = createEngine();
  e.load(`
    var src = mqtt('in/raw');
    rule('fwd').when(src).then(function(){
      mqtt('out/processed').publish(src.value, { qos: 2 });
    });
  `);
  e.publish('in/raw', 'sensor-data');
  assert.equal(e.published.length, 1);
  assert.deepEqual(e.published[0], { topic: 'out/processed', payload: 'sensor-data', qos: 2, retain: false });
});

test('publish with qos 2 and retain true', () => {
  const e = createEngine();
  e.load(`rule('p').when(input(0).isOn()).then(function(){
    mqtt('cfg/value').publish('ON', { qos: 2, retain: true });
  });`);
  e.input(0, true);
  assert.deepEqual(e.published[0], { topic: 'cfg/value', payload: 'ON', qos: 2, retain: true });
});

test('omitting opts object uses defaults', () => {
  const e = createEngine();
  e.load(`rule('p').when(input(0).isOn()).then(function(){ mqtt('t').publish('v'); });`);
  e.input(0, true);
  assert.equal(e.published[0].qos, 0);
  assert.equal(e.published[0].retain, false);
});
