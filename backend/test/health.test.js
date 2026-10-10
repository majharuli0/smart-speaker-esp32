// Device restart reports (reason, crash details) are logged and shown to pages
const { test, before, after } = require('node:test');
const assert = require('node:assert/strict');
const { startServer, device, browser, online, tempDir } = require('./helpers');

let server;
before(async () => { server = await startServer(); });
after(() => server.stop());

test('a restart report is logged and announced to pages', async () => {
  const page = await browser(server);
  const dev = await device(server, 'esp32-0000000000c1');
  await online(page, 'esp32-0000000000c1');
  dev.sendJson({ type: 'boot', reason: 'power_on', fw: '0.10.0' });
  const ev = await page.waitFor((m) => m.type === 'event' && m.event.type === 'boot');
  assert.deepEqual([ev.event.deviceId, ev.event.reason, ev.event.fw], ['esp32-0000000000c1', 'power_on', '0.10.0']);
  assert.equal(ev.event.crash, undefined);
  dev.close(); page.close();
});

test('crash details are kept and survive a server restart', async () => {
  const dir = tempDir();
  const s1 = await startServer(dir);
  const dev = await device(s1, 'esp32-0000000000c2');
  const page = await browser(s1);
  await online(page, 'esp32-0000000000c2');
  const crash = { task: 'loopTask', pc: '0x42001234', backtrace: ['0x42001234', '0x42005678'] };
  dev.sendJson({ type: 'boot', reason: 'crash', fw: '0.10.0', crash });
  await page.waitFor((m) => m.type === 'event' && m.event.reason === 'crash');
  dev.close(); page.close();
  await s1.stop();

  const s2 = await startServer(dir);
  const later = await browser(s2);
  const log = await later.waitFor((m) => m.type === 'events');
  const boot = log.events.find((e) => e.type === 'boot');
  assert.deepEqual([boot.reason, boot.crash], ['crash', crash]);
  later.close();
  await s2.stop();
});
