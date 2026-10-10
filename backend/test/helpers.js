// Shared test helpers: start a real server on a free port with a throwaway
// data folder, and connect fake devices/browsers to it.
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const WebSocket = require('ws');

const SERVER = path.join(__dirname, '..', 'server.js');

function tempDir() {
  return fs.mkdtempSync(path.join(os.tmpdir(), 'speaker-test-'));
}

// Resolves once the server prints its port. MDNS=off so tests never answer
// the real ESP32's lookups; PORT=0 so they never clash with a server on 3000.
function startServer(dataDir = tempDir(), env = {}) {
  return new Promise((resolve, reject) => {
    const proc = spawn(process.execPath, [SERVER], {
      env: { ...process.env, PORT: '0', MDNS: 'off', DATA_DIR: dataDir, ...env },
    });
    let out = '';
    proc.stdout.on('data', (chunk) => {
      out += chunk;
      const m = out.match(/localhost:(\d+)/);
      if (m) {
        resolve({
          port: Number(m[1]),
          dataDir,
          url: `http://localhost:${m[1]}`,
          stop: () => new Promise((done) => { proc.once('exit', done); proc.kill(); }),
        });
      }
    });
    proc.stderr.on('data', (chunk) => { out += chunk; });
    proc.once('exit', (code) => reject(new Error(`server exited (${code}) before starting:\n${out}`)));
  });
}

// A WebSocket client that records every message and can wait for one.
// Binary messages are recorded as { binary: true, length }.
function connect(server, hello) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(`ws://localhost:${server.port}`);
    ws.messages = [];
    const waiters = [];
    ws.on('message', (data, isBinary) => {
      const msg = isBinary ? { binary: true, length: data.length } : JSON.parse(data);
      ws.messages.push(msg);
      for (const w of [...waiters]) {
        if (w.match(msg)) { waiters.splice(waiters.indexOf(w), 1); w.resolve(msg); }
      }
    });
    ws.sendJson = (msg) => ws.send(JSON.stringify(msg));
    // Waits for a message matching `match` (already received or future)
    ws.waitFor = (match, timeout = 2000) => {
      const seen = ws.messages.find(match);
      if (seen) return Promise.resolve(seen);
      return new Promise((res, rej) => {
        const w = { match, resolve: res };
        waiters.push(w);
        setTimeout(() => {
          waiters.splice(waiters.indexOf(w), 1);
          rej(new Error('timed out waiting for message; got: ' + JSON.stringify(ws.messages)));
        }, timeout);
      });
    };
    ws.on('open', () => { if (hello) ws.sendJson(hello); resolve(ws); });
    ws.on('error', reject);
  });
}

const device = (server, deviceId, extra = {}) => connect(server, { type: 'hello', role: 'device', deviceId, fw: 'test', ...extra });
const browser = (server) => connect(server, { type: 'hello', role: 'browser' });
// Every device is sent these when it connects; they aren't commands
const ON_CONNECT = ['timezone', 'alarms_sync', 'watch'];
const commands = (dev) => dev.messages.filter((m) => !ON_CONNECT.includes(m.type));

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// Resolves when a browser sees `deviceId` in its device list (the device finished registering)
const online = (br, deviceId) => br.waitFor((m) => m.type === 'devices' && m.devices.includes(deviceId));

module.exports = { startServer, connect, device, browser, sleep, online, tempDir, commands };
