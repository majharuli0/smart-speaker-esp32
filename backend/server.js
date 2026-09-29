const express = require('express');
const { WebSocketServer } = require('ws');
const dgram = require('dgram');
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const PORT = 3000;
const MDNS_HOST = 'led-server.local';
const TONES_DIR = path.join(__dirname, 'tones');
const ALARMS_FILE = path.join(__dirname, 'alarms.json');
fs.mkdirSync(TONES_DIR, { recursive: true });

// [{ id, deviceId, time: "07:00", days: [0..6, Sun=0], tone, tz, enabled }]
let alarms = [];
try { alarms = JSON.parse(fs.readFileSync(ALARMS_FILE, 'utf8')); } catch {}
const saveAlarms = () => fs.writeFileSync(ALARMS_FILE, JSON.stringify(alarms, null, 2));
const listTones = () => fs.readdirSync(TONES_DIR).filter((f) => f.endsWith('.wav'));

const app = express();
// Allow the page to be opened from a file or Live Server, not only from this server
app.use((req, res, next) => {
  res.set({ 'Access-Control-Allow-Origin': '*', 'Access-Control-Allow-Headers': 'X-Filename' });
  req.method === 'OPTIONS' ? res.end() : next();
});
app.use(express.static(path.join(__dirname, '../frontend')));
app.use('/tones', express.static(TONES_DIR));

// Body is a 16 kHz 16-bit mono WAV, already converted by the browser
// (type: () => true — browsers send an ArrayBuffer body with no Content-Type)
app.post('/tones', express.raw({ type: () => true, limit: '20mb' }), (req, res) => {
  const name = path.basename(String(req.get('X-Filename') || '')).replace(/[^\w.-]/g, '_');
  if (!name.endsWith('.wav') || !req.body?.length) return res.status(400).end();
  fs.writeFileSync(path.join(TONES_DIR, name), req.body);
  toBrowsers({ type: 'tones', tones: listTones() });
  res.end();
});

const server = app.listen(PORT, () => console.log(`Server on http://localhost:${PORT}`));

// Answer mDNS lookups for led-server.local with this machine's LAN IP, so
// the ESP32 finds the server without a hardcoded address. The UDP "connect"
// just asks the OS which interface reaches the LAN (sends nothing); pinning
// mDNS to it keeps replies off WSL/Hyper-V virtual adapters.
const probe = dgram.createSocket('udp4');
probe.connect(80, '8.8.8.8', () => {
  const ip = probe.address().address;
  probe.close();
  const mdns = require('multicast-dns')({ interface: ip });
  mdns.on('query', (query) => {
    if (query.questions.some((q) => q.name === MDNS_HOST && (q.type === 'A' || q.type === 'ANY'))) {
      mdns.respond({ answers: [{ name: MDNS_HOST, type: 'A', ttl: 120, data: ip }] });
    }
  });
  console.log(`Advertising ${MDNS_HOST} -> ${ip}`);
});

const wss = new WebSocketServer({ server });
const devices = new Map(); // deviceId -> ws

function toBrowsers(msg) {
  const data = JSON.stringify(msg);
  wss.clients.forEach((c) => {
    if (c.role === 'browser' && c.readyState === 1) c.send(data);
  });
}

function toDevice(deviceId, msg) {
  const ws = devices.get(deviceId);
  if (ws?.readyState !== 1) return false;
  ws.send(JSON.stringify(msg));
  return true;
}

const deviceList = () => ({ type: 'devices', devices: [...devices.keys()] });

// Current date, HH:MM and weekday in the alarm's own timezone, so a server
// running on UTC (e.g. in the cloud) still rings at the user's local time.
function nowIn(tz) {
  const p = Object.fromEntries(
    new Intl.DateTimeFormat('en-US', {
      timeZone: tz, hourCycle: 'h23', year: 'numeric', month: '2-digit', day: '2-digit',
      hour: '2-digit', minute: '2-digit', weekday: 'short',
    }).formatToParts(new Date()).map((x) => [x.type, x.value])
  );
  return {
    time: `${p.hour}:${p.minute}`,
    weekday: ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'].indexOf(p.weekday),
    minuteKey: `${p.year}-${p.month}-${p.day} ${p.hour}:${p.minute}`,
  };
}

const lastFired = {}; // alarmId -> minuteKey, so each alarm fires once per matching minute
setInterval(() => {
  for (const a of alarms) {
    if (!a.enabled) continue;
    const now = nowIn(a.tz);
    if (now.time !== a.time || !a.days.includes(now.weekday) || lastFired[a.id] === now.minuteKey) continue;
    lastFired[a.id] = now.minuteKey;
    const delivered = toDevice(a.deviceId, { type: 'ring', tone: a.tone });
    console.log(`Alarm ${a.time} -> ${a.deviceId}: ${delivered ? 'ringing' : 'device offline'}`);
    toBrowsers({ type: 'alarm_fired', alarmId: a.id, deviceId: a.deviceId, time: a.time, delivered });
  }
}, 1000);

function saveAlarm(a) {
  if (!/^\d\d:\d\d$/.test(a.time) || !Array.isArray(a.days) || !a.deviceId || !a.tone) return;
  let tz = a.tz;
  try { nowIn(tz); } catch { tz = 'UTC'; } // unknown timezone name
  const alarm = {
    id: a.id || crypto.randomUUID(), deviceId: a.deviceId, time: a.time,
    days: a.days.map(Number), tone: path.basename(a.tone), tz, enabled: a.enabled !== false,
  };
  alarms = alarms.filter((x) => x.id !== alarm.id).concat(alarm);
  saveAlarms();
  toBrowsers({ type: 'alarms', alarms });
}

wss.on('connection', (ws) => {
  ws.on('message', (raw, isBinary) => {
    // Voice chunks (16 kHz 16-bit mono PCM) from a talking browser go straight
    // to its device. Dropped if the device falls behind, so delay can't pile up.
    if (isBinary) {
      const dev = devices.get(ws.talkTarget);
      if (ws.role === 'browser' && dev?.readyState === 1 && dev.bufferedAmount < 16000) dev.send(raw, { binary: true });
      return;
    }

    let msg;
    try { msg = JSON.parse(raw); } catch { return; }

    if (msg.type === 'hello') {
      ws.role = msg.role;
      if (ws.role === 'device') {
        ws.deviceId = msg.deviceId;
        devices.set(ws.deviceId, ws);
        console.log(`Device online: ${ws.deviceId}`);
        toBrowsers(deviceList());
      } else {
        ws.send(JSON.stringify(deviceList()));
        ws.send(JSON.stringify({ type: 'alarms', alarms }));
        ws.send(JSON.stringify({ type: 'tones', tones: listTones() }));
      }
      return;
    }

    if (ws.role === 'browser' && msg.type === 'alarm_save') return saveAlarm(msg.alarm || {});
    if (ws.role === 'browser' && msg.type === 'alarm_delete') {
      alarms = alarms.filter((a) => a.id !== msg.id);
      saveAlarms();
      return toBrowsers({ type: 'alarms', alarms });
    }

    // One talker per device; binary chunks follow until talk_stop
    if (ws.role === 'browser' && msg.type === 'talk_start') {
      const busy = [...wss.clients].some((c) => c !== ws && c.talkTarget === msg.target);
      if (busy || !toDevice(msg.target, msg)) {
        return ws.send(JSON.stringify({ type: 'talk_denied', deviceId: msg.target, reason: busy ? 'busy' : 'offline' }));
      }
      ws.talkTarget = msg.target;
      return;
    }
    if (ws.role === 'browser' && msg.type === 'talk_stop') ws.talkTarget = null;

    // Browser -> device: relay any other command to msg.target.
    // Device -> browsers: relay anything, tagged with which device sent it.
    if (ws.role === 'browser') toDevice(msg.target, msg);
    else if (ws.role === 'device') toBrowsers({ ...msg, deviceId: ws.deviceId });
  });

  ws.on('close', () => {
    if (ws.talkTarget) toDevice(ws.talkTarget, { type: 'talk_stop' }); // tab closed mid-talk
    // Only if this socket is still the registered one (a reconnect may have replaced it)
    if (ws.role === 'device' && devices.get(ws.deviceId) === ws) {
      devices.delete(ws.deviceId);
      console.log(`Device offline: ${ws.deviceId}`);
      toBrowsers(deviceList());
    }
  });
});
