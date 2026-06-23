// Rule Engine DSL — evaluated once at scripting startup.
// C bindings available as globals:
//   _di_get(ch)          → bool    read digital input 0-7
//   _dout_set(ch, bool)  → void    write digital output 0-7
//   _dout_get(ch)        → bool    read current digital output state
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
//   mqtt(topic).is(fn)   → MqttCondition   (condition + value holder)
//   input(ch).is(bool)   → InputCondition  (.value / .get() read the live level)
//   output(ch)           → actuator { set(bool), on(), off(), toggle(), get(), value }
//                          (set/on/off/toggle return the resulting level) AND a fact
//                          { is(bool), isOn(), isOff() } usable in when()
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
        fn();
    };
    if (typeof _set_timer === 'function')      id = _set_timer(ms, wrapped);
    else if (typeof setTimeout === 'function') id = setTimeout(wrapped, ms);
    else { fn(); return null; }   // no scheduler available — degrade to immediate
    _pending.push(id);
    return id;
}
function _unschedule(id) {
    if (id == null) return;
    var k = _pending.indexOf(id); if (k >= 0) _pending.splice(k, 1);
    if (typeof _clear_timer === 'function')      _clear_timer(id);
    else if (typeof clearTimeout === 'function') clearTimeout(id);
}
function _cancel_all_timers() { while (_pending.length) _unschedule(_pending[0]); }

// ── Output as a fact + cascade guard ──────────────────────────────────────────
// Writing an output that actually changes emits an 'output:ch' event, so rules gated
// on the output re-evaluate. Outputs are driven BY rules, so this can feed back on
// itself; a per-trigger cascade budget bounds the chain (each external event or timer
// firing resets it). When the budget is exhausted the cascade stops with a logged
// warning instead of looping forever — important on-device, where an unbounded loop
// would block the scripting task and trip the watchdog.
var CASCADE_CAP = 16;
var _cascade = 0;
var _cascadeWarned = false;
var _watchOutputs = false;   // does any rule reference an output fact / is wildcard?

function _resetCascade() { _cascade = 0; _cascadeWarned = false; }

function _emit(factKey) {
    if (_cascade >= CASCADE_CAP) {
        if (!_cascadeWarned) {
            _cascadeWarned = true;
            print('[rules] cascade limit (' + CASCADE_CAP + ') reached — stopping (output feedback loop?)');
        }
        return;
    }
    _cascade++;
    _fire_matching(factKey, true);   // cascade pass — does NOT reset the budget
}

// Drive an output through the host binding; emit only on an actual change, and only
// when some rule is watching outputs (keeps the common no-watcher case free of cost).
function _drive_output(ch, v) {
    var before = _dout_get(ch);
    _dout_set(ch, v);
    var after = _dout_get(ch);
    if (after !== before && _watchOutputs) _emit('output:' + ch);
    return after;
}

// Called on rule reload (EVT_RELOAD / simulator "Apply") — drop the rule set and
// any in-flight timers so stale callbacks can't fire against the old rules.
function _reset_rules() {
    _cancel_all_timers();
    _rules = [];
    _watchOutputs = false;
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

// ── InputCondition ───────────────────────────────────────────────────────────

function InputCondition(ch) {
    this.channel = ch;
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
InputCondition.prototype._check = function() {
    var cur = _di_get(this.channel);
    if (this._expected === undefined) return true;
    return cur === this._expected;
};
// Live input level — readable in then() bodies as a property or a method:
//   input(0).value   /   input(0).get()
Object.defineProperty(InputCondition.prototype, 'value', {
    get: function() { return _di_get(this.channel); }
});
InputCondition.prototype.get = function() { return _di_get(this.channel); };

// ── OutputCondition ──────────────────────────────────────────────────────────
// output(ch) is dual-purpose: an actuator (set/on/off/toggle/get/value, used in
// then()) AND a fact (is/isOn/isOff/_check, used in when()). Writing it through
// _drive_output emits an 'output:ch' event so rules gated on the output re-evaluate.

function OutputCondition(ch) {
    this.channel = ch;
    this._expected = undefined;
}
// actuator side — mutators return the resulting level, e.g. print(output(0).toggle())
OutputCondition.prototype.set    = function(v) { return _drive_output(this.channel, !!v); };
OutputCondition.prototype.on     = function()  { return _drive_output(this.channel, true); };
OutputCondition.prototype.off    = function()  { return _drive_output(this.channel, false); };
OutputCondition.prototype.toggle = function()  { return _drive_output(this.channel, !_dout_get(this.channel)); };
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

// ── Public factory functions ─────────────────────────────────────────────────

function mqtt(topic)  { return new MqttCondition(topic); }
function input(ch)    { return new InputCondition(ch); }
function output(ch)   { return new OutputCondition(ch); }

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
    return this;
};
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
            _fired:     false          // heldFor latch (one fire per sustained period)
        };
        this._rule.action = function() { _run_steps(ref, 0); };
        _rules.push(this._rule);
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
        try { step.fn(); } catch (e) { print('[rule:' + r.name + '] error: ' + e); }
        _run_steps(r, idx + 1);
    };
    if (step.delay > 0) _schedule(step.delay, go);
    else go();
}

// ── Internal event handlers — called from C ──────────────────────────────────

function _on_mqtt(topic, payload) {
    var armed = [];
    for (var i = 0; i < _rules.length; i++) {
        var conds = _rules[i].conditions;
        for (var j = 0; j < conds.length; j++) {
            var c = conds[j];
            if (c instanceof MqttCondition && c.topic === topic) {
                c.value = payload;
                c._seen = true;
                if (c._once) { c._armed = true; armed.push(c); }
            }
        }
    }
    _fire_matching('mqtt:' + topic);
    // Disarm edge conditions: they match only during this message's pass, so a
    // later _on_input → _fire_matching won't re-fire the rule.
    for (var k = 0; k < armed.length; k++) armed[k]._armed = false;
}

function _on_input(channel, state) {
    _fire_matching('input:' + channel);
}

function _rule_matches(r) {
    for (var j = 0; j < r.conditions.length; j++) {
        var cond = r.conditions[j];
        var ok;
        try {
            ok = (cond && typeof cond._check === 'function') ? cond._check() : !!cond;
        } catch (e) {
            print('[rule:' + r.name + '] condition error: ' + e);
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
    try { r.action(); } catch (e) { print('[rule:' + r.name + '] error: ' + e); }
    r._firing = false;
}

// factKey identifies the fact that changed (e.g. 'input:4', 'mqtt:rules/trigger').
// Only rules that reference that fact — or wildcard rules — are evaluated; a rule
// gated solely on a different fact keeps its state untouched. factKey null/omitted
// evaluates every rule.
function _fire_matching(factKey, _isCascade) {
    if (!_isCascade) _resetCascade();   // a root event/timer starts a fresh cascade budget
    for (var i = 0; i < _rules.length; i++) {
        var r = _rules[i];
        if (factKey != null && !r._wildcard && !r._facts[factKey]) continue;
        if (_rule_matches(r)) {
            if (r.heldFor > 0) {
                // sustained gate: arm a timer once; fire only if still matching at
                // elapse; latch so it fires once per held period, not every event
                if (r._timer == null && !r._fired) {
                    r._timer = _schedule(r.heldFor, (function(rule) {
                        return function() {
                            rule._timer = null;
                            if (_rule_matches(rule)) { rule._fired = true; _run_action(rule); }
                        };
                    })(r));
                }
            } else {
                _run_action(r);
            }
        } else {
            // conditions no longer hold — cancel a pending sustain timer and re-arm
            if (r._timer != null) { _unschedule(r._timer); r._timer = null; }
            r._fired = false;
        }
    }
}
