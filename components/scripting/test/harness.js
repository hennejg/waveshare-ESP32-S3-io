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

function createEngine(opts) {
  opts = opts || {};
  const di    = new Array(8).fill(false);   // digital input state
  const dout  = new Array(8).fill(false);   // digital output state
  const prints = [];                        // captured print() output
  const T = {};                             // scratch object rules can write to (fire counters etc.)

  // ── deterministic fake clock (backs _set_timer / _clear_timer) ──
  let now = 0;
  let nextId = 1;
  const timers = new Map();   // id -> { at, fn }

  // Mirrors MAX_TIMERS in scripting.c: the device answers -1 once its timer
  // table is full. Unlimited by default; set it to exercise the refusal path.
  let maxTimers = opts.maxTimers !== undefined ? opts.maxTimers : Infinity;

  // Mirrors the device _time_valid() binding: cron stays suppressed until the clock is
  // real. Defaults true (legacy behaviour); pass { timeValid: false } to test suppression.
  let timeValid = opts.timeValid !== undefined ? !!opts.timeValid : true;

  let ledState   = { r: 0, g: 0, b: 0 };   // last led().set(...)
  let buzzerFreq = 0;                       // last buzzer().set(...) Hz (0 = off)
  const published = [];                     // messages sent via mqtt(topic).publish()
  // IO1/RXD/TXD pads 1..3: which are PWM outputs (as set in the web UI), their
  // PWM state, and analog readings (mV, null = pad not analog).
  const pwmPads  = { 1: null, 2: null, 3: null };   // null = not a PWM output, else {duty, freq}
  const analogMv = { 1: null, 2: null, 3: null };

  const sandbox = {
    _di_get(ch)        { return !!di[ch]; },
    _dout_set(ch, v)   { dout[ch] = !!v; },
    _dout_get(ch)      { return !!dout[ch]; },
    _dout_toggle(ch)   { dout[ch] = !dout[ch]; },
    // Mirrors js_aux_pwm_set in bindings.c, checks and all, so that a rule the
    // device would refuse fails here too.
    _aux_pwm_set(pad, duty, freq) {
      if (!(pad >= 1 && pad <= 3) || pad !== Math.trunc(pad)) throw new RangeError('pad must be 1, 2 or 3');
      duty = Number(duty);
      if (!(duty >= 0 && duty <= 100)) throw new RangeError('pwm duty must be 0-100 (%)');
      if (freq !== undefined) {
        freq = Number(freq);
        if (freq !== 0 && (!(freq >= 5 && freq <= 1000000) || freq !== Math.trunc(freq)))
          throw new RangeError('pwm freq must be a whole number of Hz from 5 to 1000000');
      }
      if (!pwmPads[pad]) throw new Error('pad ' + pad + ' is not set to PWM output');
      pwmPads[pad] = { duty: Math.round(duty * 10) / 10, freq: freq ? freq : pwmPads[pad].freq };
    },
    _aux_pwm_get(pad)    { return pwmPads[pad] ? { duty: pwmPads[pad].duty, freq: pwmPads[pad].freq } : null; },
    _aux_analog_get(pad) { return analogMv[pad] === undefined ? null : analogMv[pad]; },
    _led_set(r, g, b)  { ledState = { r: r | 0, g: g | 0, b: b | 0 }; },
    _buzzer_set(freq)  { buzzerFreq = freq | 0; },
    _mqtt_publish(topic, payload, qos, retain) {
      published.push({ topic, payload, qos, retain });
      return published.length;  // fake msg_id
    },
    print(...a)        { prints.push(a.join(' ')); },
    _set_timer(ms, fn) {
      if (timers.size >= maxTimers) return -1;   // table full — as the device does
      const id = nextId++; timers.set(id, { at: now + Math.max(0, ms), fn }); return id;   // no int32 truncation, as on the device
    },
    _clear_timer(id)   { timers.delete(id); },
    _now()             { return now; },        // virtual wall-clock (epoch ms) for cron
    _time_valid()      { return timeValid; },  // clock-validity gate for cron
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
    analog(pad, mv) { analogMv[pad] = mv; sandbox._on_analog(pad, mv); },   // new reading + fire
    analogGone(pad) { analogMv[pad] = null; sandbox._on_analog(pad, null); }, // pad leaves analog mode
    setPwmPad(pad, on, freq) { pwmPads[pad] = on ? { duty: 0, freq: freq || 1000 } : null; },
    pwmState(pad)   { return pwmPads[pad]; },
    reload(script)  { sandbox._reset_rules(); if (script) this.load(script); },

    // ── observe ──
    output(ch)      { return !!dout[ch]; },
    setOutput(ch,v) { dout[ch] = !!v; },                            // seed output without an event
    prints,
    published,
    T,
    pendingTimers() { return timers.size; },
    setMaxTimers(n) { maxTimers = n; },   // simulate the table filling up / freeing
    advance,
    now() { return now; },
    setClock(ms) { now = ms; },   // set the virtual epoch (call before loading cron rules)
    // Mark the clock valid and re-arm cron — mirrors the device EVT_TIME_SYNC path
    // (scripting_on_time_sync → _on_time_sync). Call after an SNTP-style clock step.
    timeSync()     { timeValid = true; sandbox._on_time_sync(); },
    setTimeValid(v){ timeValid = !!v; },
    // Component observation + fieldbus activity injection (mirror the device hooks).
    ledState()     { return ledState; },     // { r, g, b } of the last led().set
    buzzerFreq()   { return buzzerFreq; },    // Hz of the last buzzer().set (0 = off)
    activity(key)  { sandbox._on_activity(key); },   // 'modbus' | 'can'
  };
}

module.exports = { createEngine };
