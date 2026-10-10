// Devices over MQTT: a real (in-process) broker, the server bridging it, and
// fake devices that behave like the firmware (topics, retained online flag,
// last will). No internet, and none of the real EMQX quota.
const { test, before, after } = require('node:test');
const assert = require('node:assert/strict');
const net = require('node:net');
const mqtt = require('mqtt');
const { createBroker } = require('aedes');
const { startServer, browser, online, sleep } = require('./helpers');

let broker, brokerServer, brokerUrl, server;
before(async () => {
  broker = await createBroker();
  brokerServer = net.createServer(broker.handle);
  await new Promise((r) => brokerServer.listen(0, r));
  brokerUrl = `mqtt://localhost:${brokerServer.address().port}`;
  server = await startServer(undefined, { MQTT_URL: brokerUrl });
  await sleep(300); // let the server connect and subscribe
});
after(async () => {
  await server.stop();
  await new Promise((r) => broker.close(r));
  brokerServer.close();
});

// A fake device on MQTT, doing what the firmware does on connect
function mqttDevice(id, { hello = true } = {}) {
  return new Promise((resolve) => {
    const c = mqtt.connect(brokerUrl, {
      clientId: id,
      will: { topic: `ss/dev/${id}/online`, payload: '0', retain: true, qos: 1 },
      reconnectPeriod: 0,
    });
    c.messages = [];
    c.on('message', (topic, payload) => {
      if (topic.endsWith('/audio')) c.messages.push({ binary: true, length: payload.length });
      else if (topic.endsWith('/cmd')) c.messages.push(JSON.parse(payload));
      else c.messages.push({ topic, text: payload.toString() });
    });
    c.waitFor = (match, timeout = 2000) => new Promise((res, rej) => {
      const t0 = Date.now();
      const tick = () => {
        const m = c.messages.find(match);
        if (m) return res(m);
        if (Date.now() - t0 > timeout) return rej(new Error('timed out; got ' + JSON.stringify(c.messages)));
        setTimeout(tick, 20);
      };
      tick();
    });
    c.evt = (msg) => c.publish(`ss/dev/${id}/evt`, JSON.stringify(msg), { qos: 1 });
    c.on('connect', () => {
      c.subscribe([`ss/dev/${id}/cmd`, `ss/dev/${id}/audio`, 'ss/server/#'], { qos: 1 }, () => {
        c.publish(`ss/dev/${id}/online`, '1', { retain: true, qos: 1 });
        if (hello) c.evt({ type: 'hello', role: 'device', deviceId: id, fw: 'test', board: 'esp32s3' });
        resolve(c);
      });
    });
  });
}

test('an MQTT device shows up on the page and gets its time zone and alarms', async () => {
  const id = 'esp32-00000000d001';
  const page = await browser(server);
  const dev = await mqttDevice(id);
  await online(page, id);
  await dev.waitFor((m) => m.type === 'timezone');
  await dev.waitFor((m) => m.type === 'alarms_sync');
  dev.end(true);
  page.close();
});

test('page commands reach the device on its cmd topic; its replies reach the page', async () => {
  const id = 'esp32-00000000d002';
  const page = await browser(server);
  const dev = await mqttDevice(id);
  await online(page, id);
  page.sendJson({ type: 'volume', target: id, value: 42 });
  await dev.waitFor((m) => m.type === 'volume' && m.value === 42);
  dev.evt({ type: 'volume', value: 42 });
  const reply = await page.waitFor((m) => m.type === 'volume' && m.deviceId === id);
  assert.equal(reply.value, 42);
  dev.end(true);
  page.close();
});

test('talk audio goes to the device on its audio topic', async () => {
  const id = 'esp32-00000000d003';
  const page = await browser(server);
  const dev = await mqttDevice(id);
  await online(page, id);
  page.sendJson({ type: 'talk_start', target: id });
  await dev.waitFor((m) => m.type === 'talk_start');
  page.send(Buffer.alloc(640));
  const audio = await dev.waitFor((m) => m.binary);
  assert.equal(audio.length, 640);
  dev.end(true);
  page.close();
});

test('when a device drops off, its last will marks it offline', async () => {
  const id = 'esp32-00000000d004';
  const page = await browser(server);
  const dev = await mqttDevice(id);
  await online(page, id);
  dev.stream.destroy(); // vanish without a goodbye, like a power cut: the broker sends the will
  await page.waitFor((m) => m.type === 'devices' && !m.devices.includes(id) && page.messages.indexOf(m) > 0, 4000);
  page.close();
});

test('the device ID comes from the topic, not the message', async () => {
  const id = 'esp32-00000000d005';
  const page = await browser(server);
  const dev = await mqttDevice(id, { hello: false });
  dev.evt({ type: 'hello', role: 'device', deviceId: 'esp32-00000000ffff', fw: 'test' }); // tries to pose as another
  await online(page, id);
  const list = [...page.messages].reverse().find((m) => m.type === 'devices');
  assert.ok(!list.devices.includes('esp32-00000000ffff'));
  dev.end(true);
  page.close();
});

test('the server announces itself and its download address (retained)', async () => {
  const dev = await mqttDevice('esp32-00000000d006', { hello: false });
  const up = await dev.waitFor((m) => m.topic === 'ss/server/online');
  assert.equal(up.text, '1');
  const http = await dev.waitFor((m) => m.topic === 'ss/server/http');
  assert.match(http.text, /^http:\/\/[\d.]+:\d+$/);
  dev.end(true);
});
