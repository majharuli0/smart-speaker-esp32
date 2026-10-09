// Firmware updates: upload checks, serving the file, and sending the update to the right devices
const { test, before, after } = require('node:test');
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const { startServer, device, browser, online } = require('./helpers');

let server;
before(async () => { server = await startServer(); });
after(() => server.stop());

// A fake firmware image: ESP32 image byte + the marker the real firmware carries
function firmware(board, version, size = 4096) {
  const buf = Buffer.alloc(size, 0xab);
  buf[0] = 0xe9;
  buf.write(`SSFW:${board}:${version}:END`, 1000, 'latin1');
  return buf;
}
const upload = (body) => fetch(`${server.url}/firmware`, { method: 'POST', body });
const sha256 = (buf) => crypto.createHash('sha256').update(buf).digest('hex');

test('a valid file is accepted, announced to pages and served for download', async () => {
  const br = await browser(server);
  await br.waitFor((m) => m.type === 'firmware');
  const bin = firmware('esp32s3', '9.9.1');
  const res = await upload(bin);
  assert.equal(res.status, 200);
  const meta = await res.json();
  assert.deepEqual([meta.board, meta.version, meta.size, meta.sha256], ['esp32s3', '9.9.1', bin.length, sha256(bin)]);

  const ann = await br.waitFor((m) => m.type === 'firmware' && m.firmware.esp32s3?.version === '9.9.1');
  assert.equal(ann.firmware.esp32s3.sha256, sha256(bin));

  const dl = await fetch(`${server.url}/firmware/esp32s3.bin`);
  assert.equal(Number(dl.headers.get('content-length')), bin.length);
  assert.deepEqual(Buffer.from(await dl.arrayBuffer()), bin);
  br.close();
});

test('files that are not this firmware are refused', async () => {
  const noMarker = Buffer.alloc(4096, 0); noMarker[0] = 0xe9;
  const notAnImage = firmware('esp32s3', '1.0.0'); notAnImage[0] = 0x00;
  assert.equal((await upload(noMarker)).status, 400, 'no SSFW marker');
  assert.equal((await upload(notAnImage)).status, 400, 'not an ESP32 image');
  assert.equal((await upload(Buffer.alloc(0))).status, 400, 'empty');
  assert.equal((await upload(firmware('esp32s3', '1.0.0', 3 * 1024 * 1024 + 1))).status, 413, 'bigger than the program slot');
});

test('an update goes only to devices of the matching board, with size and SHA-256', async () => {
  const bin = firmware('esp32', '9.9.2');
  await upload(bin);
  const br = await browser(server);
  const s3 = await device(server, 'esp32-ota-s3', { board: 'esp32s3' });
  const old = await device(server, 'esp32-ota-old', { board: 'esp32' });
  await online(br, 'esp32-ota-old');

  br.sendJson({ type: 'ota', target: 'esp32-ota-old' });
  const start = await old.waitFor((m) => m.type === 'ota_start');
  assert.deepEqual([start.path, start.version, start.size, start.sha256], ['/firmware/esp32.bin', '9.9.2', bin.length, sha256(bin)]);

  // The S3 gets the S3 file uploaded in the first test, never the esp32 one
  br.sendJson({ type: 'ota', target: 'esp32-ota-s3' });
  const s3start = await s3.waitFor((m) => m.type === 'ota_start');
  assert.equal(s3start.path, '/firmware/esp32s3.bin');
  s3.close(); old.close(); br.close();
});

test('updating an offline device, or one with no file for its board, is refused', async () => {
  const br = await browser(server);
  const dev = await device(server, 'esp32-ota-c3', { board: 'esp32c3' });
  await online(br, 'esp32-ota-c3');
  br.sendJson({ type: 'ota', target: 'esp32-ota-c3' });
  const noFile = await br.waitFor((m) => m.type === 'ota' && m.deviceId === 'esp32-ota-c3');
  assert.equal(noFile.state, 'failed');
  br.sendJson({ type: 'ota', target: 'esp32-nobody' });
  const offline = await br.waitFor((m) => m.type === 'ota' && m.deviceId === 'esp32-nobody');
  assert.equal(offline.error, 'device offline');
  dev.close(); br.close();
});
