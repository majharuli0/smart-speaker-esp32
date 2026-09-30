// Device time zones: sent on connect in the ESP32's POSIX format, changeable from the page
const { test, before, after } = require('node:test');
const assert = require('node:assert/strict');
const { startServer, device, browser, online, tempDir, sleep } = require('./helpers');

let server;
before(async () => { server = await startServer(tempDir(), { TZ_DEFAULT: 'Asia/Dhaka' }); });
after(() => server.stop());

const timezoneMsg = (dev) => dev.waitFor((m) => m.type === 'timezone');

test('a device gets the default zone on connect, as a POSIX rule', async () => {
  const dev = await device(server, 'esp32-tz01');
  const tz = await timezoneMsg(dev);
  assert.equal(tz.name, 'Asia/Dhaka');
  assert.equal(tz.tz, '<+06>-6', 'POSIX counts hours west of UTC, so +6 becomes -6');
  dev.close();
});

test('POSIX conversion: half-hour, negative and zero offsets', async () => {
  const br = await browser(server);
  const dev = await device(server, 'esp32-tz02');
  await online(br, 'esp32-tz02');
  await timezoneMsg(dev);

  const cases = [
    ['Asia/Kolkata', '<+0530>-5:30'],
    ['America/Argentina/Buenos_Aires', '<-03>3'], // no daylight saving, so always -3
    ['UTC', 'UTC0'],
  ];
  for (const [name, posix] of cases) {
    dev.messages.length = 0;
    br.sendJson({ type: 'set_timezone', target: 'esp32-tz02', tz: name });
    const got = await timezoneMsg(dev);
    assert.deepEqual([got.name, got.tz], [name, posix]);
  }
  dev.close(); br.close();
});

test('an unknown zone name is ignored', async () => {
  const br = await browser(server);
  const dev = await device(server, 'esp32-tz03');
  await online(br, 'esp32-tz03');
  await timezoneMsg(dev);
  dev.messages.length = 0;
  br.sendJson({ type: 'set_timezone', target: 'esp32-tz03', tz: 'Mars/Olympus_Mons' });
  await sleep(200);
  assert.equal(dev.messages.filter((m) => m.type === 'timezone').length, 0);
  dev.close(); br.close();
});

test('a chosen zone is saved and used again after a server restart', async () => {
  const dir = tempDir();
  const s1 = await startServer(dir, { TZ_DEFAULT: 'UTC' });
  const br = await browser(s1);
  br.sendJson({ type: 'set_timezone', target: 'esp32-tz04', tz: 'Asia/Tokyo' });
  await sleep(200); // device is offline: the setting must still be stored
  br.close();
  await s1.stop();

  const s2 = await startServer(dir, { TZ_DEFAULT: 'UTC' });
  const dev = await device(s2, 'esp32-tz04');
  const tz = await timezoneMsg(dev);
  assert.deepEqual([tz.name, tz.tz], ['Asia/Tokyo', '<+09>-9']);
  dev.close();
  await s2.stop();
});
