// Device list and message relay between browsers and devices
const { test, before, after } = require('node:test');
const assert = require('node:assert/strict');
const { startServer, device, browser, online, sleep, commands } = require('./helpers');

let server;
before(async () => { server = await startServer(); });
after(() => server.stop());

test('browser gets devices, alarms and tones on hello', async () => {
  const br = await browser(server);
  await br.waitFor((m) => m.type === 'devices');
  await br.waitFor((m) => m.type === 'alarms' && Array.isArray(m.alarms));
  await br.waitFor((m) => m.type === 'tones' && Array.isArray(m.tones));
  br.close();
});

test('device appears when it connects and disappears when it closes', async () => {
  const br = await browser(server);
  const dev = await device(server, 'esp32-relay01');
  await online(br, 'esp32-relay01');
  dev.close();
  await br.waitFor((m) => m.type === 'devices' && !m.devices.includes('esp32-relay01')
    && br.messages.indexOf(m) > 0);
  br.close();
});

test('browser commands reach only the target device', async () => {
  const br = await browser(server);
  const a = await device(server, 'esp32-relay-a');
  const b = await device(server, 'esp32-relay-b');
  await online(br, 'esp32-relay-b');

  br.sendJson({ type: 'blink', target: 'esp32-relay-a' });
  br.sendJson({ type: 'volume', target: 'esp32-relay-a', value: 35 });
  await a.waitFor((m) => m.type === 'volume' && m.value === 35);
  assert.ok(a.messages.some((m) => m.type === 'blink'));
  await sleep(100);
  assert.equal(commands(b).length, 0, 'the other device got no commands');
  a.close(); b.close(); br.close();
});

test('device messages reach browsers tagged with the device id', async () => {
  const br = await browser(server);
  const dev = await device(server, 'esp32-relay02');
  await online(br, 'esp32-relay02');
  dev.sendJson({ type: 'stats', heapFree: 1234 });
  const got = await br.waitFor((m) => m.type === 'stats');
  assert.equal(got.deviceId, 'esp32-relay02');
  assert.equal(got.heapFree, 1234);
  dev.close(); br.close();
});

test('a reconnecting device replacing its old socket stays online', async () => {
  const br = await browser(server);
  const first = await device(server, 'esp32-relay03');
  await online(br, 'esp32-relay03');
  const second = await device(server, 'esp32-relay03');
  await sleep(100);
  first.close(); // old socket closing late must not mark the device offline
  await sleep(200);
  br.sendJson({ type: 'blink', target: 'esp32-relay03' });
  await second.waitFor((m) => m.type === 'blink');
  second.close(); br.close();
});

test('a page opened later still gets each device\'s latest status reports', async () => {
  const dev = await device(server, 'esp32-replay');
  const early = await browser(server);
  await online(early, 'esp32-replay');
  dev.sendJson({ type: 'alarms_ack', version: 'v1', count: 1 });
  dev.sendJson({ type: 'alarms_ack', version: 'v2', count: 2 }); // only the latest is kept
  dev.sendJson({ type: 'cache', stored: 1, wanted: 1 });
  dev.sendJson({ type: 'ringing' });                              // events are not replayed
  await early.waitFor((m) => m.type === 'cache');

  const late = await browser(server);
  const ack = await late.waitFor((m) => m.type === 'alarms_ack' && m.deviceId === 'esp32-replay');
  assert.equal(ack.version, 'v2');
  await late.waitFor((m) => m.type === 'cache' && m.deviceId === 'esp32-replay');
  await sleep(100);
  assert.equal(late.messages.filter((m) => m.type === 'ringing').length, 0);
  dev.close(); early.close(); late.close();
});

test('devices are told when a page is open, so they only send stats often then', async () => {
  // Each test file has its own server, but earlier tests here may have left pages open: start clean
  await sleep(200);
  const dev = await device(server, 'esp32-watch01');
  const first = await dev.waitFor((m) => m.type === 'watch');
  assert.equal(first.on, false, 'no page open yet');

  const page = await browser(server);
  await dev.waitFor((m) => m.type === 'watch' && m.on === true);
  page.close();
  const off = await dev.waitFor((m) => m.type === 'watch' && m.on === false && dev.messages.indexOf(m) > dev.messages.indexOf(first));
  assert.equal(off.on, false);
  dev.close();
});
