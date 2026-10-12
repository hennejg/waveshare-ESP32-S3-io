// Rule Engine DSL — evaluated once at scripting startup.
// C bindings available as globals:
//   _di_get(ch)          → bool    read digital input 0-7, 0-10 with the header pads
//   _dout_set(ch, bool)  → void    write digital output 0-7
//   _dout_get(ch)        → bool    read current digital output state
//   _led_set(r, g, b)    → void    drive the RGB LED (IO mode only)
//   _buzzer_set(freqHz)  → void    continuous buzzer tone (0 = off)
//   print(str)           → void    log to ESP console
//
// Public API exposed to user scripts:
//   rule(name).when(...conditions).then(fn)
//   rule(name).when(...).then(a).after(ms).then(b)  → b runs ms after a, regardless
//                                                      of conditions (scheduled pulse)
//   rule(name).when(...).heldFor(ms).then(fn)        → fn runs only if the conditions
//                                                      hold continuously for ms
//   rule(name).when(...).noLoop().then(fn)           → rule won't re-fire from its own
//                                                      consequence (Drools-style no-loop)
//   every(ms)            → time trigger: an edge event every ms (min 100ms; other
//                          when()-conditions gate it)
//   cron("m h dom mon dow") → time trigger on a 5-field cron schedule (local time; numeric fields)
//   fact(initial)        → synthetic value: .set(v) in then(), .is(fn|value) in when()
//                          (calculated by some rules, gated on by others);
//                          .isTrue()/.isFalse() gate on truthiness
//   watchdog(ms)         → liveness fact: .feed() keeps it alive, .isAlive()/.isExpired()
//                          gate on it (expires if not fed for ms) — for local fallback
//   mqtt(topic).stale(ms)→ watchdog auto-fed by messages on topic (matches when stale)
//   mqttConnected() / mqttDown() → MQTT broker connection state (host-fed)
//   mqtt(topic).is(fn)   → MqttCondition   (condition + value holder)
//   input(ch).is(bool)   → InputCondition  (.value / .get() read the live level)
//   output(ch)           → actuator { set(bool), on(), off(), toggle(), get(), value }
//                          (set/on/off/toggle return the resulting level) AND a fact
//                          { is(bool), isOn(), isOff() } usable in when()
//   led()                → actuator { set(r,g,b) | set("#rrggbb"), off() }
//   buzzer()             → actuator { set(freqHz), off() }  (continuous tone)
//   modbus(ms) / can(ms) → upstream command-health: auto-fed watchdogs, .isAlive() while
//                          commands arrive within ms, .isExpired() when the link goes quiet
//
// Condition semantics (Drools-like — a bare pattern matches, .is() narrows it):
//   input(ch)            → always matches; use input(ch).value in then()
//   input(ch).is(bool)   → matches when the input equals bool
//   input(ch).isOn()     → alias for .is(true);  .isOff() → .is(false)
//   mqtt(topic)          → matches once any message arrived on topic (level/held)
//   mqtt(topic).is(fn)   → matches when fn(payload) is truthy
//   mqtt(topic).once()   → edge: matches only on each message arrival, not while
//                          the payload is still held (chainable with .is())
//   when() also accepts a plain predicate function or a boolean.
//
// Conditions are immutable: input()/mqtt() build a base pattern and .is()/.isOn()/
// .isOff()/.once() each return a NEW narrowed pattern. A base can be branched safely
// — var di = input(5); di.isOn() and di.isOff() are independent, di stays unconstrained
// (and usable for di.value / di.get()).
//
// Dispatch: each input(ch), output(ch) and mqtt(topic) is a distinct fact. An event
// re-evaluates only the rules referencing that fact (so changing input(7) never
// re-fires a rule gated on input(4)). Writing an output that changes value is itself
// an event, so rules can chain off outputs; because outputs are written by rules, a
// per-trigger cascade budget bounds the chain so a feedback loop can't run away.
// Rules with an opaque condition (predicate fn / bare value) are treated as wildcard
// and re-evaluated on every event.

'use strict';

var _rules = [];

// ── Scheduler ────────────────────────────────────────────────────────────────
// Host-agnostic timers. The C host may provide _set_timer(ms,fn) → id and
// _clear_timer(id); otherwise we fall back to the browser's setTimeout/clearTimeout
// (the simulator). All live timer ids are tracked so a rule reload can cancel any
// pending .after()/.heldFor() callbacks. (typeof on an undeclared name is safe.)
var _pending = [];
function _schedule(ms, fn) {
    var id;
    var wrapped = function() {
        var k = _pending.indexOf(id);
        if (k >= 0) _pending.splice(k, 1);
        _resetCascade();   // a timer firing is a fresh trigger → fresh cascade budget
        // Its own re-arm first (an every() takes its slot straight back),
        // then whatever the full table refused gets what is left.
        try { fn(); } finally { _retry_arms(); }
    };
    if (typeof _set_timer === 'function')      id = _set_timer(ms, wrapped);
    else if (typeof setTimeout === 'function') id = setTimeout(wrapped, ms);
    else { fn(); return null; }   // no scheduler available — degrade to immediate
    // _set_timer answers -1 when the timer table is full. Callers store the
    // result in _timer and guard on `!= null`, and -1 != null is true in JS —
    // so returning it would mark the rule as armed forever and stop it from
    // ever trying again. Report "not scheduled" instead.
    if (id == null || id < 0) { _refused = true; return null; }
    _pending.push(id);
    return id;
}
function _unschedule(id) {
    if (id == null) return;
    var k = _pending.indexOf(id); if (k >= 0) _pending.splice(k, 1);
    if (typeof _clear_timer === 'function')      _clear_timer(id);
    else if (typeof clearTimeout === 'function') clearTimeout(id);
    _retry_arms();
}

// Timers the full table refused. every(), cron(), a watchdog, the later
// steps of .after() -- and a heldFor() whose condition simply stays true --
// have no next event of their own: refused once, an every() never ticked again, a
// watchdog fed while the table was full never expired -- the case it exists
// for -- and an .after() that switches an output off was dropped. They wait
// here, in order, and are tried again when a slot may have freed (a timer
// fires or is cleared, an event comes in): late rather than never. An entry
// answers whether the table took it; the first one refused ends the pass,
// since the table is full again -- trying the rest would only be refused too.
var _retryArms = [];
var _refused = false;        // set by _schedule() when the table said no
var _retrying = false;       // a pass is running (a clear inside it must not start another)
var RETRY_CAP = 256;         // every/cron/watchdog queue once each; .after() steps
var _retryWarned = false;    // pile up under sustained overload -- not without bound
// first: an every(), cron() or watchdog -- one entry each, and what keeps a
// rule set running -- goes ahead of the .after() steps that may pile up.
function _arm_later(fn, first) {
    if (first) { _retryArms.unshift(fn); return; }
    if (_retryArms.length >= RETRY_CAP) {
        if (!_retryWarned) { _retryWarned = true; print('[rules] timer table full and ' + RETRY_CAP + ' timers waiting: further ones dropped'); }
        return;
    }
    _retryArms.push(fn);
}
function _retry_arms() {
    if (_retrying || !_retryArms.length) return;
    _retrying = true;
    try {
        while (_retryArms.length && _retryArms[0]()) _retryArms.shift();
    } finally { _retrying = false; }
    if (_retryArms.length < RETRY_CAP) _retryWarned = false;
}
function _schedule_retrying(ms, fn) {
    if (_schedule(ms, fn) == null)
        _arm_later(function() { return _schedule(ms, fn) != null; });
}
// For an every(), cron() or watchdog whose own arm the table refused: queued
// once (o._waiting), and taken off the queue once rearm() got past the table
// -- armed, or nothing to arm (cron without a valid clock).
function _rearm_when_free(o, rearm) {
    if (o._timer != null || o._waiting) return;
    o._waiting = true;
    _arm_later(function() {
        _refused = false;
        if (o._timer == null) rearm.call(o);   // a refusal in there finds o._waiting set
        if (_refused) return false;
        o._waiting = false;
        return true;
    }, true);
}
function _cancel_all_timers() { while (_pending.length) _unschedule(_pending[0]); }

// ── Output as a fact + cascade guard ──────────────────────────────────────────
// Writing an output that actually changes emits an 'output:ch' event, so rules gated
// on the output re-evaluate. Outputs are driven BY rules, so this can feed back on
// itself; a per-trigger cascade budget bounds the chain (each external event or timer
// firing resets it). When the budget is exhausted the cascade stops with a logged
// warning instead of looping forever — important on-device, where an unbounded loop
// would block the scripting task and trip the watchdog.
//
// CASCADE_CAP is the depth -- how many hops one change may chain through, as
// documented. It used to count every change of the event instead: one action
// setting 20 watched facts fired only 16 rules, and the 17th top-level
// fact.set() of a script was dropped while it loaded. The total cap is the
// backstop for wide fan-out, which depth alone does not bound.
var CASCADE_CAP = 16;
var CASCADE_TOTAL_CAP = 256;
var _cascade = 0;            // current depth
var _cascadeTotal = 0;       // changes emitted for this triggering event
var _cascadeWarned = false;
var _watchOutputs = false;   // does any rule reference an output fact / is wildcard?

function _resetCascade() { _cascade = 0; _cascadeTotal = 0; _cascadeWarned = false; }

function _emit(factKey) {
    if (_cascade >= CASCADE_CAP || _cascadeTotal >= CASCADE_TOTAL_CAP) {
        if (!_cascadeWarned) {
            _cascadeWarned = true;
            print('[rules] cascade limit (' + (_cascade >= CASCADE_CAP ? CASCADE_CAP + ' hops'
                  : CASCADE_TOTAL_CAP + ' changes') + ') reached — stopping (output feedback loop?)');
        }
        return;
    }
    _cascade++;
    _cascadeTotal++;
    try { _fire_matching(factKey, true); }   // cascade pass — does NOT reset the budget
    finally { _cascade--; }                  // skipped by the budget's interrupt; the next event resets
}

// Drive an output through the host binding; emit only on an actual change, and only
// when some rule is watching outputs (keeps the common no-watcher case free of cost).
// v === null flips the output in one host call, under the output lock;
// reading it here and setting the opposite lost one of two concurrent toggles.
function _drive_output(ch, v) {
    var before = _dout_get(ch);
    if (v === null) _dout_toggle(ch); else _dout_set(ch, v);
    var after = _dout_get(ch);
    if (after !== before && _watchOutputs) _emit('output:' + ch);
    return after;
}

// Called on rule reload (EVT_RELOAD / simulator "Apply") — drop the rule set and
// any in-flight timers so stale callbacks can't fire against the old rules.
function _reset_rules() {
    _retryArms = [];           // refused arms of the old rules -- before the clears below retry them
    _cancel_all_timers();
    _rules = [];
    _cronTriggers = [];        // timers already cancelled above; drop the old trigger registry
    _mqttIndex = Object.create(null);   // topic index of the old rules
    _activityWatchdogs = {};   // drop modbus()/can() health watchdogs from the old rule set
    _watchOutputs = false;
    _passes = [];              // a cut-short pass of the old rules has nothing left to visit
    _heldCheck = null;
    _resetCascade();
}

// ── MqttCondition ────────────────────────────────────────────────────────────

function MqttCondition(topic) {
    this.topic = topic;
    this._predicate = null;
    this.value = null;   // last received payload — readable in then() body
    this._seen = false;
    this._once = false;  // edge mode (see .once())
    this._armed = false; // true only during the _fire_matching pass a message triggered
}
// Constraint setters are copy-on-write: they return a NEW narrowed pattern and
// never mutate the receiver, so a base pattern can be branched safely.
MqttCondition.prototype._clone = function() {
    var c = new MqttCondition(this.topic);
    c._predicate = this._predicate;
    c._once      = this._once;
    return c;   // runtime state (value/_seen/_armed) starts fresh
};
MqttCondition.prototype.is = function(predicate) {
    var c = this._clone();
    c._predicate = predicate;
    return c;
};
// Edge-trigger: match only on the arrival of a (matching) message, not on later
// re-evaluations (e.g. input changes) while the same payload is still held.
MqttCondition.prototype.once = function() {
    var c = this._clone();
    c._once = true;
    return c;
};
MqttCondition.prototype._check = function() {
    // level-triggered gates on "ever seen"; edge gates on "armed this pass"
    if (!(this._once ? this._armed : this._seen)) return false;
    if (!this._predicate) return true;
    return !!this._predicate(this.value);
};
// action: publish a message to the MQTT broker.
// opts: { qos: 0|1|2, retain: bool } — both optional, defaults to QoS 0 non-retained.
// Returns the broker message-id (>= 0) on success, or -1 if not connected / no broker.
MqttCondition.prototype.publish = function(payload, opts) {
    opts = opts || {};
    var qos    = (typeof opts.qos    === 'number')  ? (opts.qos | 0) : 0;
    var retain = (typeof opts.retain === 'boolean') ? opts.retain    : false;
    return _mqtt_publish(this.topic, String(payload), qos, retain);
};

// ── InputCondition ───────────────────────────────────────────────────────────

// Channel numbers reach the host as integers; keep the DSL's own bookkeeping
// in step so a fact key cannot drift away from the pin that was read.
function _channel(ch, what) {
    var n = Number(ch);
    if (!isFinite(n) || Math.floor(n) !== n)
        throw new RangeError(what + ' channel must be a whole number, got ' + ch);
    // Refused when the rule is built, before a fact key like 'input:4294967296'
    // exists that no event ever matches; the bindings check the real range.
    if ((what === 'input' || what === 'output') && (n < 0 || n > 255))
        throw new RangeError(what + ' channel out of range: ' + ch);   // pads: pad_arg() in C
    return n;
}

// A rule's error, logged without throwing again: '' + e throws for a Symbol
// and for an object whose toString() throws, and an exception escaping the
// catch block left the pass on _passes and the rule's _firing set -- every
// later event pushed another stale pass, and a noLoop rule never ran again.
function _log_rule_error(r, what, e) {
    var name, msg;
    try { name = String(r.name); } catch (_) { name = '?'; }
    try { msg = String(e); } catch (_) { msg = '(error that cannot be printed)'; }
    try { print('[rule:' + name + '] ' + what + ': ' + msg); } catch (_) {}
}

function InputCondition(ch) {
    // Normalise: the host truncates to an integer, but the dispatch key is built
    // from this value. input(3.7) would read DI3 and then wait for 'input:3.7',
    // a fact nothing ever emits -- the rule is registered and never fires.
    this.channel = _channel(ch, 'input');
    this._expected = undefined;
}
// Copy-on-write: returns a NEW narrowed pattern, never mutates the receiver, so
// `var di = input(5)` can be branched into di.isOn() / di.isOff() independently.
InputCondition.prototype.is = function(expected) {
    var c = new InputCondition(this.channel);
    c._expected = expected;
    return c;
};
InputCondition.prototype.isOn  = function() { return this.is(true);  };
InputCondition.prototype.isOff = function() { return this.is(false); };
// The level an input condition is checked against comes from the event
// stream, not from the live pin: events are queued, and by the time a LOW is
// processed the pin may be HIGH again. Checking the live level then made a
// late LOW look like HIGH, and a running heldFor() timer that the LOW should
// have cancelled kept going. .value and .get() stay live -- they are for
// then() bodies, which want the pin as it is now.
var _di_seen = {};   // channel -> last level delivered by _on_input()
InputCondition.prototype._check = function() {
    var cur = (this.channel in _di_seen) ? _di_seen[this.channel] : _di_get(this.channel);
    if (this._expected === undefined) return true;
    return cur === this._expected;
};
// Live input level — readable in then() bodies as a property or a method:
//   input(0).value   /   input(0).get()
Object.defineProperty(InputCondition.prototype, 'value', {
    get: function() { return _di_get(this.channel); }
});
InputCondition.prototype.get = function() { return _di_get(this.channel); };

// ── pwm(pad) / analog(pad) — the IO1/RXD/TXD header pads, numbered 1..3 ──────
// Which pad does what is set in the web UI; these only act on a pad in that mode.
// pwm(pad).set(duty%, freq?) changes a running PWM output (not stored: a reboot
// starts from the configured values); .duty / .freq / .get() read it back.
// Pads are 1..3; checked here so that a typo fails where it is written, in
// the simulator and the tests as on the device.
function _pad(pad, what) {
    var n = _channel(pad, what);
    if (n < 1 || n > 3) throw new RangeError(what + ' pad must be 1, 2 or 3, got ' + pad);
    return n;
}
function PwmChannel(pad) { this.pad = _pad(pad, 'pwm'); }
PwmChannel.prototype.set = function(duty, freq) {
    _aux_pwm_set(this.pad, Number(duty), freq === undefined ? undefined : Number(freq));
    return this.get();
};
PwmChannel.prototype.off = function() { return this.set(0); };
PwmChannel.prototype.get = function() { return _aux_pwm_get(this.pad); };   // {duty, freq} or null
Object.defineProperty(PwmChannel.prototype, 'duty', {
    get: function() { var p = _aux_pwm_get(this.pad); return p ? p.duty : null; }
});
Object.defineProperty(PwmChannel.prototype, 'freq', {
    get: function() { var p = _aux_pwm_get(this.pad); return p ? p.freq : null; }
});
function pwm(pad) { return new PwmChannel(pad); }

// analog(pad) is a fact like input(): bare, it matches any new reading; narrowed
// with .above(mV) / .below(mV) / .between(lo, hi) it matches a band. A rule
// gated on an analog band fires when the reading ENTERS the band, not on every
// reading inside it -- a reading moves a little all the time, and a level
// trigger would re-run the action on each one. .value / .get() read live (mV).
var _an_seen = {};   // pad -> last reading delivered by _on_analog()
function AnalogCondition(pad) {
    this.pad = _pad(pad, 'analog');
    this._factKey = 'analog:' + this.pad;
    this._lo = undefined;    // strictly above
    this._hi = undefined;    // strictly below
}
AnalogCondition.prototype._narrow = function(lo, hi) {
    var c = new AnalogCondition(this.pad);
    c._lo = (lo === undefined) ? this._lo : Number(lo);
    c._hi = (hi === undefined) ? this._hi : Number(hi);
    return c;
};
AnalogCondition.prototype.above   = function(mv)     { return this._narrow(mv, undefined); };
AnalogCondition.prototype.below   = function(mv)     { return this._narrow(undefined, mv); };
AnalogCondition.prototype.between = function(lo, hi) { return this._narrow(lo, hi); };
AnalogCondition.prototype._check = function() {
    var v = (this.pad in _an_seen) ? _an_seen[this.pad] : _aux_analog_get(this.pad);
    if (v === null || v === undefined) return false;
    if (this._lo !== undefined && !(v > this._lo)) return false;
    if (this._hi !== undefined && !(v < this._hi)) return false;
    return true;
};
Object.defineProperty(AnalogCondition.prototype, 'value', {
    get: function() { return _aux_analog_get(this.pad); }
});
AnalogCondition.prototype.get = function() { return _aux_analog_get(this.pad); };
function analog(pad) { return new AnalogCondition(pad); }

// ── OutputCondition ──────────────────────────────────────────────────────────
// output(ch) is dual-purpose: an actuator (set/on/off/toggle/get/value, used in
// then()) AND a fact (is/isOn/isOff/_check, used in when()). Writing it through
// _drive_output emits an 'output:ch' event so rules gated on the output re-evaluate.

function OutputCondition(ch) {
    this.channel = _channel(ch, 'output');
    this._expected = undefined;
}
// actuator side — mutators return the resulting level, e.g. print(output(0).toggle())
OutputCondition.prototype.set    = function(v) { return _drive_output(this.channel, !!v); };
OutputCondition.prototype.on     = function()  { return _drive_output(this.channel, true); };
OutputCondition.prototype.off    = function()  { return _drive_output(this.channel, false); };
OutputCondition.prototype.toggle = function()  { return _drive_output(this.channel, null); };
OutputCondition.prototype.get    = function()  { return _dout_get(this.channel); };
Object.defineProperty(OutputCondition.prototype, 'value', {
    get: function() { return _dout_get(this.channel); }
});
// condition side — copy-on-write, like InputCondition
OutputCondition.prototype.is    = function(expected) {
    var c = new OutputCondition(this.channel);
    c._expected = expected;
    return c;
};
OutputCondition.prototype.isOn  = function() { return this.is(true);  };
OutputCondition.prototype.isOff = function() { return this.is(false); };
OutputCondition.prototype._check = function() {
    var cur = _dout_get(this.channel);
    if (this._expected === undefined) return true;
    return cur === this._expected;
};

// ── Actuators: led() and buzzer() ─────────────────────────────────────────────
// Singletons (one RGB LED, one buzzer), used in then() bodies — like output(), they
// drive hardware via host bindings. Deliberately simpler than the MQTT led/buzzer paths:
// no per-call duration or sequences — the rule engine's own timing (after/heldFor/every/
// cron) covers that. Note: led().set() only takes effect in LED "IO" mode; in status mode
// the firmware owns the LED and the write is ignored.

// "#rrggbb" (leading '#' optional) → [r,g,b]; null if unparseable.
function _parseHexColor(s) {
    s = String(s).replace(/^#/, '');
    if (!/^[0-9a-fA-F]{6}$/.test(s)) return null;
    return [parseInt(s.slice(0, 2), 16), parseInt(s.slice(2, 4), 16), parseInt(s.slice(4, 6), 16)];
}
function Led() {}
// led().set(r, g, b)  or  led().set("#rrggbb")
Led.prototype.set = function(r, g, b) {
    if (typeof r === 'string') {
        var c = _parseHexColor(r);
        if (!c) { print('[rules] led().set: bad colour "' + r + '"'); return this; }
        _led_set(c[0], c[1], c[2]);
    } else {
        _led_set(r, g, b);
    }
    return this;
};
Led.prototype.off = function() { _led_set(0, 0, 0); return this; };

function Buzzer() {}
// buzzer().set(freqHz) → continuous tone; buzzer().off() → silence.
Buzzer.prototype.set = function(freq) { _buzzer_set(freq); return this; };
Buzzer.prototype.off = function()     { _buzzer_set(0);    return this; };

// ── Time triggers (every / cron) ──────────────────────────────────────────────
// Unlike .after()/.heldFor() (which time a rule's *action*), these are trigger
// *sources*: the engine fires its own event on a schedule. Each is an edge — armed
// only during its own tick — so other when()-conditions act as gates:
//   .when(every(5000), input(0).isOn())  → every 5 s, fire if DI0 is on.
// Each instance gets a unique fact key (time:N) so dispatch only re-evaluates the
// rules referencing it; the timer is armed when the rule registers and cancelled by
// _reset_rules (it runs through _schedule, so _cancel_all_timers covers it).
var _timeSeq = 0;

// Wall-clock epoch ms (UTC). Prefers a host _now() binding (gettimeofday on-device,
// mockable in tests); falls back to Date.now(). On-device real time comes from the RTC at
// boot and SNTP once the network is up; until the clock is valid, cron stays suppressed
// (see _clockValid / _on_time_sync). See RULES.md.
function _now_ms() {
    if (typeof _now === 'function') return _now();
    if (typeof Date !== 'undefined' && Date.now) return Date.now();
    return 0;
}

// ── IntervalCondition: every(ms) ──
// Intervals are floored at MIN_INTERVAL_MS: they re-arm every tick (a fresh esp_timer
// per period on-device), so a very short period would churn the timer subsystem and the
// scripting task. 100 ms is plenty for I/O automation; one-shot delays (.after/.heldFor)
// and cron are not floored.
var MIN_INTERVAL_MS = 100;
function IntervalCondition(ms) {
    if (!(ms >= MIN_INTERVAL_MS)) {           // also catches undefined/NaN/<=0
        print('[rules] every(' + ms + ') raised to the ' + MIN_INTERVAL_MS + 'ms minimum');
        ms = MIN_INTERVAL_MS;
    }
    this.ms       = ms;
    this._factKey = 'time:' + (++_timeSeq);
    this._armed   = false;
    this._timer   = null;
}
IntervalCondition.prototype._check = function() { return this._armed; };
IntervalCondition.prototype._arm = function() {
    if (this._timer != null) return;          // already armed (shared across rules)
    var self = this;
    self._timer = _schedule(self.ms, function() {
        self._timer = null;
        self._armed = true;
        _fire_matching(self._factKey);
        self._armed = false;
        self._arm();                          // re-arm for the next period
    });
    if (self._timer == null) _rearm_when_free(self, self._arm);   // table full: when a slot frees
};

// ── Cron: cron("min hour dom month dow") — standard 5-field, local time ──
// Evaluated in the device's local timezone (the configured POSIX TZ; UTC when none is
// set). The wall-clock instant comes from _now_ms(); local field extraction uses the
// JS Date local getters, which on-device resolve through QuickJS → newlib localtime, so
// DST is handled by the TZ rules. Supports *, lists (a,b), ranges (a-b), and steps
// (*/n, a-b/n). Day-of-month and day-of-week follow Vixie semantics: if both are
// restricted the match is OR.
// DST edge cases (unavoidable with local-time cron): on spring-forward a wall-clock time
// in the skipped hour never occurs, so a job scheduled then is skipped that day; on
// fall-back a repeated wall-clock time fires once (we advance strictly past each match).
function _cronField(spec, min, max) {
    var set = {};
    var parts = String(spec).split(',');
    for (var i = 0; i < parts.length; i++) {
        var p = parts[i], step = 1, hasStep = false, slash = p.indexOf('/');
        if (slash >= 0) {
            step = parseInt(p.slice(slash + 1), 10);
            p = p.slice(0, slash);
            hasStep = true;
            if (!(step >= 1)) throw new Error('cron: bad step in "' + spec + '"');
        }
        var lo, hi;
        if (p === '*') { lo = min; hi = max; }
        else if (p.indexOf('-') >= 0) { var r = p.split('-'); lo = parseInt(r[0], 10); hi = parseInt(r[1], 10); }
        else { lo = parseInt(p, 10); hi = hasStep ? max : lo; }   // "a/n" → a..max step n (Vixie)
        if (!(lo >= min && hi <= max && lo <= hi)) throw new Error('cron: out of range in "' + spec + '"');
        for (var v = lo; v <= hi; v += step) set[v] = true;
    }
    return set;
}
function _parseCron(expr) {
    var f = String(expr).trim().split(/\s+/);
    if (f.length !== 5) throw new Error('cron: expected 5 fields in "' + expr + '"');
    var dow = _cronField(f[4], 0, 7);
    if (dow[7]) { dow[0] = true; delete dow[7]; }   // 7 and 0 both mean Sunday
    return {
        minute: _cronField(f[0], 0, 59),
        hour:   _cronField(f[1], 0, 23),
        dom:    _cronField(f[2], 1, 31),
        month:  _cronField(f[3], 1, 12),
        dow:    dow,
        domR:   f[2] !== '*',
        dowR:   f[4] !== '*'
    };
}
function _cronDayOk(f, d) {
    var domOk = f.dom[d.getDate()] === true;
    var dowOk = f.dow[d.getDay()] === true;
    if (f.domR && f.dowR) return domOk || dowOk;   // Vixie OR
    if (f.domR) return domOk;
    if (f.dowR) return dowOk;
    return true;
}
// Next epoch ms strictly after fromMs matching the cron fields in LOCAL time; -1 if none
// in ~5y. Field tests/steps use local Date getters/setters (so the JS engine applies the
// TZ + DST when mapping local fields ↔ epoch); the returned d.getTime() is still UTC ms.
// Minutes are stepped in absolute time, the coarser fields in local time. Stepping the
// minute through the local fields jumped from 02:59 summer time to 03:00 winter time on
// the night clocks go back, and the repeated hour never came: a "* * * * *" rule went
// silent for an hour. Absolute steps walk through the repeated hour like any other, so a
// rule with a wildcard hour runs in both of them (as Vixie cron does); one with a fixed
// hour runs once -- the second pass through the same local time is skipped below.
function _cronNext(f, fromMs) {
    var d = new Date(Math.floor(fromMs / 60000) * 60000 + 60000);   // the next whole minute
    var capYear = new Date(fromMs).getFullYear() + 5;
    var anyHour = true;
    for (var h = 0; h < 24; h++) if (f.hour[h] !== true) { anyHour = false; break; }
    while (d.getFullYear() <= capYear) {
        if (f.month[d.getMonth() + 1] !== true) { d.setMonth(d.getMonth() + 1, 1); d.setHours(0, 0, 0, 0); continue; }
        if (!_cronDayOk(f, d))                   { d.setDate(d.getDate() + 1);     d.setHours(0, 0, 0, 0); continue; }
        if (f.hour[d.getHours()] !== true)       { d.setHours(d.getHours() + 1, 0, 0, 0); continue; }
        if (f.minute[d.getMinutes()] !== true)   { d = new Date(d.getTime() + 60000); continue; }
        // Never a time at or before fromMs. Local-time arithmetic around a
        // daylight saving switch is where engines go wrong (QuickJS put "the
        // next minute" an hour in the past on the spring-forward night); a
        // match in the past would fire at once and re-arm into the past again,
        // running the action over and over. Step on to the next minute instead.
        if (d.getTime() <= fromMs) { d = new Date(Math.floor(fromMs / 60000) * 60000 + 60000); continue; }
        // The hour repeated when clocks go back: the same local time an hour earlier.
        // A job at a fixed hour has run then already.
        if (!anyHour) {
            var e = new Date(d.getTime() - 3600000);
            if (e.getHours() === d.getHours() && e.getMinutes() === d.getMinutes()) { d = new Date(d.getTime() + 60000); continue; }
        }
        return d.getTime();
    }
    return -1;
}
// Clock validity gates cron. Until the wall-clock is real (host _time_valid() — set by an
// RTC seed at boot or the first SNTP sync) cron stays unarmed, so "0 7 * * *" can't fire
// ~7 h after boot off a 1970 clock. Without the host binding (simulator, tests) the clock
// is assumed real, so cron behaves as before. Cron triggers of registered rules are
// tracked so the host can re-arm them when the clock jumps from boot-relative to real
// (see _on_time_sync); a cron() no rule uses is never armed and holds no timer slot.
var _clockValid   = (typeof _time_valid === 'function') ? !!_time_valid() : true;
var _cronTriggers = [];

function CronCondition(expr) {
    this.expr      = expr;
    this._fields   = _parseCron(expr);    // throws on a malformed expression
    this._factKey = 'time:' + (++_timeSeq);
    this._armed    = false;
    this._timer    = null;
}
CronCondition.prototype._check = function() { return this._armed; };
CronCondition.prototype._arm = function() {
    if (_cronTriggers.indexOf(this) < 0) _cronTriggers.push(this);   // first rule using it
    if (this._timer != null) return;       // already armed (shared across rules)
    if (!_clockValid) return;              // suppressed until time is valid; _on_time_sync arms
    var self = this;
    var now  = _now_ms();
    var next = _cronNext(self._fields, now);
    if (next < 0) { print('[rules] cron "' + self.expr + '" has no upcoming match'); return; }
    self._timer = _schedule(next - now, function() {
        self._timer = null;
        self._armed = true;
        _fire_matching(self._factKey);
        self._armed = false;
        self._arm();                       // schedule the following occurrence
    });
    if (self._timer == null) _rearm_when_free(self, self._arm);   // table full: when a slot frees
};

// Called by the host (EVT_TIME_SYNC) when SNTP (re)syncs the clock. esp_timer delays are
// monotonic, so a cron timer armed against a boot-relative (or stale) clock would fire at
// the wrong real moment after a step. Mark the clock valid, then cancel and re-arm every
// cron trigger from the corrected wall-clock. every()/.after()/.heldFor() are relative and
// need no re-arming. Exposed as a global so the C task can call it.
function _on_time_sync() {
    _clockValid = true;
    for (var i = 0; i < _cronTriggers.length; i++) {
        var c = _cronTriggers[i];
        if (c._timer != null) { _unschedule(c._timer); c._timer = null; }
        c._arm();
    }
}

// ── Fact: synthetic working-memory value ──────────────────────────────────────
// fact(initial) holds an arbitrary value that rules compute and gate on — the
// Drools "calculated fact" pattern. It is a value holder (.set/.get/.value, used in
// then()) AND a fact source: .set() emits a change event (only when the value
// actually changes) so rules gating on .is(...) re-evaluate. Each fact is its own
// dispatch key, and the change event runs through _emit, so the cascade cap and
// .noLoop() apply just as they do for outputs.
var _factSeq = 0;
function Fact(initial) {
    this._value   = (arguments.length ? initial : null);
    this._factKey = 'fact:' + (++_factSeq);
}
Fact.prototype.get = function() { return this._value; };
Object.defineProperty(Fact.prototype, 'value', { get: function() { return this._value; } });
Fact.prototype.set = function(v) {
    if (this._value !== v) { this._value = v; _emit(this._factKey); }   // change → event
    return this._value;
};
Fact.prototype._check = function() { return true; };                    // bare fact always matches
// .is(matcher): matcher is a predicate (fn of the value) or a constant to equal.
Fact.prototype.is = function(matcher) { return new FactCondition(this, matcher); };
// Truthiness shortcuts for boolean-ish facts (any truthy/falsy value, not just true/false).
Fact.prototype.isTrue  = function() { return this.is(function(v) { return !!v; }); };
Fact.prototype.isFalse = function() { return this.is(function(v) { return !v;  }); };

function FactCondition(src, matcher) {
    this._src     = src;             // the underlying Fact (live value lives here)
    this._factKey = src._factKey;    // same dispatch key → .set() re-evaluates this rule
    this._matcher = matcher;
}
FactCondition.prototype._check = function() {
    var v = this._src._value;
    return (typeof this._matcher === 'function') ? !!this._matcher(v) : (v === this._matcher);
};

// ── Watchdog: liveness / dead-man's-switch ────────────────────────────────────
// watchdog(ms) is "alive" while fed within `ms`, else "expired" — the basis for local
// fallback when a governing system or its link goes away. It starts alive with a grace
// period (the timeout is armed when a rule using it registers), so a healthy system has
// `ms` after boot to check in before fallback kicks in. .feed() re-arms; transitions
// (alive→expired on timeout, expired→alive on a feed) emit so gating rules re-evaluate.
// It is its own dispatch fact. Used via .isAlive()/.isExpired() in when() and .feed() in
// then(); cancelled by _reset_rules (its timer runs through _schedule).
function Watchdog(ms) {
    this.ms       = ms;
    this._alive   = true;
    this._factKey = 'fact:' + (++_factSeq);
    this._timer   = null;
    this._started = false;
}
Watchdog.prototype._rearm = function() {
    if (this._timer != null) _unschedule(this._timer);
    var self = this;
    this._timer = _schedule(this.ms, function() {
        self._timer = null;
        if (self._alive) { self._alive = false; _emit(self._factKey); }   // timed out → expired
    });
    if (this._timer == null) _rearm_when_free(this, this._rearm);   // table full: when a slot frees
};
Watchdog.prototype._arm = function() {           // at rule registration; idempotent
    if (this._started) return;
    this._started = true;
    this._rearm();                                // start the grace period
};
Watchdog.prototype.feed = function() {
    var was = this._alive;
    this._alive = true; this._started = true;
    this._rearm();
    if (!was) _emit(this._factKey);               // recovered → notify gating rules
    return true;
};
Watchdog.prototype._cond = function(test, label) {   // a condition bound to this watchdog
    var self = this;
    return { _factKey: self._factKey, _check: test, _arm: function() { self._arm(); }, _desc: label };
};
Watchdog.prototype.isAlive   = function() { var s = this; return this._cond(function() { return  s._alive; }, 'watchdog.isAlive()'); };
Watchdog.prototype.isExpired = function() { var s = this; return this._cond(function() { return !s._alive; }, 'watchdog.isExpired()'); };

// mqtt(topic).stale(ms): a watchdog auto-fed by messages on `topic` — matches (stale)
// when no message arrived for `ms`. _on_mqtt feeds it; otherwise it is a Watchdog.
function StaleCondition(topic, ms) {
    this.topic    = topic;
    this._wd      = new Watchdog(ms);
    this._factKey = this._wd._factKey;
    this._desc    = "mqtt('" + topic + "').stale(" + ms + ")";
}
StaleCondition.prototype._check = function() { return !this._wd._alive; };   // stale = expired
StaleCondition.prototype._arm   = function() { this._wd._arm(); };
StaleCondition.prototype._feed  = function() { this._wd.feed(); };
MqttCondition.prototype.stale = function(ms) { return new StaleCondition(this.topic, ms); };

// ── Fieldbus command health: modbus(ms) / can(ms) ─────────────────────────────
// Upstream-control liveness for the MODBUS and CAN/NMEA2000 command paths — the fieldbus
// analogue of mqtt(topic).stale(ms). Each is a Watchdog the host auto-feeds whenever a
// command arrives (the firmware calls _on_activity('modbus'|'can') on every inbound
// write/control command), so .isAlive() means "a command came within ms" and .isExpired()
// means the upstream link has gone quiet — the basis for local fallback. Like any
// watchdog, each starts with an `ms` grace period from when its rule registers.
var _activityWatchdogs = {};   // source key → [Watchdog, ...]
function _activitySource(key, ms) {
    var wd = new Watchdog(ms);
    (_activityWatchdogs[key] || (_activityWatchdogs[key] = [])).push(wd);
    return wd;
}
// Host-called when an upstream command arrives on `key` — keep its watchdogs alive
// (feed() emits only on a quiet→active recovery, so gating rules re-evaluate on edges).
function _on_activity(key) {
    _retry_arms();
    var list = _activityWatchdogs[key];
    if (!list) return;
    _resetCascade();                       // root event → fresh cascade budget
    for (var i = 0; i < list.length; i++) list[i].feed();
}

// ── Public factory functions ─────────────────────────────────────────────────

function mqtt(topic)  { return new MqttCondition(topic); }
function input(ch)    { return new InputCondition(ch); }
function output(ch)   { return new OutputCondition(ch); }
function led()        { return new Led(); }
function buzzer()     { return new Buzzer(); }
function every(ms)    { return new IntervalCondition(ms); }
function cron(expr)   { return new CronCondition(expr); }
function fact(initial){ return new Fact(initial); }
function watchdog(ms) { return new Watchdog(ms); }
// Upstream command-health watchdogs, auto-fed by the host (see _on_activity).
function modbus(ms)   { return _activitySource('modbus', ms); }
function can(ms)      { return _activitySource('can', ms); }
// MQTT connection state, fed by the host as internal '$sys/mqtt' events (up/down).
function mqttConnected() { return mqtt('$sys/mqtt').is(function(s){ return s === 'up';   }); }
function mqttDown()      { return mqtt('$sys/mqtt').is(function(s){ return s === 'down'; }); }

// ── Rule builder (fluent API) ────────────────────────────────────────────────

// Normalize a when() argument into an object exposing _check().
// Mirrors Drools-style pattern semantics: a bare pattern matches by existence,
// constraints only narrow it.
//   • input()/mqtt() (anything with _check) → passed through unchanged, so MQTT
//     routing in _on_mqtt and the simulator's introspection keep working.
//   • a plain function → used as a predicate, e.g. .when(function(){ return ... })
//   • an explicit boolean / value → constant gate (!!value)
//   • null/undefined → always-matching gate, so a stray/empty argument never
//     silently blocks the whole rule.
function _asCondition(c) {
    if (c && typeof c._check === 'function') return c;
    if (typeof c === 'function')             return { _check: function() { return !!c(); } };
    if (c === undefined || c === null)       return { _check: function() { return true;  } };
    return { _check: function() { return !!c; } };
}

// Which facts a rule's conditions reference. An event on one fact only re-evaluates
// rules that depend on it — input(4) and input(7) are distinct facts, so changing
// one never re-fires a rule gated solely on the other. Opaque conditions (predicate
// functions, constants) can't be analysed, so such a rule is marked `wildcard` and
// re-evaluated on every event.
function _ruleFacts(conditions) {
    var facts = {}, wildcard = false;
    for (var i = 0; i < conditions.length; i++) {
        var c = conditions[i];
        if      (c instanceof InputCondition)  facts['input:'  + c.channel] = true;
        else if (c instanceof OutputCondition) facts['output:' + c.channel] = true;
        else if (c instanceof MqttCondition)   facts['mqtt:'   + c.topic]   = true;
        else if (c && c._factKey)             facts[c._factKey]           = true;  // every()/cron()
        else                                   wildcard = true;
    }
    return { facts: facts, wildcard: wildcard };
}

function RuleBuilder(name) {
    this.name = name;
    this._conditions = [];
    this._heldFor   = 0;     // .heldFor(ms): conditions must hold this long before firing
    this._nextDelay = 0;     // .after(ms):   delay applied to the next .then() step
    this._steps     = [];    // action timeline: [{ delay, fn }, ...]
    this._noLoop    = false; // .noLoop(): don't re-fire from this rule's own consequence
    this._rule      = null;  // the registered rule object (created on first .then())
}
RuleBuilder.prototype.when = function() {
    for (var i = 0; i < arguments.length; i++)
        this._conditions.push(_asCondition(arguments[i]));
    return this;
};
// Sustained gate: fire only once the when()-conditions have held continuously for
// `ms`; the pending fire is cancelled if they break before then. Fires once per
// sustained period (re-arms after the conditions drop and rise again).
RuleBuilder.prototype.heldFor = function(ms) {
    this._heldFor = ms;
    if (this._rule) this._rule.heldFor = ms;
    _check_held_edges(this);
    return this;
};

// An edge -- mqtt().once(), every(), cron() -- is true only during the pass
// it triggers, never when the held period runs out: such a rule never fired,
// and nothing said why. Refused when the rule is built, in either order.
function _check_held_edges(b) {
    if (!(b._heldFor > 0)) return;
    for (var i = 0; i < b._conditions.length; i++) {
        var c = b._conditions[i];
        if ((c instanceof MqttCondition && c._once) || c instanceof IntervalCondition || c instanceof CronCondition)
            throw new Error('rule "' + b.name + '": heldFor() cannot be combined with once(), every() or cron() -- ' +
                            'an edge is never still true when the held period ends');
    }
}
// No-loop (Drools-style): the rule will not be re-fired by changes its OWN action
// causes — directly or through the cascade it triggers. Other rules still react to
// its writes, and it still fires on later, independent events. Order-independent.
RuleBuilder.prototype.noLoop = function() {
    this._noLoop = true;
    if (this._rule) this._rule.noLoop = true;
    return this;
};
// Schedule the next .then() step `ms` after the previous one — fire-and-forget,
// independent of whether the conditions still hold (e.g. on now, off 5 s later).
RuleBuilder.prototype.after = function(ms) {
    this._nextDelay = ms;
    return this;
};
RuleBuilder.prototype.then = function(fn) {
    _check_held_edges(this);
    this._steps.push({ delay: this._nextDelay, fn: fn });
    this._nextDelay = 0;
    if (!this._rule) {
        var f = _ruleFacts(this._conditions);
        // A rule "watches" outputs if it gates on one, or is wildcard (may read one).
        if (f.wildcard) _watchOutputs = true;
        else for (var fk in f.facts) { if (fk.indexOf('output:') === 0) { _watchOutputs = true; break; } }
        var ref = this._rule = {
            name:       this.name,
            conditions: this._conditions,
            heldFor:    this._heldFor,
            steps:      this._steps,   // same array — later .then() calls extend it
            action:     null,
            _facts:     f.facts,       // fact keys this rule listens to (e.g. 'input:4')
            _wildcard:  f.wildcard,    // true → re-evaluate on every event
            noLoop:     this._noLoop,  // .noLoop(): suppress self-re-firing
            _firing:    false,         // re-entrancy guard for noLoop
            _timer:     null,          // pending heldFor timer
            _fired:     false,         // heldFor latch (one fire per sustained period)
            _analogIn:  false          // analog edge latch: conditions held at the last evaluation
        };
        this._rule.action = function() { _run_steps(ref, 0); };
        _rules.push(this._rule);
        _indexMqtt(this._conditions);
        // Arm any time triggers (every/cron) now that the rule is live. Idempotent,
        // so a trigger shared across rules is armed once; _reset_rules cancels them.
        for (var ci = 0; ci < this._conditions.length; ci++) {
            var tc = this._conditions[ci];
            if (tc && typeof tc._arm === 'function') tc._arm();
        }
    }
    return this;   // allow .after().then() chaining
};

function rule(name) { return new RuleBuilder(name); }

// Run a rule's action timeline: step 0 immediately, each later step `delay` ms
// after the previous one (fire-and-forget — see .after()).
function _run_steps(r, idx) {
    if (idx >= r.steps.length) return;
    var step = r.steps[idx];
    var go = function() {
        // A delayed next step is armed before this one runs. The budget's
        // interrupt cannot be caught here: when it landed in step.fn the rest
        // of the timeline went with it -- an output switched on by step 0
        // stayed on, its "off" never armed.
        var next = r.steps[idx + 1];
        var armedNext = !!next && next.delay > 0;
        if (armedNext) _run_steps(r, idx + 1);
        // A delayed step runs from its timer, outside _run_action and its
        // no-loop guard: a .noLoop() rule's own .after() step re-fired it
        // (on, off, on, ... for good). The guard is taken here for it.
        var guard = r.noLoop && !r._firing;
        if (guard) r._firing = true;
        try { step.fn(); } catch (e) { _log_rule_error(r, 'error', e); }
        finally { if (guard) r._firing = false; }
        if (!armedNext) _run_steps(r, idx + 1);
    };
    if (step.delay > 0) _schedule_retrying(step.delay, go);
    else go();
}

// ── Internal event handlers — called from C ──────────────────────────────────

// MQTT conditions of the registered rules by topic, each once even when rules
// share it -- a shared stale() was fed (its timer re-created) once per rule.
// A null-prototype map: a topic like "constructor" is no inherited key.
var _mqttIndex = Object.create(null);   // topic → { conds: [MqttCondition], stale: [StaleCondition] }
function _indexMqtt(conds) {
    for (var j = 0; j < conds.length; j++) {
        var c = conds[j], list;
        if (c instanceof MqttCondition) list = 'conds';
        else if (c instanceof StaleCondition) list = 'stale';
        else continue;
        var e = _mqttIndex[c.topic] || (_mqttIndex[c.topic] = { conds: [], stale: [] });
        if (e[list].indexOf(c) < 0) e[list].push(c);
    }
}

// The topics the loaded rules read -- what the host subscribes at the broker
// (besides "rules/#"). Called by the host after every load.
function _mqtt_topics() { return Object.keys(_mqttIndex); }

function _on_mqtt(topic, payload) {
    _retry_arms();                   // slots may have freed since (a reload, a clear)
    _resetCascade();                 // root event — fresh cascade budget (stale feeds may emit)
    var e = _mqttIndex[topic];
    var armed = [];
    if (e) {
        for (var i = 0; i < e.conds.length; i++) {
            var c = e.conds[i];
            c.value = payload;
            c._seen = true;
            if (c._once) { c._armed = true; armed.push(c); }
        }
        for (var fi = 0; fi < e.stale.length; fi++) e.stale[fi]._feed();   // re-arm stale watchdogs
    }
    _fire_matching('mqtt:' + topic);
    // Disarm edge conditions: they match only during this message's pass, so a
    // later _on_input → _fire_matching won't re-fire the rule.
    for (var k = 0; k < armed.length; k++) armed[k]._armed = false;
}

function _on_input(channel, state) {
    _retry_arms();
    _di_seen[channel] = !!state;
    _fire_matching('input:' + channel);
}

// An input change the engine's queue dropped: the live level is delivered
// as the event it would have been, unless it is what was last seen anyway.
function _resync_input(channel, live) {
    live = !!live;
    if ((channel in _di_seen) && _di_seen[channel] === live) return;
    _on_input(channel, live);
}

function _rule_matches(r) {
    for (var j = 0; j < r.conditions.length; j++) {
        var cond = r.conditions[j];
        var ok;
        try {
            ok = (cond && typeof cond._check === 'function') ? cond._check() : !!cond;
        } catch (e) {
            _log_rule_error(r, 'condition error', e);
            ok = false;   // a throwing condition blocks rather than crashing the engine
        }
        if (!ok) return false;
    }
    return true;
}

// Run a rule's action with the no-loop guard. A noLoop rule sets _firing for the
// duration of its action (and the synchronous cascade it spawns), so any re-evaluation
// reaching it during that window — i.e. caused by its own consequence — is skipped.
function _run_action(r) {
    if (r.noLoop && r._firing) return;   // its own consequence reached it again — skip
    r._firing = true;
    try { r.action(); } catch (e) { _log_rule_error(r, 'error', e); }
    finally { r._firing = false; }     // the budget's interrupt skips this: _after_interrupt
}

// Called by the host after the time budget interrupted a call. QuickJS's
// interrupt is uncatchable: it unwinds past every catch and finally, so the
// flags a pass sets before its work and clears after it stay set -- a noLoop
// rule's _firing (the rule would never run again), an edge condition's _armed
// (an mqtt().once() or a time trigger would keep matching on later, unrelated
// events) -- and a time trigger whose callback was cut short never re-armed
// itself. All of these are only true during a pass, and no pass is running now.
//
// Before the flags are cleared, the passes the interrupt cut short are taken
// up again past the rule that ran over (see _pass), so the later rules still
// see the event -- with the edge flags it armed. Cut short again, the next
// call goes on past the next such rule. resume === false (the host's last
// try) drops what is left of them instead: clearing flags evaluates no rule.
// A heldFor check cut short is set again for another held period.
function _after_interrupt(resume) {
    _resetCascade();
    _retrying = false;          // a retry pass the interrupt cut short (its finally skipped)
    for (var f = 0; f < _rules.length; f++) _rules[f]._firing = false;
    var h = _heldCheck;
    _heldCheck = null;
    if (h && h._timer == null && !h._fired) {
        print('[rule:' + h.name + '] time budget exceeded in the heldFor() check -- checked again after another ' + h.heldFor + ' ms');
        h._timer = _schedule(h.heldFor, _held_elapsed(h));
        if (h._timer == null) _held_later(h);
    }
    if (resume === false) {
        if (_passes.length) print('[rules] time budget exceeded again -- the event is not passed on to the remaining rules');
        _passes = [];
    }
    while (_passes.length) {
        var p = _passes.pop();
        print('[rule:' + (p.rule ? p.rule.name : '?') + '] time budget exceeded -- the event goes on to the rules after it');
        _pass(p);
    }
    _resetCascade();
    var rearm = [];
    for (var i = 0; i < _rules.length; i++) {
        var r = _rules[i];
        r._firing = false;
        var conds = r.conditions;
        for (var j = 0; j < conds.length; j++) {
            var c = conds[j];
            if (c instanceof MqttCondition) c._armed = false;
            else if (c instanceof IntervalCondition || c instanceof CronCondition) {
                c._armed = false;
                if (c._timer == null) rearm.push(c);
            }
        }
    }
    for (var k = 0; k < rearm.length; k++) rearm[k]._arm();   // idempotent per trigger
}

// Whether a rule is edge-triggered on analog readings: it has a band on some
// pad, or it is a predicate rule (wildcard), which would otherwise re-run its
// action on every reading -- several a second -- while its predicate holds.
// A bare analog(pad) is not: it is meant to see every reading.
function _analogEdge(r) {
    if (r._wildcard) return true;
    for (var j = 0; j < r.conditions.length; j++) {
        var c = r.conditions[j];
        if (c instanceof AnalogCondition && (c._lo !== undefined || c._hi !== undefined)) return true;
    }
    return false;
}

// The heldFor timer's callback. The check runs with _heldCheck naming the
// rule: cut short by the time budget there, the timer is gone and nothing
// has fired, and with no further event the rule would never fire although
// its conditions keep holding -- _after_interrupt() sets it again.
var _heldCheck = null;
function _held_elapsed(rule) {
    return function() {
        rule._timer = null;
        _heldCheck = rule;
        var m = _rule_matches(rule);
        _heldCheck = null;
        if (m) { rule._fired = true; _run_action(rule); }
    };
}

// Fire, arm or disarm one rule for a match result -- shared by _fire_matching()
// and _on_analog(). The analog latch follows every evaluation, not only those
// from analog readings: a band rule fired by another fact while the reading is
// inside its band has taken that band's edge.
function _apply_match(r, matched) {
    r._analogIn = matched;
    if (matched) {
        if (r.heldFor > 0) {
            // sustained gate: arm a timer once; fire only if still matching at
            // elapse; latch so it fires once per held period, not every event
            if (r._timer == null && !r._fired) {
                r._timer = _schedule(r.heldFor, _held_elapsed(r));
                if (r._timer == null) _held_later(r);
            }
        } else {
            _run_action(r);
        }
    } else {
        // conditions no longer hold — cancel a pending sustain timer and re-arm
        if (r._timer != null) { _unschedule(r._timer); r._timer = null; }
        r._fired = false;
        r._heldWaiting = false;   // a queued arm (_held_later) is dropped when it comes up
    }
}

// A heldFor() the full table refused. Waiting for its next matching event was
// not enough: an input that went HIGH and stays HIGH sends none, and the rule
// never fired. Queued once, armed as soon as a slot frees -- the hold counts
// from then, late rather than never -- and dropped if the condition has gone
// in the meantime.
function _held_later(r) {
    if (r._heldWaiting) return;
    r._heldWaiting = true;
    _arm_later(function() {
        if (!r._heldWaiting || r._timer != null || r._fired) { r._heldWaiting = false; return true; }
        _heldCheck = r;
        var m = _rule_matches(r);
        _heldCheck = null;
        if (!m) { r._heldWaiting = false; return true; }
        r._timer = _schedule(r.heldFor, _held_elapsed(r));
        if (r._timer == null) return false;          // still full: stays first in line
        r._heldWaiting = false;
        return true;
    });
}

// factKey identifies the fact that changed (e.g. 'input:4', 'mqtt:rules/trigger').
// Only rules that reference that fact — or wildcard rules — are evaluated; a rule
// gated solely on a different fact keeps its state untouched. factKey null/omitted
// evaluates every rule.
function _fire_matching(factKey, _isCascade) {
    if (!_isCascade) _resetCascade();   // a root event/timer starts a fresh cascade budget
    _pass({ key: factKey, analog: false, next: 0, rule: null });
}

// One event's walk over the rules. The passes in progress are kept on
// _passes, innermost last, each with the index of the next rule to take:
// when the time budget cuts a rule short, the interrupt unwinds the whole
// pass -- and every pass around it -- and the rules after that one never
// saw the event. _after_interrupt() goes on from `next`, past the rule that
// ran over. `next` moves before the rule runs, in the one expression that
// reads it, so wherever the interrupt lands no rule runs twice.
var _passes = [];
function _pass(p) {
    _passes.push(p);
    try {
        while (p.next < _rules.length) {
            var r = _rules[p.next++];
            if (p.key != null && !r._wildcard && !r._facts[p.key]) continue;
            p.rule = r;
            var m = _rule_matches(r);
            if (p.analog && m && r._analogIn && !(r.heldFor > 0) && _analogEdge(r)) continue;   // edge taken
            _apply_match(r, m);
        }
    } finally {
        _passes.pop();                 // the budget's interrupt skips this: _after_interrupt
    }
}

// A new analog reading. Edge-triggered per rule: a rule fires when its
// conditions start to hold on an analog reading, and not again until they have
// stopped holding on one -- see AnalogCondition.
// mv null: the pad has stopped being an analog input -- its last reading is
// forgotten, and band conditions on it no longer match.
function _on_analog(pad, mv) {
    _retry_arms();
    if (mv === null || mv === undefined) delete _an_seen[pad];
    else _an_seen[pad] = mv;
    _resetCascade();
    _pass({ key: 'analog:' + pad, analog: true, next: 0, rule: null });
}
