const express = require('express');
const { WebSocketServer } = require('ws');
const dgram = require('dgram');
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

// Settings come from the environment (or backend/.env, see .env.example)
const PORT = Number(process.env.PORT ?? 3000); // ?? not ||: PORT=0 (tests) must stay 0
const MDNS_HOST = process.env.MDNS_HOST || 'led-server.local';
const MDNS_ENABLED = process.env.MDNS !== 'off'; // tests turn it off
const DATA_DIR = path.resolve(__dirname, process.env.DATA_DIR || '.');
const TONES_DIR = path.join(DATA_DIR, 'tones');
const ALARMS_FILE = path.join(DATA_DIR, 'alarms.json');
const DEVICES_FILE = path.join(DATA_DIR, 'devices.json');
const FIRMWARE_DIR = path.join(DATA_DIR, 'firmware'); // newest firmware per board: <board>.bin + <board>.json
const EVENTS_FILE = path.join(DATA_DIR, 'events.json'); // doorbell rings, newest first
// Time zone for devices that haven't been given one: this machine's (right for a local server)
const DEFAULT_TZ = process.env.TZ_DEFAULT || Intl.DateTimeFormat().resolvedOptions().timeZone;
fs.mkdirSync(TONES_DIR, { recursive: true });
fs.mkdirSync(FIRMWARE_DIR, { recursive: true });

// ---- Doorbell ----
// Rung from the owner's page or the visitor page (web/bell.html). The device
// plays a built-in ding-dong; every ring is logged and shown on the pages.
const MAX_EVENTS = 100;
let events = [];
try { events = JSON.parse(fs.readFileSync(EVENTS_FILE, 'utf8')); } catch {}
const saveEvents = () => fs.writeFileSync(EVENTS_FILE, JSON.stringify(events, null, 2));

// Doorbell rings and device restarts: saved newest-first and announced to pages
function logEvent(event) {
  events = [event, ...events].slice(0, MAX_EVENTS);
  saveEvents();
  toBrowsers({ type: 'event', event });
}

function ringDoorbell(deviceId, source) {
  const now = Date.now();
  const delivered = toDevice(deviceId, { type: 'doorbell' });
  logEvent({ type: 'doorbell', deviceId, source, at: new Date(now).toISOString(), delivered });
  console.log(`Doorbell ${deviceId} (${source}): ${delivered ? 'rang' : 'device offline'}`);
  return delivered ? { ok: true } : { ok: false, reason: 'offline' };
}

// [{ id, deviceId, time: "07:00" (device's local time), days: [0..6, Sun=0], tone, enabled }]
let alarms = [];
try { alarms = JSON.parse(fs.readFileSync(ALARMS_FILE, 'utf8')); } catch {}
const saveAlarms = () => fs.writeFileSync(ALARMS_FILE, JSON.stringify(alarms, null, 2));
const listTones = () => fs.readdirSync(TONES_DIR).filter((f) => f.endsWith('.wav'));

// Per-device settings: { [deviceId]: { tz: "Asia/Dhaka" } }
let deviceSettings = {};
try { deviceSettings = JSON.parse(fs.readFileSync(DEVICES_FILE, 'utf8')); } catch {}
const saveDeviceSettings = () => fs.writeFileSync(DEVICES_FILE, JSON.stringify(deviceSettings, null, 2));

const isValidTz = (tz) => {
  try { new Intl.DateTimeFormat('en-US', { timeZone: tz }); return true; } catch { return false; }
};

// IANA name → the POSIX rule the ESP32 understands, using the zone's offset
// right now: Asia/Dhaka → "<+06>-6", Asia/Kolkata → "<+0530>-5:30", UTC → "UTC0".
// POSIX counts hours WEST of UTC, so the sign flips. Daylight-saving rules
// aren't encoded; the server re-sends every hour, which covers the switch.
function posixTz(tz) {
  const offset = new Intl.DateTimeFormat('en-US', { timeZone: tz, timeZoneName: 'longOffset' })
    .formatToParts(new Date()).find((p) => p.type === 'timeZoneName').value; // "GMT+06:00", or "GMT"
  const m = offset.match(/GMT([+-])(\d\d):(\d\d)/);
  if (!m || (m[2] === '00' && m[3] === '00')) return 'UTC0'; // UTC shows as "GMT+00:00"
  const [, sign, hh, mm] = m;
  const minutes = mm === '00' ? '' : mm;
  return `<${sign}${hh}${minutes}>${sign === '+' ? '-' : ''}${Number(hh)}${minutes ? ':' + minutes : ''}`;
}

const app = express();
// Allow the page to be opened from a file or Live Server, not only from this server
app.use((req, res, next) => {
  res.set({ 'Access-Control-Allow-Origin': '*', 'Access-Control-Allow-Headers': 'X-Filename' });
  req.method === 'OPTIONS' ? res.end() : next();
});
app.use(express.static(path.join(__dirname, '../web')));
app.use('/tones', express.static(TONES_DIR));

// Body is a 16 kHz 16-bit mono WAV, already converted by the browser
// (type: () => true — browsers send an ArrayBuffer body with no Content-Type)
app.post('/tones', express.raw({ type: () => true, limit: '20mb' }), (req, res) => {
  const name = path.basename(String(req.get('X-Filename') || '')).replace(/[^\w.-]/g, '_');
  if (!name.endsWith('.wav') || !req.body?.length) return res.status(400).end();
  fs.writeFileSync(path.join(TONES_DIR, name), req.body);
  toBrowsers({ type: 'tones', tones: listTones() });
  const users = new Set(alarms.filter((a) => a.tone === name).map((a) => a.deviceId));
  if (users.size) {
    users.forEach(syncAlarms);
    toBrowsers(alarmsMessage());
  }
  res.end();
});

// ---- Firmware updates ----
// The uploaded .bin carries a marker "SSFW:<board>:<version>:END" (see
// firmware/ota.cpp), so the server knows which devices it fits and refuses
// files that aren't this firmware. Only the newest file per board is kept.
const MAX_FIRMWARE = 3 * 1024 * 1024; // the S3's program slot is 3 MB (the 16 MB merged.bin is not an update file)

function readFirmwareMarker(buf) {
  if (buf[0] !== 0xe9) return null; // every ESP32 program image starts with this byte
  const m = buf.toString('latin1').match(/SSFW:([a-z0-9]+):([0-9A-Za-z.-]+):END/);
  return m && { board: m[1], version: m[2] };
}

function firmwareList() {
  return Object.fromEntries(fs.readdirSync(FIRMWARE_DIR).filter((f) => f.endsWith('.json')).map((f) => {
    const meta = JSON.parse(fs.readFileSync(path.join(FIRMWARE_DIR, f), 'utf8'));
    return [meta.board, meta];
  }));
}

app.post('/firmware', express.raw({ type: () => true, limit: MAX_FIRMWARE }), (req, res) => {
  const buf = req.body;
  const found = Buffer.isBuffer(buf) && buf.length ? readFirmwareMarker(buf) : null;
  if (!found) return res.status(400).json({ error: 'not a firmware file for this project (use firmware/build/<board>/firmware.ino.bin)' });
  const meta = { ...found, size: buf.length, sha256: crypto.createHash('sha256').update(buf).digest('hex'), uploaded: new Date().toISOString() };
  fs.writeFileSync(path.join(FIRMWARE_DIR, `${meta.board}.bin`), buf);
  fs.writeFileSync(path.join(FIRMWARE_DIR, `${meta.board}.json`), JSON.stringify(meta, null, 2));
  console.log(`Firmware ${meta.version} for ${meta.board} uploaded (${meta.size} bytes)`);
  toBrowsers({ type: 'firmware', firmware: firmwareList() });
  res.json(meta);
});
app.use('/firmware', express.static(FIRMWARE_DIR, { extensions: false }));

// Visitor page's Ring button. Only real device IDs.
app.post('/bell/:deviceId', (req, res) => {
  const id = req.params.deviceId;
  if (!/^esp32-[0-9a-f]{12}$/.test(id)) return res.status(404).json({ ok: false, reason: 'unknown' });
  const result = ringDoorbell(id, 'visitor');
  res.status(result.ok ? 200 : 503).json(result);
});

// PORT=0 picks a free port (tests use this); log the real one
const server = app.listen(PORT, (err) => {
  if (err) throw err; // e.g. port already in use
  console.log(`Server on http://localhost:${server.address().port}`);
});

// Answer mDNS lookups for led-server.local with this machine's LAN IP, so
// the ESP32 finds the server without a hardcoded address. The UDP "connect"
// just asks the OS which interface reaches the LAN (sends nothing); pinning
// mDNS to it keeps replies off WSL/Hyper-V virtual adapters.
// This machine's LAN address: for mDNS, and for links other devices open (the
// visitor doorbell page), since "localhost" only works on this machine.
let lanIp = null;
function advertiseMdns(ip) {
  const mdns = require('multicast-dns')({ interface: ip });
  mdns.on('query', (query) => {
    if (query.questions.some((q) => q.name === MDNS_HOST && (q.type === 'A' || q.type === 'ANY'))) {
      mdns.respond({ answers: [{ name: MDNS_HOST, type: 'A', ttl: 120, data: ip }] });
    }
  });
  console.log(`Advertising ${MDNS_HOST} -> ${ip}`);
}
const probe = dgram.createSocket('udp4');
probe.on('error', () => {}); // no network: links fall back to the page's own address
probe.connect(80, '8.8.8.8', () => {
  lanIp = probe.address().address;
  probe.close();
  if (MDNS_ENABLED) advertiseMdns(lanIp);
  publishHttpBase();
});
// PUBLIC_URL overrides it once the server runs somewhere else (e.g. the cloud)
const lanUrl = () => process.env.PUBLIC_URL || (lanIp ? `http://${lanIp}:${server.address().port}` : null);

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

// A device's latest status reports, kept so a page opened later still gets them
const REPLAYED_REPORTS = ['alarms_ack', 'cache', 'volume', 'partitions', 'stats'];

function sendTimezone(deviceId) {
  const tz = deviceSettings[deviceId]?.tz || DEFAULT_TZ;
  toDevice(deviceId, { type: 'timezone', tz: posixTz(tz), name: tz });
}
// Hourly re-send keeps devices right across daylight-saving switches
setInterval(() => devices.forEach((_, id) => sendTimezone(id)), 60 * 60 * 1000);

// Size + SHA-256 of a tone file, so a device can store it and check the
// download. Cached until the file changes (re-upload under the same name).
const toneInfoCache = new Map();
function toneInfo(name) {
  let stat;
  try { stat = fs.statSync(path.join(TONES_DIR, name)); } catch { return null; }
  const cached = toneInfoCache.get(name);
  if (cached && cached.mtimeMs === stat.mtimeMs && cached.size === stat.size) return cached.info;
  const sha256 = crypto.createHash('sha256').update(fs.readFileSync(path.join(TONES_DIR, name))).digest('hex');
  const info = { name, size: stat.size, sha256 };
  toneInfoCache.set(name, { mtimeMs: stat.mtimeMs, size: stat.size, info });
  return info;
}

// Alarms ring on the device, from its own clock and time zone (so they work
// with the network down). The server only keeps the list and sends each
// device its own alarms, plus the tones they use so the device can store
// them: on connect and after every change. The version is a hash of all of
// it, so an unchanged list is recognised and not re-saved.
function alarmsFor(deviceId) {
  const list = alarms
    .filter((a) => a.deviceId === deviceId)
    .map(({ id, time, days, date, tone, enabled }) => ({ id, time, days, ...(date && { date }), tone, enabled }));
  const tones = [...new Set(list.map((a) => a.tone))].map(toneInfo).filter(Boolean);
  const version = crypto.createHash('sha1').update(JSON.stringify({ list, tones })).digest('hex').slice(0, 8);
  return { list, tones, version };
}

function syncAlarms(deviceId) {
  const { list, tones, version } = alarmsFor(deviceId);
  toDevice(deviceId, { type: 'alarms_sync', version, alarms: list, tones });
}

// All alarms for the page, plus the version each device should confirm with alarms_ack
function alarmsMessage() {
  const ids = new Set([...devices.keys(), ...alarms.map((a) => a.deviceId)]);
  const versions = Object.fromEntries([...ids].map((id) => [id, alarmsFor(id).version]));
  return { type: 'alarms', alarms, versions };
}

// A one-time alarm has a date ("2026-10-12") instead of weekdays
const isDate = (d) => typeof d === 'string' && /^\d{4}-\d\d-\d\d$/.test(d) && !Number.isNaN(Date.parse(d));

function saveAlarm(a) {
  if (!/^\d\d:\d\d$/.test(a.time) || !a.deviceId || !a.tone) return;
  const date = isDate(a.date) ? a.date : undefined;
  const days = date ? [] : (Array.isArray(a.days) ? a.days : []).map(Number).filter((d) => d >= 0 && d <= 6);
  if (!date && !days.length) return; // rings never
  const alarm = {
    id: a.id || crypto.randomUUID(), deviceId: a.deviceId, time: a.time,
    days, ...(date && { date }), tone: path.basename(a.tone), enabled: a.enabled !== false,
  };
  alarms = alarms.filter((x) => x.id !== alarm.id).concat(alarm);
  saveAlarms();
  toBrowsers(alarmsMessage());
  syncAlarms(alarm.deviceId);
}

function deleteAlarm(id) {
  const gone = alarms.find((a) => a.id === id);
  if (!gone) return;
  alarms = alarms.filter((a) => a.id !== id);
  saveAlarms();
  toBrowsers(alarmsMessage());
  syncAlarms(gone.deviceId);
}

// Messages from a browser, a WebSocket device, or an MQTT device (see the
// MQTT section: its "link" behaves like a device WebSocket)
function onMessage(ws, raw, isBinary) {
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
    ws.role = ws.viaMqtt ? 'device' : msg.role;
    if (ws.role === 'device') {
      ws.deviceId = ws.viaMqtt ? ws.deviceId : msg.deviceId; // MQTT: the ID comes from the topic, not the message
      ws.board = msg.board; // which firmware file fits it
      devices.set(ws.deviceId, ws);
      console.log(`Device online: ${ws.deviceId}`);
      toBrowsers(deviceList());
      sendTimezone(ws.deviceId);
      syncAlarms(ws.deviceId);
    } else {
      ws.send(JSON.stringify(deviceList()));
      ws.send(JSON.stringify(alarmsMessage()));
      ws.send(JSON.stringify({ type: 'tones', tones: listTones() }));
      ws.send(JSON.stringify({ type: 'firmware', firmware: firmwareList() }));
      ws.send(JSON.stringify({ type: 'events', events: events.slice(0, 50) }));
      ws.send(JSON.stringify({ type: 'server', lanUrl: lanUrl() }));
      // Devices send these once (on connect or on change), so replay them for a page opened later
      devices.forEach((dev) => Object.values(dev.lastReports || {}).forEach((m) => ws.send(JSON.stringify(m))));
    }
    return;
  }

  if (ws.role === 'browser' && msg.type === 'alarm_save') return saveAlarm(msg.alarm || {});
  if (ws.role === 'browser' && msg.type === 'alarm_delete') return deleteAlarm(msg.id);

  // Doorbell button on the owner's page
  if (ws.role === 'browser' && msg.type === 'doorbell_ring') {
    const result = ringDoorbell(msg.target, 'page');
    if (!result.ok) ws.send(JSON.stringify({ type: 'doorbell_result', deviceId: msg.target, ...result }));
    return;
  }

  // Update a device to the newest firmware for its board
  if (ws.role === 'browser' && msg.type === 'ota') {
    const dev = devices.get(msg.target);
    const meta = dev && firmwareList()[dev.board];
    if (!meta) return ws.send(JSON.stringify({ type: 'ota', deviceId: msg.target, state: 'failed', error: dev ? 'no firmware uploaded for this board' : 'device offline' }));
    return toDevice(msg.target, { type: 'ota_start', path: `/firmware/${meta.board}.bin`, version: meta.version, size: meta.size, sha256: meta.sha256 });
  }

  if (ws.role === 'browser' && msg.type === 'set_timezone') {
    if (!msg.target || !isValidTz(msg.tz)) return;
    deviceSettings[msg.target] = { ...deviceSettings[msg.target], tz: msg.tz };
    saveDeviceSettings();
    return sendTimezone(msg.target);
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
  else if (ws.role === 'device') {
    // Why the device last restarted (and crash details, if it crashed): keep it in the log
    if (msg.type === 'boot') {
      const { reason, fw, crash } = msg;
      console.log(`Device ${ws.deviceId} started (${reason}, firmware ${fw})${crash ? ' after a CRASH in ' + crash.task : ''}`);
      return logEvent({ type: 'boot', deviceId: ws.deviceId, reason: String(reason || 'unknown'), fw, ...(crash && { crash }), at: new Date().toISOString() });
    }
    const tagged = { ...msg, deviceId: ws.deviceId };
    // A one-time alarm has now rung: switch it off
    if (msg.type === 'alarm_fired') {
      const a = alarms.find((x) => x.id === msg.alarmId && x.deviceId === ws.deviceId);
      if (a?.date && a.enabled) saveAlarm({ ...a, enabled: false });
    }
    if (REPLAYED_REPORTS.includes(msg.type)) (ws.lastReports ??= {})[msg.type] = tagged;
    toBrowsers(tagged);
  }
}

function onClose(ws) {
  if (ws.talkTarget) toDevice(ws.talkTarget, { type: 'talk_stop' }); // tab closed mid-talk
  // Only if this socket is still the registered one (a reconnect may have replaced it)
  if (ws.role === 'device' && devices.get(ws.deviceId) === ws) {
    devices.delete(ws.deviceId);
    console.log(`Device offline: ${ws.deviceId}`);
    toBrowsers(deviceList());
  }
}

wss.on('connection', (ws) => {
  ws.on('message', (raw, isBinary) => onMessage(ws, raw, isBinary));
  ws.on('close', () => onClose(ws));
});

// ---- MQTT: devices connect to the broker (EMQX); the server bridges them ----
//   ss/dev/<id>/cmd     server → device   commands (JSON)
//   ss/dev/<id>/audio   server → device   hold-to-talk audio (binary)
//   ss/dev/<id>/evt     device → server   status and replies (JSON)
//   ss/dev/<id>/online  device            "1" retained; the broker sets "0" if it drops (last will)
//   ss/server/online    server            "1" retained / "0" last will: devices say hello again on "1"
//   ss/server/http      server            retained base URL for downloads (tones, firmware)
const MQTT_URL = process.env.MQTT_URL; // e.g. mqtts://xxxx.emqxsl.com:8883; unset = WebSocket devices only
let mqttClient = null;
const mqttLinks = new Map(); // deviceId -> link

// Stands in for a device WebSocket, so the rest of the server needn't care how a device is connected
function mqttLink(deviceId) {
  let link = mqttLinks.get(deviceId);
  if (!link) {
    link = {
      viaMqtt: true, deviceId, readyState: 3, bufferedAmount: 0,
      send(data, opts) {
        const audio = opts?.binary;
        mqttClient.publish(`ss/dev/${deviceId}/${audio ? 'audio' : 'cmd'}`, data, { qos: audio ? 0 : 1 });
      },
    };
    mqttLinks.set(deviceId, link);
  }
  return link;
}

function publishHttpBase() {
  const url = lanUrl();
  if (mqttClient?.connected && url) mqttClient.publish('ss/server/http', url, { retain: true, qos: 1 });
}

if (MQTT_URL) {
  mqttClient = require('mqtt').connect(MQTT_URL, {
    username: process.env.MQTT_USERNAME,
    password: process.env.MQTT_PASSWORD,
    clientId: `server-${crypto.randomUUID().slice(0, 8)}`,
    will: { topic: 'ss/server/online', payload: '0', retain: true, qos: 1 },
  });
  mqttClient.on('connect', () => {
    console.log(`MQTT connected to ${new URL(MQTT_URL).host}`);
    mqttClient.subscribe(['ss/dev/+/evt', 'ss/dev/+/online'], { qos: 1 });
    mqttClient.publish('ss/server/online', '1', { retain: true, qos: 1 });
    publishHttpBase();
  });
  mqttClient.on('error', (e) => console.log(`MQTT error: ${e.message}`));
  mqttClient.on('message', (topic, payload) => {
    const m = topic.match(/^ss\/dev\/(esp32-[0-9a-f]{12})\/(evt|online)$/);
    if (!m) return;
    const link = mqttLink(m[1]);
    if (m[2] === 'online') {
      if (payload.toString() === '1') link.readyState = 1;
      else { link.readyState = 3; onClose(link); }
      return;
    }
    link.readyState = 1;
    onMessage(link, payload, false);
  });
}
