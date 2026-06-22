// Rule Engine DSL — evaluated once at scripting startup.
// C bindings available as globals:
//   _di_get(ch)          → bool    read digital input 0-7
//   _dout_set(ch, bool)  → void    write digital output 0-7
//   _dout_get(ch)        → bool    read current digital output state
//   print(str)           → void    log to ESP console
//
// Public API exposed to user scripts:
//   rule(name).when(...conditions).then(fn)
//   mqtt(topic).is(fn)   → MqttCondition   (condition + value holder)
//   input(ch).is(bool)   → InputCondition
//   output(ch)           → { set(bool), get() }

'use strict';

var _rules = [];

// ── MqttCondition ────────────────────────────────────────────────────────────

function MqttCondition(topic) {
    this.topic = topic;
    this._predicate = null;
    this.value = null;   // last received payload — readable in then() body
    this._seen = false;
}
MqttCondition.prototype.is = function(predicate) {
    this._predicate = predicate;
    return this;
};
MqttCondition.prototype._check = function() {
    if (!this._seen) return false;
    if (!this._predicate) return true;
    return !!this._predicate(this.value);
};

// ── InputCondition ───────────────────────────────────────────────────────────

function InputCondition(ch) {
    this.channel = ch;
    this._expected = undefined;
}
InputCondition.prototype.is = function(expected) {
    this._expected = expected;
    return this;
};
InputCondition.prototype._check = function() {
    var cur = _di_get(this.channel);
    if (this._expected === undefined) return true;
    return cur === this._expected;
};

// ── Public factory functions ─────────────────────────────────────────────────

function mqtt(topic)  { return new MqttCondition(topic); }
function input(ch)    { return new InputCondition(ch); }
function output(ch) {
    return {
        set: function(v)  { _dout_set(ch, !!v); },
        get: function()   { return _dout_get(ch); }
    };
}

// ── Rule builder (fluent API) ────────────────────────────────────────────────

function RuleBuilder(name) {
    this.name = name;
    this._conditions = [];
}
RuleBuilder.prototype.when = function() {
    for (var i = 0; i < arguments.length; i++)
        this._conditions.push(arguments[i]);
    return this;
};
RuleBuilder.prototype.then = function(action) {
    _rules.push({ name: this.name, conditions: this._conditions, action: action });
};

function rule(name) { return new RuleBuilder(name); }

// ── Internal event handlers — called from C ──────────────────────────────────

function _on_mqtt(topic, payload) {
    for (var i = 0; i < _rules.length; i++) {
        var conds = _rules[i].conditions;
        for (var j = 0; j < conds.length; j++) {
            var c = conds[j];
            if (c instanceof MqttCondition && c.topic === topic) {
                c.value = payload;
                c._seen = true;
            }
        }
    }
    _fire_matching();
}

function _on_input(channel, state) {
    _fire_matching();
}

function _fire_matching() {
    for (var i = 0; i < _rules.length; i++) {
        var r = _rules[i];
        var all_match = true;
        for (var j = 0; j < r.conditions.length; j++) {
            if (!r.conditions[j]._check()) { all_match = false; break; }
        }
        if (all_match) {
            try {
                r.action();
            } catch (e) {
                print('[rule:' + r.name + '] error: ' + e);
            }
        }
    }
}
