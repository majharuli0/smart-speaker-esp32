// Tone upload (as the browser sends it) and serving (as the ESP32 fetches it)
const { test, before, after } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { startServer, browser } = require('./helpers');

let server;
before(async () => { server = await startServer(); });
after(() => server.stop());

// 16 kHz 16-bit mono WAV with n samples, like the web page produces
function wav(n = 1600) {
  const b = Buffer.alloc(44 + n * 2);
  b.write('RIFF', 0); b.writeUInt32LE(36 + n * 2, 4); b.write('WAVEfmt ', 8);
  b.writeUInt32LE(16, 16); b.writeUInt16LE(1, 20); b.writeUInt16LE(1, 22);
  b.writeUInt32LE(16000, 24); b.writeUInt32LE(32000, 28); b.writeUInt16LE(2, 32); b.writeUInt16LE(16, 34);
  b.write('data', 36); b.writeUInt32LE(n * 2, 40);
  return b;
}

// No Content-Type header, exactly like the browser's ArrayBuffer upload
const upload = (name, body) => fetch(`${server.url}/tones`, { method: 'POST', headers: { 'X-Filename': name }, body });

test('upload with no Content-Type is saved under a safe name and announced', async () => {
  const br = await browser(server);
  const res = await upload('../evil name!.wav', wav());
  assert.equal(res.status, 200);
  assert.ok(fs.existsSync(path.join(server.dataDir, 'tones', 'evil_name_.wav')), 'path stripped, unsafe chars replaced');
  await br.waitFor((m) => m.type === 'tones' && m.tones.includes('evil_name_.wav'));
  br.close();
});

test('the device can download a tone with its exact size', async () => {
  const body = wav(8000);
  await upload('download.wav', body);
  const res = await fetch(`${server.url}/tones/download.wav`);
  assert.equal(res.status, 200);
  assert.equal(Number(res.headers.get('content-length')), body.length, 'firmware relies on Content-Length');
  assert.deepEqual(Buffer.from(await res.arrayBuffer()), body);
});

test('non-wav names and empty bodies are rejected', async () => {
  assert.equal((await upload('song.mp3', wav())).status, 400);
  assert.equal((await upload('empty.wav', Buffer.alloc(0))).status, 400);
});

test('CORS allows the page to upload when opened from a file', async () => {
  const res = await fetch(`${server.url}/tones`, { method: 'OPTIONS' });
  assert.equal(res.headers.get('access-control-allow-origin'), '*');
  assert.match(res.headers.get('access-control-allow-headers'), /X-Filename/);
});

// ---- Tones the device should store (sent with its alarm list) ----
const crypto = require('node:crypto');
const { device, online } = require('./helpers');
const sha256 = (buf) => crypto.createHash('sha256').update(buf).digest('hex');

test('alarm sync lists the tones the alarms use, with size and SHA-256', async () => {
  const body = wav(4000);
  await upload('cached.wav', body);
  const br = await browser(server);
  const dev = await device(server, 'esp32-cache1');
  await online(br, 'esp32-cache1');
  br.sendJson({ type: 'alarm_save', alarm: { deviceId: 'esp32-cache1', time: '07:00', days: [1], tone: 'cached.wav' } });
  br.sendJson({ type: 'alarm_save', alarm: { deviceId: 'esp32-cache1', time: '08:00', days: [1], tone: 'cached.wav' } });
  br.sendJson({ type: 'alarm_save', alarm: { deviceId: 'esp32-cache1', time: '09:00', days: [1], tone: 'missing.wav' } });
  const sync = await dev.waitFor((m) => m.type === 'alarms_sync' && m.alarms.length === 3);
  assert.deepEqual(sync.tones, [{ name: 'cached.wav', size: body.length, sha256: sha256(body) }],
    'each tone once; a tone with no file is left out');
  dev.close(); br.close();
});

test('re-uploading a tone in use sends devices a new version to download', async () => {
  await upload('changing.wav', wav(1000));
  const br = await browser(server);
  const dev = await device(server, 'esp32-cache2');
  await online(br, 'esp32-cache2');
  br.sendJson({ type: 'alarm_save', alarm: { deviceId: 'esp32-cache2', time: '07:00', days: [1], tone: 'changing.wav' } });
  const before = await dev.waitFor((m) => m.type === 'alarms_sync' && m.tones?.length === 1);

  const newBody = wav(2000);
  await upload('changing.wav', newBody);
  const after = await dev.waitFor((m) => m.type === 'alarms_sync' && m.tones?.[0]?.sha256 === sha256(newBody));
  assert.notEqual(after.version, before.version);
  assert.equal(after.tones[0].size, newBody.length);
  dev.close(); br.close();
});
