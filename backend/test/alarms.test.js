// Alarm saving, validation, persistence and the 1 s scheduler
const { test, before, after } = require('node:test');
const assert = require('node:assert/strict');
const { startServer, device, browser, online, sleep, tempDir } = require('./helpers');

const DAYS = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];
const TZ = 'Asia/Tokyo'; // deliberately not the machine's timezone

// Current HH:MM and weekday in a timezone, the same way the server computes them
function nowIn(tz) {
  const p = Object.fromEntries(new Intl.DateTimeFormat('en-US', {
    timeZone: tz, hourCycle: 'h23', hour: '2-digit', minute: '2-digit', weekday: 'short',
  }).formatToParts(new Date()).map((x) => [x.type, x.value]));
  return { time: `${p.hour}:${p.minute}`, weekday: DAYS.indexOf(p.weekday) };
}

// Don't start a timed test in the last seconds of a minute: the alarm minute could pass
async function awayFromMinuteEdge() {
  const s = new Date().getSeconds();
  if (s >= 55) await sleep((61 - s) * 1000);
}

let server;
before(async () => { server = await startServer(); });
after(() => server.stop());

const latestAlarms = (br) => [...br.messages].reverse().find((m) => m.type === 'alarms').alarms;

test('valid alarms are saved and broadcast; invalid ones are ignored', async () => {
  const br = await browser(server);
  await br.waitFor((m) => m.type === 'alarms');
  br.sendJson({ type: 'alarm_save', alarm: { deviceId: 'esp32-a1', time: 'bad', days: [1], tone: 'x.wav', tz: TZ } });
  br.sendJson({ type: 'alarm_save', alarm: { deviceId: 'esp32-a1', time: '07:00', days: [1, 2], tone: '../../evil.wav', tz: 'Not/AZone' } });
  const msg = await br.waitFor((m) => m.type === 'alarms' && m.alarms.length === 1);
  const a = msg.alarms[0];
  assert.equal(a.time, '07:00');
  assert.equal(a.tone, 'evil.wav', 'tone path is stripped to a file name');
  assert.equal(a.tz, 'UTC', 'unknown timezone falls back to UTC');
  assert.equal(a.enabled, true);
  br.sendJson({ type: 'alarm_delete', id: a.id });
  await br.waitFor((m) => m.type === 'alarms' && m.alarms.length === 0);
  br.close();
});

test('alarms survive a server restart', async () => {
  const dir = tempDir();
  const s1 = await startServer(dir);
  const br1 = await browser(s1);
  br1.sendJson({ type: 'alarm_save', alarm: { deviceId: 'esp32-a2', time: '06:30', days: [0], tone: 't.wav', tz: TZ } });
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

test('scheduler rings the right alarm once, in the alarm timezone', async () => {
  await awayFromMinuteEdge();
  const br = await browser(server);
  const dev = await device(server, 'esp32-a3');
  await online(br, 'esp32-a3');

  const now = nowIn(TZ);
  const other = (now.weekday + 1) % 7;
  const base = { deviceId: 'esp32-a3', time: now.time, tz: TZ };
  br.sendJson({ type: 'alarm_save', alarm: { ...base, days: [now.weekday], tone: 'ring.wav' } });
  br.sendJson({ type: 'alarm_save', alarm: { ...base, days: [other], tone: 'wrong-day.wav' } });
  br.sendJson({ type: 'alarm_save', alarm: { ...base, days: [now.weekday], tone: 'disabled.wav', enabled: false } });

  const ring = await dev.waitFor((m) => m.type === 'ring', 3000);
  assert.equal(ring.tone, 'ring.wav');
  const fired = await br.waitFor((m) => m.type === 'alarm_fired');
  assert.equal(fired.delivered, true);

  await sleep(2500); // several more scheduler ticks within the same minute
  const rings = dev.messages.filter((m) => m.type === 'ring');
  assert.deepEqual(rings.map((r) => r.tone), ['ring.wav'], 'fires once; wrong-day and disabled never');

  for (const a of latestAlarms(br)) br.sendJson({ type: 'alarm_delete', id: a.id });
  dev.close(); br.close();
});

test('an alarm for an offline device is reported as not delivered', async () => {
  await awayFromMinuteEdge();
  const br = await browser(server);
  const now = nowIn(TZ);
  br.sendJson({ type: 'alarm_save', alarm: { deviceId: 'esp32-offline', time: now.time, days: [now.weekday], tone: 'x.wav', tz: TZ } });
  const fired = await br.waitFor((m) => m.type === 'alarm_fired' && m.deviceId === 'esp32-offline', 3000);
  assert.equal(fired.delivered, false);
  for (const a of latestAlarms(br)) br.sendJson({ type: 'alarm_delete', id: a.id });
  br.close();
});
