// Alarms: saving and validation, persistence, and syncing each device its own
// list. Alarms ring on the device itself, so the server must never ring them.
const { test, before, after } = require('node:test');
const assert = require('node:assert/strict');
const { startServer, device, browser, online, sleep, tempDir } = require('./helpers');

let server;
before(async () => { server = await startServer(); });
after(() => server.stop());

const latestAlarms = (br) => [...br.messages].reverse().find((m) => m.type === 'alarms').alarms;
const syncs = (dev) => dev.messages.filter((m) => m.type === 'alarms_sync');
const clear = async (br) => { for (const a of latestAlarms(br)) br.sendJson({ type: 'alarm_delete', id: a.id }); await sleep(100); };

test('valid alarms are saved and broadcast; invalid ones are ignored', async () => {
  const br = await browser(server);
  await br.waitFor((m) => m.type === 'alarms');
  br.sendJson({ type: 'alarm_save', alarm: { deviceId: 'esp32-a1', time: 'bad', days: [1], tone: 'x.wav' } });
  br.sendJson({ type: 'alarm_save', alarm: { deviceId: 'esp32-a1', time: '07:00', days: [1, 2, 9], tone: '../../evil.wav' } });
  const msg = await br.waitFor((m) => m.type === 'alarms' && m.alarms.length === 1);
  const a = msg.alarms[0];
  assert.equal(a.time, '07:00');
  assert.deepEqual(a.days, [1, 2], 'day 9 is not a weekday');
  assert.equal(a.tone, 'evil.wav', 'tone path is stripped to a file name');
  assert.equal(a.enabled, true);
  await clear(br);
  br.close();
});

test('alarms survive a server restart', async () => {
  const dir = tempDir();
  const s1 = await startServer(dir);
  const br1 = await browser(s1);
  br1.sendJson({ type: 'alarm_save', alarm: { deviceId: 'esp32-a2', time: '06:30', days: [0], tone: 't.wav' } });
  await br1.waitFor((m) => m.type === 'alarms' && m.alarms.length === 1);
  br1.close();
  await s1.stop();

  const s2 = await startServer(dir);
  const br2 = await browser(s2);
  const msg = await br2.waitFor((m) => m.type === 'alarms');
  assert.equal(msg.alarms.length, 1);
  assert.equal(msg.alarms[0].time, '06:30');
  br2.close();
  await s2.stop();
});

test('a device gets only its own alarms, on connect and after each change', async () => {
  const br = await browser(server);
  await br.waitFor((m) => m.type === 'alarms');
  br.sendJson({ type: 'alarm_save', alarm: { deviceId: 'esp32-a3', time: '07:00', days: [1], tone: 'mine.wav' } });
  br.sendJson({ type: 'alarm_save', alarm: { deviceId: 'esp32-other', time: '08:00', days: [1], tone: 'theirs.wav' } });
  await br.waitFor((m) => m.type === 'alarms' && m.alarms.length === 2);

  const dev = await device(server, 'esp32-a3');
  const first = await dev.waitFor((m) => m.type === 'alarms_sync');
  assert.equal(first.alarms.length, 1);
  assert.deepEqual(Object.keys(first.alarms[0]).sort(), ['days', 'enabled', 'id', 'time', 'tone']);
  assert.equal(first.alarms[0].tone, 'mine.wav');

  // Change it: a new sync with a new version
  br.sendJson({ type: 'alarm_save', alarm: { ...first.alarms[0], deviceId: 'esp32-a3', time: '07:30' } });
  const second = await dev.waitFor((m) => m.type === 'alarms_sync' && m.alarms[0]?.time === '07:30');
  assert.notEqual(second.version, first.version);

  // Delete it: an empty list
  br.sendJson({ type: 'alarm_delete', id: first.alarms[0].id });
  const third = await dev.waitFor((m) => m.type === 'alarms_sync' && m.alarms.length === 0);
  assert.notEqual(third.version, second.version);

  await clear(br);
  dev.close(); br.close();
});

test('the version stays the same when the list has not changed', async () => {
  const br = await browser(server);
  await br.waitFor((m) => m.type === 'alarms');
  br.sendJson({ type: 'alarm_save', alarm: { deviceId: 'esp32-a4', time: '05:00', days: [2], tone: 'x.wav' } });
  await br.waitFor((m) => m.type === 'alarms' && m.alarms.some((a) => a.deviceId === 'esp32-a4'));

  const d1 = await device(server, 'esp32-a4');
  const v1 = (await d1.waitFor((m) => m.type === 'alarms_sync')).version;
  d1.close();
  const d2 = await device(server, 'esp32-a4');
  const v2 = (await d2.waitFor((m) => m.type === 'alarms_sync')).version;
  assert.equal(v2, v1, 'reconnecting device can skip re-saving an identical list');

  await clear(br);
  d2.close(); br.close();
});

test('the server never rings alarms itself (the device does)', async () => {
  const br = await browser(server);
  const dev = await device(server, 'esp32-a5');
  await online(br, 'esp32-a5');
  const now = new Date();
  const hhmm = `${String(now.getHours()).padStart(2, '0')}:${String(now.getMinutes()).padStart(2, '0')}`;
  br.sendJson({ type: 'alarm_save', alarm: { deviceId: 'esp32-a5', time: hhmm, days: [now.getDay()], tone: 'x.wav' } });
  await dev.waitFor((m) => m.type === 'alarms_sync' && m.alarms.length === 1);
  await sleep(2500);
  assert.equal(dev.messages.filter((m) => m.type === 'ring').length, 0);
  await clear(br);
  dev.close(); br.close();
});

test("the device's acknowledgement and alarm_fired reach the page", async () => {
  const br = await browser(server);
  const dev = await device(server, 'esp32-a6');
  await online(br, 'esp32-a6');
  dev.sendJson({ type: 'alarms_ack', version: 'abc12345', count: 2 });
  dev.sendJson({ type: 'alarm_fired', alarmId: 'x', time: '07:00' });
  const ack = await br.waitFor((m) => m.type === 'alarms_ack');
  assert.deepEqual([ack.deviceId, ack.count], ['esp32-a6', 2]);
  const fired = await br.waitFor((m) => m.type === 'alarm_fired');
  assert.equal(fired.deviceId, 'esp32-a6');
  dev.close(); br.close();
});

test('the page is told the same version the device receives', async () => {
  const br = await browser(server);
  const dev = await device(server, 'esp32-a7');
  await online(br, 'esp32-a7');
  br.sendJson({ type: 'alarm_save', alarm: { deviceId: 'esp32-a7', time: '09:15', days: [3], tone: 'x.wav' } });
  const sync = await dev.waitFor((m) => m.type === 'alarms_sync' && m.alarms.length === 1);
  const page = await br.waitFor((m) => m.type === 'alarms' && m.versions?.['esp32-a7'] === sync.version);
  assert.ok(page, 'alarms message carries the version the device will confirm with alarms_ack');
  await clear(br);
  dev.close(); br.close();
});

test('one-time alarms: a date instead of weekdays, switched off once they ring', async () => {
  const br = await browser(server);
  const dev = await device(server, 'esp32-a8');
  await online(br, 'esp32-a8');
  br.sendJson({ type: 'alarm_save', alarm: { deviceId: 'esp32-a8', time: '06:00', days: [1, 2], date: '2026-12-25', tone: 'x.wav' } });
  const sync = await dev.waitFor((m) => m.type === 'alarms_sync' && m.alarms.length === 1);
  assert.deepEqual([sync.alarms[0].date, sync.alarms[0].days, sync.alarms[0].enabled], ['2026-12-25', [], true],
    'a date replaces the weekdays');

  dev.sendJson({ type: 'alarm_fired', alarmId: sync.alarms[0].id, time: '06:00' });
  const off = await dev.waitFor((m) => m.type === 'alarms_sync' && m.alarms[0]?.enabled === false);
  assert.equal(off.alarms[0].date, '2026-12-25');
  await clear(br);
  dev.close(); br.close();
});

test('an alarm with neither weekdays nor a valid date is refused', async () => {
  const br = await browser(server);
  await br.waitFor((m) => m.type === 'alarms');
  br.sendJson({ type: 'alarm_save', alarm: { deviceId: 'esp32-a9', time: '06:00', days: [], tone: 'x.wav' } });
  br.sendJson({ type: 'alarm_save', alarm: { deviceId: 'esp32-a9', time: '06:00', days: [], date: '2026-13-45', tone: 'x.wav' } });
  await sleep(200);
  assert.equal(latestAlarms(br).filter((a) => a.deviceId === 'esp32-a9').length, 0);
  br.close();
});

test('repeating alarms are not switched off when they ring', async () => {
  const br = await browser(server);
  const dev = await device(server, 'esp32-a10');
  await online(br, 'esp32-a10');
  br.sendJson({ type: 'alarm_save', alarm: { deviceId: 'esp32-a10', time: '06:00', days: [3], tone: 'x.wav' } });
  const sync = await dev.waitFor((m) => m.type === 'alarms_sync' && m.alarms.length === 1);
  dev.messages.length = 0;
  dev.sendJson({ type: 'alarm_fired', alarmId: sync.alarms[0].id, time: '06:00' });
  await sleep(200);
  assert.equal(dev.messages.filter((m) => m.type === 'alarms_sync').length, 0);
  await clear(br);
  dev.close(); br.close();
});
