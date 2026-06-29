# Rule Engine Language

A small, declarative language for wiring digital inputs, digital outputs, and MQTT
messages together into automation rules. Rules are written in JavaScript and
evaluated once at startup; the engine then reacts to events (input changes, MQTT
messages, timers) and runs the matching rules.

The same language runs in two places:

- **On the device** — inside the firmware's scripting runtime.
- **In the browser simulator** (`simulator/`) — which loads the *live* engine source
  (`components/scripting/dsl.js`) so what you test is what runs.

The model is deliberately **Drools-like**: a rule has a *when* part (facts that must
match) and a *then* part (what to do). A bare fact pattern matches by existence;
adding constraints narrows it.

---

## Table of contents

1. [Rule structure](#1-rule-structure)
2. [Facts and helpers](#2-facts-and-helpers)
   - [`input(ch)`](#inputch--digital-input)
   - [`output(ch)`](#outputch--digital-output)
   - [`led()` and `buzzer()`](#led-and-buzzer--led-and-buzzer-actuators)
   - [`mqtt(topic)`](#mqtttopic--mqtt-message)
   - [How events reach rules (the fact model)](#how-events-reach-rules-the-fact-model)
3. [Time-based rules](#3-time-based-rules)
4. [Rule chaining](#4-rule-chaining)
5. [Quick reference](#5-quick-reference)
6. [Gotchas](#6-gotchas)

---

## 1. Rule structure

Every rule has the same shape:

```js
rule(name)
  .when(/* one or more conditions */)
  .then(function () { /* action */ });
```

- **`rule(name)`** — starts a rule. `name` is a string used in logs and the
  simulator's rule panel.
- **`.when(...)`** — the conditions. **All of them must match** for the rule to fire
  (logical AND). Conditions are facts (`input`, `mqtt`) or plain predicates.
- **`.then(fn)`** — the action to run when the rule fires.

### Minimal example

```js
// Edge action: when DI0 goes high, turn DO0 on (and leave it on).
rule('DO0 on when DI0 goes high')
  .when(input(0).isOn())
  .then(function () {
    output(0).on();
  });
```

The rule above only ever turns DO0 *on* — it fires when DI0 becomes high and does
nothing when DI0 goes low. If instead you want DO0 to **follow** DI0 exactly (a true
mirror), gate on the **bare** input — which fires on every change of that input — and
copy the level into the output:

```js
// Mirror: DO0 tracks DI0 in both directions.
rule('DO0 mirrors DI0')
  .when(input(0))                       // bare pattern → fires on any DI0 change
  .then(function () {
    output(0).set(input(0).value);      // DO0 := DI0
  });
```

### Multiple conditions are AND-ed

```js
// Fire only when a trigger message arrives AND DI0 is off.
var trig = mqtt('rules/trigger').is(function (m) { return m === 'ON'; });

rule('guarded start')
  .when(trig, input(0).isOff())
  .then(function () {
    output(0).on();
  });
```

> **Tip — arrow functions.** The rule language is JavaScript, so predicates and actions
> can use the shorter arrow syntax, which often reads better:
> ```js
> var trig = mqtt('rules/trigger').is(m => m === 'ON');
>
> rule('guarded start')
>   .when(trig, input(0).isOff())
>   .then(() => output(0).on());
> ```
> The examples here use `function () { … }` for clarity, but `=>` works anywhere a
> function is accepted (`.is(...)`, `.when(...)` predicates, `.then(...)`).

### OR — use a predicate, or two rules

There is no `or()` combinator. Express OR either with a predicate function …

```js
rule('any door open')
  .when(function () { return input(0).value || input(1).value; })
  .then(function () { output(7).on(); });
```

A change on **either** `input(0)` or `input(1)` runs this rule — but for a subtle
reason worth understanding: because the condition is a predicate function, the engine
can't see which inputs it reads, so the rule is **wildcard** and re-evaluated on
*every* event. That makes the OR work, at the cost of also running on unrelated events.
For targeted dispatch, the two-rules form is leaner:

```js
rule('door 0 open').when(input(0).isOn()).then(function () { output(7).on(); });
rule('door 1 open').when(input(1).isOn()).then(function () { output(7).on(); });
```

See [the fact model](#how-events-reach-rules-the-fact-model) for the wildcard trade-off.

### Predicates and booleans in `when()`

`when()` also accepts a plain function (used as a predicate) or a boolean constant:

```js
rule('complex gate')
  .when(function () { return input(0).value && !output(1).value; })
  .then(function () { output(1).on(); });
```

This fires when DI0 is high and DO1 is still low (so it latches DO1 on once). Because
the condition is a predicate function, the rule is **wildcard**: it is re-evaluated on
every event, and reads `input(0)`/`output(1)` live each time. See
[the fact model](#how-events-reach-rules-the-fact-model) for the trade-off.

Since `input` and `output` are both facts, this particular gate is better written
declaratively — targeted dispatch, no wildcard:

```js
rule('complex gate')
  .when(input(0).isOn(), output(1).isOff())
  .then(function () { output(1).on(); });
```

---

## 2. Facts and helpers

There are three fact sources. `input` and `mqtt` are **conditions** (usable in
`when()`); `output` is for **actions and reading state** in `then()`. `mqtt` is also
an **actuator**: call `.publish()` in `then()` to send a message to the broker.

> **Conditions are immutable.** `input()` / `mqtt()` build a *base pattern*, and every
> constraint method (`.is()`, `.isOn()`, `.isOff()`, `.once()`) returns a **new**
> narrowed pattern — the base is never mutated. This means you can safely branch a
> base:
> ```js
> var di5 = input(5);
> rule('on').when(di5.isOn()).then(...);   // independent
> rule('off').when(di5.isOff()).then(...);  // independent
> // di5 itself stays unconstrained, still usable for di5.value / di5.get()
> ```

### `input(ch)` — digital input

`ch` is the channel number (0–7).

| Form | Meaning |
|------|---------|
| `input(ch)` | **bare pattern** — always matches (a digital input always exists) |
| `input(ch).is(bool)` | matches when the input equals `bool` |
| `input(ch).isOn()` | alias for `.is(true)` |
| `input(ch).isOff()` | alias for `.is(false)` |
| `input(ch).value` | live level as a property (read in `then()`) |
| `input(ch).get()` | live level as a method (same as `.value`) |

```js
// Gate on a specific level
rule('start on DI2 high')
  .when(input(2).isOn())
  .then(function () { output(2).on(); });

// Use a bare input as the trigger, read its value in the action
var di3 = input(3);
rule('copy DI3 inverted to DO3')
  .when(di3)
  .then(function () {
    output(3).set(!di3.get());          // DO3 = NOT DI3
  });
```

A **bare** `input(ch)` is always true — it is a *trigger/value source*, not a gate.
To gate, add `.is(...)` / `.isOn()` / `.isOff()`.

### `output(ch)` — digital output

`output(ch)` is **dual-purpose**: an *actuator* (drive/read the output, used in
`then()`) **and** a *fact* (gate on the output's state, used in `when()`).

**Actuator** (in `then()`):

| Method | Effect | Returns |
|--------|--------|---------|
| `output(ch).set(bool)` | set to `bool` | resulting level |
| `output(ch).on()` | set high | resulting level (`true`) |
| `output(ch).off()` | set low | resulting level (`false`) |
| `output(ch).toggle()` | invert current level | resulting level |
| `output(ch).get()` | read current level | level |
| `output(ch).value` | read current level (property) | level |

The mutators return the resulting level, which is handy for logging:

```js
rule('toggle DO0 on trigger')
  .when(mqtt('rules/trigger').is(function (m) { return m === 'ON'; }))
  .then(function () {
    print('DO0 is now: ' + output(0).toggle());   // logs true/false
  });
```

**Fact** (in `when()`) — `output(ch)` is a condition, just like `input`:

| Form | Meaning |
|------|---------|
| `output(ch)` | bare pattern — always matches |
| `output(ch).is(bool)` | matches when the output equals `bool` |
| `output(ch).isOn()` | alias for `.is(true)` |
| `output(ch).isOff()` | alias for `.is(false)` |

Writing an output that **changes** its level is itself an event, so a rule gated on an
output re-evaluates when that output changes — which lets you chain outputs:

```js
// When DO0 turns on, also turn DO1 on.
rule('DO1 follows DO0')
  .when(output(0).isOn())
  .then(function () { output(1).on(); });
```

> **Feedback loops are bounded.** Because outputs are written *by* rules, output→output
> chains can feed back on themselves. The engine caps the cascade at **16 hops per
> triggering event**; if exceeded it logs a warning and stops, so a runaway loop can't
> wedge the engine (or trip the device watchdog). Writing an output to the value it
> already holds is a no-op and emits no event.

### `led()` and `buzzer()` — LED and buzzer actuators

Two more actuators for `then()` bodies. They are **write-only** (not conditions) and
deliberately simple — no per-call duration or sequences like the MQTT LED/buzzer paths;
when you want timing, drive them from the engine's own timers (`.after()`, `.heldFor()`,
`every()`, `cron()`).

| Form | Effect |
|------|--------|
| `led().set(r, g, b)` | set the RGB LED (each channel `0`–`255`) |
| `led().set("#rrggbb")` | set from a hex colour (leading `#` optional) |
| `led().off()` | turn the LED off (black) |
| `buzzer().set(freqHz)` | play a continuous tone (`100`–`10000` Hz) |
| `buzzer().off()` | silence the buzzer |

```js
// Visible + audible alarm while DI0 is active; clear it when DI0 releases.
rule('alarm on')
  .when(input(0).isOn())
  .then(function () { led().set('#ff0000'); buzzer().set(2000); });

rule('alarm off')
  .when(input(0).isOff())
  .then(function () { led().off(); buzzer().off(); });
```

> **The LED needs IO mode.** `led()` only takes effect when the LED is configured in
> **IO** mode; in **status** mode the firmware owns the LED (showing connectivity) and the
> write is ignored. The buzzer is always available.

### `mqtt(topic)` — MQTT message

`mqtt(topic)` is dual-purpose: a **condition** (usable in `when()`) and an **actuator**
(usable in `then()` to publish back to the broker).

**As a condition:**

| Form | Meaning |
|------|---------|
| `mqtt(topic)` | matches once **any** message has arrived on `topic` (level/held) |
| `mqtt(topic).is(fn)` | matches when `fn(payload)` is truthy |
| `mqtt(topic).once()` | **edge** — matches only on each message arrival, not while held |
| `mqtt(topic).value` | last received payload (read in `then()`) |

`.is(fn)` receives the raw payload string:

```js
var trig = mqtt('rules/trigger').is(function (m) { return m === 'ON'; });

rule('on command')
  .when(trig)
  .then(function () {
    print('payload was: ' + trig.value);
    output(0).on();
  });
```

#### Level vs. edge

By default an `mqtt` condition is **level-triggered**: once a matching message has
arrived, the condition stays satisfied (it holds the last payload). The rule may then
re-fire on later events while that payload still matches.

`.once()` makes it **edge-triggered**: it is satisfied only during the evaluation pass
caused by the message itself. Use it for "do this once per command":

```js
// Toggles DO0 exactly once per matching message, even if other inputs change
rule('toggle per command')
  .when(mqtt('rules/trigger').is(function (m) { return m === 'ON'; }).once(),
        input(0).isOff())
  .then(function () { output(0).toggle(); });
```

`.once()` is a true edge: if the rule's other conditions are not met at the instant the
message arrives, the edge is **missed**, not queued.

#### Publishing — `mqtt(topic).publish(payload [, opts])`

In `then()` bodies, `mqtt(topic)` can also **send** a message to the broker:

| Form | Effect |
|------|--------|
| `mqtt(topic).publish(payload)` | publish `payload` with QoS 0, non-retained |
| `mqtt(topic).publish(payload, {qos: n})` | publish with QoS `0`, `1`, or `2` |
| `mqtt(topic).publish(payload, {retain: true})` | publish with the retained flag set |
| `mqtt(topic).publish(payload, {qos: n, retain: bool})` | explicit QoS and retained |

`payload` is coerced to a string. Returns the broker message-id (`>= 0`) on success,
or `-1` if the broker is not connected. The configured topic prefix is prepended
automatically (a topic starting with `/` bypasses the prefix, same as all other MQTT
operations).

```js
// Report the current output state every minute
rule('heartbeat')
  .when(every(60000))
  .then(function () {
    mqtt('status/do0').publish(output(0).value ? 'ON' : 'OFF', { retain: true });
  });

// Forward an incoming command to a second topic with QoS 1
var cmd = mqtt('rules/cmd');
rule('forward')
  .when(cmd)
  .then(function () {
    mqtt('state/last-cmd').publish(cmd.value, { qos: 1, retain: true });
  });
```

### Time triggers — `every(ms)` and `cron(...)`

`input`/`output`/`mqtt` react to *external* changes. Time triggers are the opposite —
the engine fires its own event on a **schedule**. They are conditions you put in
`when()`, and they are **edges** (satisfied only during their own tick), so any other
conditions act as gates.

| Form | Fires |
|------|-------|
| `every(ms)` | every `ms` milliseconds (**minimum 100 ms**; first tick one period after the rule loads) |
| `cron("m h dom mon dow")` | on a standard 5-field cron schedule |

```js
// Poll a sensor every 5 s, but only while enabled (DI0 high)
rule('poll')
  .when(every(5000), input(0).isOn())
  .then(function () { output(0).toggle(); });

// Every day at 07:00, turn DO1 on
rule('morning')
  .when(cron('0 7 * * *'))
  .then(function () { output(1).on(); });
```

**Cron syntax** is the standard five fields — `minute hour day-of-month month day-of-week`
— supporting `*`, lists (`a,b`), ranges (`a-b`), and steps (`*/n`, `a-b/n`, and `a/n` =
from `a` to the field max). Fields are **numeric only** (no `JAN`/`MON` names). Day-of-week
is `0`–`6` with Sunday `0` (`7` also accepted). When **both** day-of-month and day-of-week
are restricted, a day matches if **either** does (Vixie semantics).

> **Cron runs in the device's local time and only once the clock is real.**
> Fields are evaluated in **local time** — the timezone configured on the *Time* page (a
> POSIX `TZ` string; **UTC** when none is set), with DST handled by the zone's rules. The
> wall-clock comes from the RTC at boot and from SNTP once the network is up. **Until the
> clock is valid, cron stays suppressed** (so `0 7 * * *` can't fire ~7 h after a 1970
> boot); when SNTP first syncs, cron is armed/re-armed against the corrected time. `every(ms)`
> is unaffected — it uses a purely relative timer and runs immediately.
>
> *DST edge cases:* on spring-forward, a wall-clock time in the skipped hour never occurs,
> so a job scheduled then is skipped that day; on fall-back, a repeated wall-clock time
> fires once. This is inherent to local-time cron — use UTC if you need a fixed cadence
> across DST.

Each `every`/`cron` is its own fact, so its tick only re-evaluates the rules that use it,
and reloading the rules cancels the timers.

### Synthetic facts — `fact(initial)`

A `fact` is a value you compute in some rules and gate on in others — the classic
"calculated fact" pattern. It holds an arbitrary value (`.set(v)` / `.get()` / `.value`)
and is itself a fact source: `.set()` emits a change event (only when the value actually
changes) so rules gating on it re-evaluate.

| Form | Meaning |
|------|---------|
| `fact(initial)` | create a value holder (used as a condition: bare → always matches) |
| `f.set(v)` | set the value (emits a change event); returns the new value |
| `f.get()` / `f.value` | read the current value |
| `f.is(fn)` | condition: matches when `fn(value)` is truthy |
| `f.is(x)` | condition: matches when `value === x` |
| `f.isTrue()` / `f.isFalse()` | condition: matches when `value` is truthy / falsy |

```js
var sunElevation = fact(0);

// one rule calculates the fact …
rule('update sun elevation')
  .when(every(60000))
  .then(function () {
    sunElevation.set(/* compute from time + lat/lon … */ Math.random() * 90 - 45);
  });

// … others gate on it
rule('light on at night')
  .when(sunElevation.is(function (e) { return e < 5; }))
  .then(function () { output(1).on(); });

rule('light off at sunrise')
  .when(sunElevation.is(function (e) { return e > 10; }))
  .then(function () { output(1).off(); });
```

A `fact` is its own dispatch fact (setting it only re-evaluates rules that use it), and
its change event runs through the same path as an output write — so the **cascade cap**
and **`.noLoop()`** apply (a rule that sets a fact it also gates on is bounded / can be
made self-suppressing). `set(v)` only emits when `v !== ` the current value (`===`
comparison, so it's change-detection for primitives; replace a held object rather than
mutating it in place).

### Liveness & fallback — `watchdog`, `.stale()`, MQTT state

The point of on-board rules is to keep working when the governing system or its link
goes away. These primitives detect that absence and let you gate fallback behaviour.

| Form | Meaning |
|------|---------|
| `watchdog(ms)` | a liveness fact: **alive** while fed within `ms`, **expired** otherwise |
| `wd.feed()` | keep-alive (in a `then()`) — re-arms the timeout, recovers if expired |
| `wd.isAlive()` / `wd.isExpired()` | conditions to gate on |
| `mqtt(topic).stale(ms)` | a watchdog **auto-fed** by messages on `topic` — matches when no message for `ms` |
| `mqttConnected()` / `mqttDown()` | the broker connection state (fed by the firmware) |
| `modbus(ms)` | upstream **MODBUS** command health: a watchdog auto-fed by each inbound coil/holding-register write |
| `can(ms)` | upstream **CAN/NMEA2000** command health: a watchdog auto-fed by each inbound control write |

A watchdog **starts alive with a grace period** (the timeout is armed when a rule using
it registers), so a healthy system has `ms` after boot to check in before fallback kicks
in. Feeding re-arms it; the alive→expired (timeout) and expired→alive (recovery)
transitions emit, so gating rules re-evaluate.

```js
// Fallback: while the controlling system is absent, DI0 toggles the lights locally.
var control = watchdog(90000);                              // expires after 90s with no sign of life

rule('control alive')                                       // feed from a heartbeat — or from the
  .when(mqtt('sys/heartbeat')).then(() => control.feed());  // commands the system re-issues

rule('manual override')
  .when(input(0).isOn(), control.isExpired())
  .then(() => { output(0).toggle(); output(1).toggle(); /* … */ });
```

This covers **both** failure modes: a broker/network outage and a dead controller both
stop the messages, so the watchdog expires either way (within `ms`). For the pure
heartbeat case, `mqtt('sys/heartbeat').stale(90000)` is the same thing without the
explicit feed rule:

```js
rule('manual override')
  .when(input(0).isOn(), mqtt('sys/heartbeat').stale(90000))
  .then(() => { /* … */ });
```

`mqttConnected()` / `mqttDown()` reflect the broker connection **immediately** (the
firmware feeds connect/disconnect as an internal `$sys/mqtt` event), so you can react to
a clean disconnect without waiting for a watchdog timeout — handy as a faster companion
to the watchdog. They become meaningful after the first connection event.

`modbus(ms)` and `can(ms)` extend the same idea to the fieldbuses: each is a watchdog the
firmware auto-feeds whenever an upstream **command** arrives (a MODBUS coil/holding-register
write, or a CAN/NMEA2000 control write — reads/polls don't count). So `.isAlive()` means a
command came within `ms`, and `.isExpired()` means that control link has gone quiet — the
same fallback gate as the MQTT case, for a different transport:

```js
// If the MODBUS master stops commanding us for 10 s, fall back to local control.
rule('modbus fallback')
  .when(input(0).isOn(), modbus(10000).isExpired())
  .then(() => output(0).toggle());
```

Like any watchdog they **start alive with an `ms` grace period** from when the rule
registers, so the upstream has that long after boot to begin commanding before fallback
kicks in. `can(ms)` covers both basic-CAN and NMEA2000 modes (whichever is configured).

> **A note on trust.** Time-based and liveness facts are only as trustworthy as their
> inputs — an MQTT heartbeat is unauthenticated, and NTP is unauthenticated too, so a
> spoofed sync could make cron fire at the wrong moment. For anything safety-relevant,
> treat fallback as a local interlock, not a security control.

### How events reach rules (the fact model)

Each `input(ch)`, `output(ch)`, and `mqtt(topic)` is a **distinct fact**. When something
happens, the engine only re-evaluates the rules that reference the *changed* fact:

- A change on **DI4** re-evaluates only rules whose conditions mention `input(4)`.
  A rule gated on `input(7)` is **not** touched.
- An **MQTT message** on `topic` re-evaluates only rules referencing `mqtt(topic)`.
- Writing **DO0** to a new level re-evaluates only rules referencing `output(0)`.
- An **`every`/`cron` tick** re-evaluates only the rules using that specific trigger.

This is why toggling one input never accidentally fires a rule about another input.

**Outputs are event sources too.** Writing an output that changes its level emits an
`output:ch` event, so rules gated on it re-evaluate (this is what enables output→output
chaining). Because outputs are written by rules, a per-event **cascade cap (16 hops)**
bounds the chain so a feedback loop can't run away — see
[`output(ch)`](#outputch--digital-output). An idempotent write (same value) emits nothing.

**Wildcard rules.** If a rule has an *opaque* condition — a predicate function or a bare
value — the engine cannot tell which facts it depends on, so the rule is treated as
**wildcard** and re-evaluated on *every* event. Prefer `input()` / `output()` / `mqtt()`
conditions when you want the targeted behavior.

---

## 3. Time-based rules

Two builder methods add timing. They are **different** and not interchangeable.

| Method | Reads as | Semantics | If conditions break during the wait |
|--------|----------|-----------|--------------------------------------|
| `.heldFor(ms)` | "conditions held for `ms`, then …" | **sustained gate** (delay-on / debounce) | the pending fire is **cancelled** |
| `.after(ms)` | "do this, then `ms` later do that" | **scheduled follow-up** (timeline delay) | the follow-up **still runs** (fire-and-forget) |

### `.heldFor(ms)` — sustained gate / debounce

Place it **before** `.then()`. The action fires only if the `when()` conditions stay
true continuously for `ms` milliseconds. If they break first, the timer is cancelled.
It fires **once per sustained period** (it re-arms after the conditions drop and rise
again).

```js
// DO5 turns on only after DI5 has been high continuously for 5 seconds.
rule('DI5 held 5s -> DO5 on')
  .when(input(5).isOn())
  .heldFor(5000)
  .then(function () { output(5).on(); });

// Pair it with a release rule:
rule('DI5 off -> DO5 off')
  .when(input(5).isOff())
  .then(function () { output(5).off(); });
```

Typical uses: debouncing a noisy input, "long press" detection, delay-on timers.

### `.after(ms)` — scheduled follow-up

Place it **between** two `.then()` steps. The next step runs `ms` after the previous
one, **regardless** of whether the conditions still hold. This is for pulses and
sequences.

```js
// Pulse: DO4 on now, off 5 seconds later.
rule('DI4 pulses DO4 for 5s')
  .when(input(4).isOn())
  .then(function () { output(4).on(); })
  .after(5000)
  .then(function () { output(4).off(); });
```

If DI4 drops within the 5 seconds, the `off` step **still runs** — that's the point of a
fire-and-forget pulse (otherwise the output could latch on forever).

---

## 4. Rule chaining

"Chaining" comes in two flavors: chaining **actions** within one rule, and composing
**multiple rules** into a behavior.

### Chaining actions (a timeline)

`.then()` returns the rule, so you can keep chaining `.after(ms).then(...)` to build a
timeline of steps. Step 0 runs immediately (or after `heldFor`); each subsequent step
runs after its `.after()` delay relative to the previous one.

```js
// Blink DO6 three times (on/off/on/off/on/off), 200 ms apart.
rule('blink DO6 thrice')
  .when(mqtt('cmd/blink').once())
  .then(function () { output(6).on();  })
  .after(200).then(function () { output(6).off(); })
  .after(200).then(function () { output(6).on();  })
  .after(200).then(function () { output(6).off(); })
  .after(200).then(function () { output(6).on();  })
  .after(200).then(function () { output(6).off(); });
```

Each `.after(ms)` is relative to the previous step, so the steps above land at
0 / 200 / 400 / 600 / 800 / 1000 ms.

### Composing rules (a small state machine)

Several rules sharing facts cooperate to implement behavior. This is the common way to
build something stateful — each rule handles one transition.

```js
// A trigger topic that means ON/OFF, plus DI0 as a mode selector.
var trig = mqtt('rules/trigger').is(function (m) { return m === 'ON'; });

// ON + DI0 low  -> toggle DO0
rule('toggle')
  .when(trig, input(0).isOff())
  .then(function () { print('toggle: ' + output(0).toggle()); });

// ON + DI0 high -> copy DI1 into DO0 (latch from DI1)
rule('latch from DI1')
  .when(trig, input(0).isOn())
  .then(function () { print('latch: ' + output(0).set(input(1).get())); });

// OFF -> force DO0 off
rule('turn off')
  .when(mqtt('rules/trigger').is(function (m) { return m === 'OFF'; }))
  .then(function () { output(0).off(); });
```

Rules can also chain **through output state**: because writing an output is an event
(see the [fact model](#how-events-reach-rules-the-fact-model)), one rule writing
`output(n)` re-evaluates another rule gated on `output(n).isOn()`. For example, a fault
output can fan out to several reactions:

```js
rule('fault -> alarm') .when(output(7).isOn()).then(function () { output(6).on(); });
rule('fault -> shed')  .when(output(7).isOn()).then(function () { output(0).off(); });
```

> **Mind the feedback loop.** Output→output chains can cycle (A writes DO0 → B writes DO1
> → A …). The engine caps the cascade at 16 hops per triggering event and logs a warning
> if exceeded, but a rule that *fights itself* (e.g. toggles an output it gates on) will
> burn the budget every event — keep chains acyclic.

### `.noLoop()` — don't re-fire from your own consequence

A rule that writes a fact it also gates on would re-fire itself (bounded by the cascade
cap, but still). `.noLoop()` suppresses exactly that: the rule won't be re-fired by
changes *its own action* causes. Other rules still react to its writes, and it still
fires on later, independent events — so it's per-rule self-suppression, not a global
"stop cascading". (Same idea as Drools' `no-loop` rule attribute.)

```js
// Without .noLoop() this would re-fire itself every time it flips DO0
// (until the cascade cap); with it, exactly one flip per trigger.
rule('blink on command')
  .when(mqtt('cmd/blink').once())
  .noLoop()
  .then(function () { output(0).toggle(); });
```

`.noLoop()` is order-independent in the chain (like `.heldFor()`).

---

## 5. Quick reference

```js
// ── Rule ──────────────────────────────────────────────────────────────────
rule(name).when(cond, cond, …).then(fn)      // AND of conditions
          .heldFor(ms).then(fn)              // fire after conditions held ms
          .then(a).after(ms).then(b)         // a now, b ms later (timeline)
          .noLoop().then(fn)                 // don't re-fire from own consequence

// ── input(ch) — condition + live value ─────────────────────────────────────
input(ch)                 // bare: always matches
input(ch).is(bool)        // matches when level == bool
input(ch).isOn()          // .is(true)
input(ch).isOff()         // .is(false)
input(ch).value           // live level (property)
input(ch).get()           // live level (method)

// ── output(ch) — actuator AND fact ─────────────────────────────────────────
output(ch).set(bool)      // → resulting level   (a value change emits an event)
output(ch).on()           // → true
output(ch).off()          // → false
output(ch).toggle()       // → resulting level
output(ch).get()          // current level
output(ch).value          // current level (property)
output(ch)                // condition: bare → always matches
output(ch).is(bool)       // condition: matches when level == bool
output(ch).isOn()         // .is(true)
output(ch).isOff()        // .is(false)

// ── led() / buzzer() — actuators (then() only) ─────────────────────────────
led().set(r, g, b)        // set RGB (0-255 each)
led().set('#rrggbb')      // set from hex (IO mode only; ignored in status mode)
led().off()               // LED off
buzzer().set(freqHz)      // continuous tone (100-10000 Hz)
buzzer().off()            // silence

// ── mqtt(topic) — condition + last payload + publisher ─────────────────────
mqtt(topic)               // matches once any message arrived (level/held)
mqtt(topic).is(fn)        // matches when fn(payload) is truthy
mqtt(topic).once()        // edge: matches only on each arrival
mqtt(topic).value         // last payload
mqtt(topic).publish(payload)                    // send; QoS 0, non-retained (default)
mqtt(topic).publish(payload, {qos: n})          // QoS 0|1|2
mqtt(topic).publish(payload, {retain: true})    // retained
mqtt(topic).publish(payload, {qos: n, retain: bool})  // both

// ── time triggers (edge; gate with other conditions) ───────────────────────
every(ms)                 // fires every ms
cron('m h dom mon dow')   // 5-field cron schedule (local time; suppressed until clock valid)

// ── fact(initial) — synthetic value: set by rules, gated on by rules ────────
var f = fact(initial)     // value holder + condition
f.set(v)                  // set (emits on change) → new value
f.get() / f.value         // read
f.is(fn)                  // condition: fn(value) is truthy
f.is(x)                   // condition: value === x
f.isTrue() / f.isFalse()  // condition: value truthy / falsy

// ── liveness / fallback ─────────────────────────────────────────────────────
var wd = watchdog(ms)     // alive while fed within ms, else expired
wd.feed()                 // keep-alive (in then())
wd.isAlive() / wd.isExpired()           // conditions
mqtt(topic).stale(ms)     // condition: no message on topic for ms (auto-fed watchdog)
mqttConnected() / mqttDown()            // broker connection state (host-fed)
modbus(ms).isAlive() / .isExpired()     // upstream MODBUS command health (auto-fed)
can(ms).isAlive() / .isExpired()        // upstream CAN/NMEA2000 command health (auto-fed)

// ── in then() bodies ───────────────────────────────────────────────────────
print(…)                  // log to the console (args joined by space)
```

---

## 6. Gotchas

- **`when()` is AND-only.** Use a predicate function or multiple rules for OR.
- **Bare `input(ch)` always matches** — it is a value source, not a gate. Add
  `.is()/.isOn()/.isOff()` to gate.
- **`output(ch)` is both an actuator and a fact.** Gate on it with `output(0).isOn()` /
  `.isOff()`; a bare `output(0)` always matches. Writing an output that changes value is
  an event, so output→output chains work — but they can cycle, so the engine caps the
  cascade at 16 hops per event. Keep chains acyclic.
- **`mqtt(topic)` is held by default.** Once a message arrives it stays matched; use
  `.once()` for per-message edge behavior.
- **`.heldFor()` vs `.after()`** are different: `heldFor` cancels if conditions drop;
  `after` fires regardless. Don't swap them.
- **Edges can be missed.** A `.once()` edge fires only if the rule's other conditions
  are met at the instant the message arrives; it is not queued.
- **Reload resets everything.** Reloading the rule set (device `EVT_RELOAD`, simulator
  "Apply Rules") clears all rules and cancels any pending `.after()` / `.heldFor()`
  timers.

> **Simulator note:** the simulator loads the engine (`dsl.js`) once at page load.
> Editing your *script* and clicking **Apply Rules** re-runs the script, but editing the
> *engine* requires a full **page reload** to take effect.

---

## Tests

The engine has a test suite (Node's built-in runner, zero dependencies) that loads the
real `dsl.js` into a sandbox with mock I/O and a deterministic fake clock:

```sh
cd components/scripting && npm test
```

Tests live in `components/scripting/test/`. When you change the engine, run the suite
first and add a case for any new behaviour.
