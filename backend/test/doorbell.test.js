// Doorbell: ringing from the owner's page and the visitor page, cooldown, log
const { test, before, after } = require('node:test');
const assert = require('node:assert/strict');
const { startServer, device, browser, online, sleep, tempDir } = require('./helpers');

let server;
before(async () => { server = await startServer(); });
after(() => server.stop());

const ringAsVisitor = (id) => fetch(`${server.url}/bell/${id}`, { method: 'POST' });

test('the page button rings the device and every page hears about it', async () => {
  const id = 'esp32-0000000000b1';
  const dev = await device(server, id);
  const owner = await browser(server);
  const other = await browser(server);
  await online(owner, id);
  owner.sendJson({ type: 'doorbell_ring', target: id });
  await dev.waitFor((m) => m.type === 'doorbell');
  const ev = await other.waitFor((m) => m.type === 'event' && m.event.deviceId === id);
  assert.deepEqual([ev.event.type, ev.event.source, ev.event.delivered], ['doorbell', 'page', true]);
  dev.close(); owner.close(); other.close();
});

test('the visitor page rings it; a second ring within 10 s is refused', async () => {
  const id = 'esp32-0000000000b2';
  const dev = await device(server, id);
  const owner = await browser(server);
  await online(owner, id);

  const first = await ringAsVisitor(id);
  assert.equal(first.status, 200);
  await dev.waitFor((m) => m.type === 'doorbell');
  const ev = await owner.waitFor((m) => m.type === 'event' && m.event.deviceId === id);
  assert.equal(ev.event.source, 'visitor');

  const second = await ringAsVisitor(id);
  assert.equal(second.status, 429);
  const body = await second.json();
  assert.equal(body.reason, 'cooldown');
  assert.ok(body.retryIn >= 1 && body.retryIn <= 10);
  await sleep(100);
  assert.equal(dev.messages.filter((m) => m.type === 'doorbell').length, 1);
  dev.close(); owner.close();
});

test('a ring for an offline device is logged as not delivered', async () => {
  const owner = await browser(server);
  const res = await ringAsVisitor('esp32-0000000000b3');
  assert.equal(res.status, 503);
  const ev = await owner.waitFor((m) => m.type === 'event' && m.event.deviceId === 'esp32-0000000000b3');
  assert.equal(ev.event.delivered, false);
  owner.close();
});

test('made-up device IDs are refused', async () => {
  assert.equal((await ringAsVisitor('not-a-device')).status, 404);
  assert.equal((await ringAsVisitor('esp32-../../x')).status, 404);
});

test('the doorbell log survives a restart and is sent to pages that open later', async () => {
  const dir = tempDir();
  const s1 = await startServer(dir);
  await fetch(`${s1.url}/bell/esp32-0000000000b4`, { method: 'POST' });
  await s1.stop();
  const s2 = await startServer(dir);
  const page = await browser(s2);
  const log = await page.waitFor((m) => m.type === 'events');
  assert.equal(log.events[0].deviceId, 'esp32-0000000000b4');
  page.close();
  await s2.stop();
});

test('pages are told the server\'s network address for the visitor link', async () => {
  const page = await browser(server);
  const msg = await page.waitFor((m) => m.type === 'server');
  if (msg.lanUrl !== null) assert.match(msg.lanUrl, /^http:\/\/\d+\.\d+\.\d+\.\d+:\d+$/); // null only with no network
  page.close();
});
