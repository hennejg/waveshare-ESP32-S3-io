'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { createEngine } = require('./harness.js');

test('bare input always matches; .isOn/.isOff gate', () => {
  const e = createEngine();
  e.load(`rule('r').when(input(0)).then(function(){ T.bare = (T.bare||0)+1; });`);
  e.load(`rule('on').when(input(1).isOn()).then(function(){ T.on = (T.on||0)+1; });`);

  e.input(0, true);  assert.equal(e.T.bare, 1);   // bare fires on the change
  e.input(0, false); assert.equal(e.T.bare, 2);   // and on the next change too

  e.input(1, false); assert.equal(e.T.on, undefined); // isOn does not match when low
  e.input(1, true);  assert.equal(e.T.on, 1);         // matches when high
});

test('isOff is the inverse of isOn', () => {
  const e = createEngine();
  e.load(`rule('off').when(input(2).isOff()).then(function(){ output(2).on(); });`);
  e.input(2, true);  assert.equal(e.output(2), false);
  e.input(2, false); assert.equal(e.output(2), true);
});

test('input .value / .get read the live level inside an action', () => {
  const e = createEngine();
  e.load(`rule('copy').when(input(0)).then(function(){ output(0).set(input(0).value); output(1).set(input(0).get()); });`);
  e.input(0, true);  assert.equal(e.output(0), true);  assert.equal(e.output(1), true);
  e.input(0, false); assert.equal(e.output(0), false); assert.equal(e.output(1), false);
});

test('output actuator: set/on/off/toggle return the resulting level', () => {
  const e = createEngine();
  e.load(`
    rule('r').when(input(0).isOn()).then(function(){
      T.set    = output(0).set(true);
      T.off    = output(0).off();
      T.on     = output(0).on();
      T.toggle = output(0).toggle();
    });
  `);
  e.input(0, true);
  assert.equal(e.T.set, true);
  assert.equal(e.T.off, false);
  assert.equal(e.T.on, true);
  assert.equal(e.T.toggle, false);
});

test('output as a fact: isOn/isOff gate in when()', () => {
  const e = createEngine();
  e.setOutput(3, true);
  e.load(`rule('g').when(input(0).isOn(), output(3).isOn()).then(function(){ output(4).on(); });`);
  e.input(0, true);
  assert.equal(e.output(4), true);    // DO3 was high → gate open

  const e2 = createEngine();
  e2.setOutput(3, false);
  e2.load(`rule('g').when(input(0).isOn(), output(3).isOn()).then(function(){ output(4).on(); });`);
  e2.input(0, true);
  assert.equal(e2.output(4), false);  // DO3 low → gate closed
});

test('mqtt: bare matches once seen; .is(predicate) filters; .value holds payload', () => {
  const e = createEngine();
  e.load(`
    var t = mqtt('go');
    rule('bare').when(t).then(function(){ T.bare = (T.bare||0)+1; T.val = t.value; });
    rule('on').when(mqtt('go').is(function(m){ return m === 'ON'; })).then(function(){ T.on = (T.on||0)+1; });
  `);
  // bare hasn't matched before any message
  e.publish('go', 'ON');
  assert.equal(e.T.bare, 1);
  assert.equal(e.T.val, 'ON');
  assert.equal(e.T.on, 1);

  e.publish('go', 'OFF');
  assert.equal(e.T.bare, 2);          // bare matches any payload
  assert.equal(e.T.on, 1);            // predicate rejects 'OFF'
});

test('mqtt .once() is edge (per message), bare is level (re-fires on input changes)', () => {
  const e = createEngine();
  e.load(`
    rule('level').when(mqtt('go').is(function(m){return m==='ON';}), input(1).isOn())
                 .then(function(){ T.level = (T.level||0)+1; });
    rule('edge').when(mqtt('go').is(function(m){return m==='ON';}).once(), input(1).isOn())
                .then(function(){ T.edge = (T.edge||0)+1; });
  `);
  e.setInput(1, true);
  e.publish('go', 'ON');               // both fire once
  assert.equal(e.T.level, 1);
  assert.equal(e.T.edge, 1);

  e.input(1, false); e.input(1, true); // input churn while 'ON' is still held
  assert.equal(e.T.level, 2);          // level re-fires (payload still matches)
  assert.equal(e.T.edge, 1);           // edge does NOT (no new message)

  e.publish('go', 'ON');               // a fresh message
  assert.equal(e.T.edge, 2);           // edge fires again
});

test('conditions are immutable (copy-on-write): a base input can be branched', () => {
  const e = createEngine();
  e.load(`
    var di = input(5);
    rule('on').when(di.isOn()).then(function(){ T.on = (T.on||0)+1; });
    rule('off').when(di.isOff()).then(function(){ T.off = (T.off||0)+1; });
  `);
  e.input(5, true);  assert.equal(e.T.on, 1);  assert.equal(e.T.off, undefined);
  e.input(5, false); assert.equal(e.T.off, 1); assert.equal(e.T.on, 1);
});

test('mqtt copy-on-write: .is().once() preserves both constraints', () => {
  const e = createEngine();
  // confirm both order chainings keep predicate + edge
  const probe = e.evalIn(`
    (function(){
      var base = mqtt('t');
      var a = base.is(function(m){return m==='X';}).once();
      var b = base.once().is(function(m){return m==='Y';});
      return JSON.stringify({
        base_pristine: (base._predicate === null && base._once === false),
        a: { once: a._once, pred: typeof a._predicate },
        b: { once: b._once, pred: typeof b._predicate }
      });
    })()
  `);
  assert.deepEqual(JSON.parse(probe), {
    base_pristine: true,
    a: { once: true, pred: 'function' },
    b: { once: true, pred: 'function' },
  });
});
