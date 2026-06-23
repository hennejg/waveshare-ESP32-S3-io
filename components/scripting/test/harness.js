'use strict';
// Test harness for the rule engine.
//
// Loads the *real* components/scripting/dsl.js into an isolated V8 context with mock
// host bindings (the same surface the device/simulator provide: _di_get, _dout_set,
// _dout_get, print, _set_timer, _clear_timer) and a deterministic fake clock, so
// time-based rules can be tested without real waiting.
//
// Each createEngine() call is a fresh engine: new context, new rule set, new I/O.

const fs   = require('node:fs');
const path = require('node:path');
const vm   = require('node:vm');

const DSL_SRC = fs.readFileSync(path.join(__dirname, '..', 'dsl.js'), 'utf8');

function createEngine() {
  const di    = new Array(8).fill(false);   // digital input state
  const dout  = new Array(8).fill(false);   // digital output state
  const prints = [];                        // captured print() output
  const T = {};                             // scratch object rules can write to (fire counters etc.)

  // ── deterministic fake clock (backs _set_timer / _clear_timer) ──
  let now = 0;
  let nextId = 1;
  const timers = new Map();   // id -> { at, fn }

  const sandbox = {
    _di_get(ch)        { return !!di[ch]; },
    _dout_set(ch, v)   { dout[ch] = !!v; },
    _dout_get(ch)      { return !!dout[ch]; },
    print(...a)        { prints.push(a.join(' ')); },
    _set_timer(ms, fn) { const id = nextId++; timers.set(id, { at: now + (ms | 0), fn }); return id; },
    _clear_timer(id)   { timers.delete(id); },
    _now()             { return now; },   // virtual wall-clock (epoch ms) for cron
    T,
  };

  const ctx = vm.createContext(sandbox);
  vm.runInContext(DSL_SRC, ctx, { filename: 'dsl.js' });   // defines rule/input/output/mqtt/_on_*/... as globals

  // Advance the fake clock by `ms`, firing due timers in chronological order.
  // A firing callback may schedule new timers; the loop picks those up too.
  function advance(ms) {
    const target = now + ms;
    for (;;) {
      let best = null;
      for (const [id, t] of timers) {
        if (t.at <= target &&
            (best === null || t.at < best.at || (t.at === best.at && id < best.id))) {
          best = { id, at: t.at, fn: t.fn };
        }
      }
      if (!best) break;
      timers.delete(best.id);
      now = best.at;
      best.fn();
    }
    now = target;
  }

  return {
    // ── define rules (runs DSL/user script in the engine context) ──
    load(script)  { return vm.runInContext(script, ctx, { filename: 'rules.js' }); },
    // ── read/evaluate an expression inside the engine (white-box checks) ──
    evalIn(expr)  { return vm.runInContext(expr, ctx); },

    // ── drive events (mirror the device/simulator entry points) ──
    input(ch, v)    { di[ch] = !!v; sandbox._on_input(ch, !!v); },   // set DI + fire
    setInput(ch, v) { di[ch] = !!v; },                              // set DI without firing
    publish(t, p)   { sandbox._on_mqtt(t, p); },                    // fire _on_mqtt
    reload(script)  { sandbox._reset_rules(); if (script) this.load(script); },

    // ── observe ──
    output(ch)      { return !!dout[ch]; },
    setOutput(ch,v) { dout[ch] = !!v; },                            // seed output without an event
    prints,
    T,
    pendingTimers() { return timers.size; },
    advance,
    now() { return now; },
    setClock(ms) { now = ms; },   // set the virtual epoch (call before loading cron rules)
  };
}

module.exports = { createEngine };
