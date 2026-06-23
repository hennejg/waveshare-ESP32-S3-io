#!/usr/bin/env node
// Serves the rule engine simulator.
// GET /        → simulator/index.html
// GET /dsl.js  → components/scripting/dsl.js  (live from firmware source)
const http = require('http');
const fs   = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');
const PORT = process.env.PORT || 3000;

const ROUTES = {
  '/':        path.join(__dirname, 'index.html'),
  '/dsl.js':  path.join(ROOT, 'components/scripting/dsl.js'),
  // CodeMirror editor assets — served from the same self-hosted files the firmware ships.
  '/codemirror.min.js':            path.join(ROOT, 'www/codemirror.min.js'),
  '/codemirror.min.css':           path.join(ROOT, 'www/codemirror.min.css'),
  '/codemirror-javascript.min.js': path.join(ROOT, 'www/codemirror-javascript.min.js'),
};

const MIME = { '.html': 'text/html', '.js': 'application/javascript', '.css': 'text/css' };

http.createServer((req, res) => {
  const file = ROUTES[req.url];
  if (!file) { res.writeHead(404); res.end('Not found'); return; }
  try {
    const content = fs.readFileSync(file);
    res.writeHead(200, {
      'Content-Type': MIME[path.extname(file)] || 'text/plain',
      // dsl.js is edited live during development — never let the browser cache it,
      // otherwise a reload keeps running a stale engine (see "Apply ≠ reload").
      'Cache-Control': 'no-store',
    });
    res.end(content);
  } catch (e) {
    res.writeHead(500); res.end(String(e));
  }
}).listen(PORT, '127.0.0.1', () => {
  console.log(`Rule Engine Simulator → http://localhost:${PORT}`);
  console.log(`DSL source: ${ROUTES['/dsl.js']}`);
});
