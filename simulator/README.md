# Rule engine simulator

A browser sandbox for the rule engine DSL. It runs the **live firmware engine source**
(`../components/scripting/dsl.js`) in your browser with mock I/O, so you can prototype and
test rules — inputs, outputs, MQTT, time triggers, LED/buzzer, MODBUS/CAN health — without
flashing a device.

## Start it

Requires Node.js ≥ 16. No dependencies to install.

```sh
cd simulator
npm start            # → http://localhost:3000
```

`npm start` runs `node server.js`. Open the printed URL in a browser.

To use a different port:

```sh
PORT=8080 npm start
```

The server binds to `127.0.0.1` only (local access).

## Why a server is needed

The simulator deliberately does **not** embed a copy of the engine. `server.js` serves two
things:

- `/` → `index.html`
- `/dsl.js` → `../components/scripting/dsl.js`, the exact source the firmware embeds, with
  `Cache-Control: no-store` so edits are never served stale.

Opening `index.html` directly via `file://` will not work — the page fetches `/dsl.js` over
HTTP, and `file://` can't serve it (and would cache it).

## Apply ≠ reload

The page loads `dsl.js` **once** at page load. Clicking **Apply** re-runs your *rules*
against the already-loaded engine. If you edit `dsl.js` itself (the engine), **reload the
page** to pick up the change — Apply alone won't.

## Tests

The headless test suite for the same engine lives in `../components/scripting`:

```sh
cd ../components/scripting
npm test
```
