// Hold-to-talk: routing of binary voice chunks, one talker per device
const { test, before, after } = require('node:test');
const assert = require('node:assert/strict');
const { startServer, device, browser, online, sleep, commands } = require('./helpers');

let server;
before(async () => { server = await startServer(); });
after(() => server.stop());

const chunk = () => Buffer.alloc(640); // 20 ms of 16 kHz 16-bit mono
const binaries = (ws) => ws.messages.filter((m) => m.binary).length;

test('voice chunks go only to the talk target, only between start and stop', async () => {
  const br = await browser(server);
  const a = await device(server, 'esp32-talk-a');
  const b = await device(server, 'esp32-talk-b');
  await online(br, 'esp32-talk-b');

  br.send(chunk()); // before talk_start: dropped
  br.sendJson({ type: 'talk_start', target: 'esp32-talk-a' });
  await a.waitFor((m) => m.type === 'talk_start');
  br.send(chunk()); br.send(chunk());
  await sleep(150);
  br.sendJson({ type: 'talk_stop', target: 'esp32-talk-a' });
  await a.waitFor((m) => m.type === 'talk_stop');
  br.send(chunk()); // after talk_stop: dropped
  await sleep(150);

  assert.equal(binaries(a), 2);
  assert.equal(commands(b).length, 0, 'the other device got nothing');
  a.close(); b.close(); br.close();
});

test('a second talker is refused as busy; an offline device as offline', async () => {
  const br1 = await browser(server);
  const br2 = await browser(server);
  const dev = await device(server, 'esp32-talk-c');
  await online(br1, 'esp32-talk-c');

  br1.sendJson({ type: 'talk_start', target: 'esp32-talk-c' });
  await dev.waitFor((m) => m.type === 'talk_start');
  br2.sendJson({ type: 'talk_start', target: 'esp32-talk-c' });
  const busy = await br2.waitFor((m) => m.type === 'talk_denied');
  assert.equal(busy.reason, 'busy');

  br2.sendJson({ type: 'talk_start', target: 'esp32-nobody' });
  const offline = await br2.waitFor((m) => m.type === 'talk_denied' && m.reason === 'offline');
  assert.equal(offline.deviceId, 'esp32-nobody');

  br2.send(chunk()); // br2 isn't talking: its audio must not reach the device
  await sleep(150);
  assert.equal(binaries(dev), 0);
  br1.close(); br2.close(); dev.close();
});

test('closing the tab mid-talk stops talk on the device', async () => {
  const br = await browser(server);
  const dev = await device(server, 'esp32-talk-d');
  await online(br, 'esp32-talk-d');
  br.sendJson({ type: 'talk_start', target: 'esp32-talk-d' });
  await dev.waitFor((m) => m.type === 'talk_start');
  br.close();
  await dev.waitFor((m) => m.type === 'talk_stop');
  dev.close();
});
