const express = require('express');
const { WebSocketServer } = require('ws');
const dgram = require('dgram');
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const MDNS_HOST = 'led-server.local';
const CLAIMS_FILE = path.join(__dirname, 'claims.json');
const SOUNDS_FILE = path.join(__dirname, 'sounds.json');
const SOUNDS_DIR = path.join(__dirname, 'sounds');
fs.mkdirSync(SOUNDS_DIR, { recursive: true });

// deviceId -> clientId of the browser that paired it. Persisted to disk so
// pairing survives a server restart. This is "first person to paste the
// UID owns it" style pairing, not real authentication.
let claims = {};
try {
  claims = JSON.parse(fs.readFileSync(CLAIMS_FILE, 'utf8'));
} catch {
  claims = {};
}

function saveClaims() {
  fs.writeFileSync(CLAIMS_FILE, JSON.stringify(claims, null, 2));
}

// Lowercased so a device is matched by ID regardless of the case it was
// typed in on the pairing form vs. the case the ESP32 reports (its MAC
// comes back uppercase, e.g. esp32-246F28AE5278) — a mismatch here used to
// mean "paired" but never "online" for the exact same device.
function normalizeDeviceId(id) {
  id = String(id || '').trim().toLowerCase();
  return id.startsWith('esp32-') ? id : `esp32-${id}`;
}

// Re-normalize any claims saved before IDs were lowercased, so existing
// pairings keep matching a freshly-connecting device.
claims = Object.fromEntries(
  Object.entries(claims).map(([deviceId, ownerId]) => [normalizeDeviceId(deviceId), ownerId])
);
saveClaims();

// deviceId -> { [soundId]: { name, size, status, reason?, path } }. The
// device's fs_report is authoritative for "ready" entries — this is a
// cache of what the device last reported, never an assumption (see
// docs/audio-alarm-design.md). `path` is server-internal (where the
// uploaded bytes live on disk) and never sent to the browser.
let soundsByDevice = {};
try {
  soundsByDevice = JSON.parse(fs.readFileSync(SOUNDS_FILE, 'utf8'));
} catch {
  soundsByDevice = {};
}

function saveSounds() {
  fs.writeFileSync(SOUNDS_FILE, JSON.stringify(soundsByDevice, null, 2));
}

function soundsListFor(deviceId) {
  const sounds = soundsByDevice[deviceId] || {};
  return Object.entries(sounds).map(([id, s]) => ({
    id, name: s.name, size: s.size, status: s.status, reason: s.reason || undefined,
  }));
}

function broadcastSoundsFor(deviceId) {
  const ownerId = claims[deviceId];
  if (!ownerId) return;
  const payload = JSON.stringify({ type: 'sounds', deviceId, sounds: soundsListFor(deviceId) });
  wss.clients.forEach((client) => {
    if (client.role === 'browser' && client.clientId === ownerId && client.readyState === 1) {
      client.send(payload);
    }
  });
}

// Relays a control message to whichever connected client is that device.
// Returns whether a currently-connected device actually received it.
function sendToDevice(deviceId, msgObj) {
  let sent = false;
  wss.clients.forEach((client) => {
    if (client.role === 'device' && client.readyState === 1 && client.deviceId === deviceId) {
      client.send(JSON.stringify(msgObj));
      sent = true;
    }
  });
  return sent;
}

const app = express();
app.use(express.static(path.join(__dirname, '../frontend')));

// Raw binary upload — the browser POSTs the file body directly (no
// multipart parsing needed). Ownership-checked the same way WS commands
// are: the browser must have this deviceId paired to its own clientId.
app.post('/upload/:deviceId', express.raw({ type: '*/*', limit: '20mb' }), (req, res) => {
  const deviceId = normalizeDeviceId(req.params.deviceId);
  const clientId = req.query.clientId;
  if (!clientId || claims[deviceId] !== clientId) {
    return res.status(403).json({ error: 'This browser has not paired that device' });
  }
  if (!Buffer.isBuffer(req.body) || req.body.length === 0) {
    return res.status(400).json({ error: 'Empty upload body' });
  }
  if (!localIp) {
    return res.status(503).json({ error: 'Server has no LAN IP yet, try again in a moment' });
  }

  const filename = req.get('X-Filename') || 'sound.wav';
  const id = crypto.randomBytes(6).toString('hex');
  const storedPath = path.join(SOUNDS_DIR, `${deviceId}_${id}`);
  fs.writeFileSync(storedPath, req.body);

  soundsByDevice[deviceId] = soundsByDevice[deviceId] || {};
  soundsByDevice[deviceId][id] = { name: filename, size: req.body.length, status: 'pending', path: storedPath };
  saveSounds();

  const url = `http://${localIp}:${PORT}/sounds/file/${deviceId}/${id}`;
  const delivered = sendToDevice(deviceId, {
    type: 'sync_file', target: deviceId, id, url, expectedSize: req.body.length,
  });
  if (!delivered) {
    soundsByDevice[deviceId][id].status = 'failed';
    soundsByDevice[deviceId][id].reason = 'device_offline';
    saveSounds();
  }
  broadcastSoundsFor(deviceId);
  res.json({ ok: true, id });
});

// Serves the raw bytes for a device to download during sync. Keyed off
// the manifest's own stored path, never a client-supplied filename, so
// there's no path-traversal surface here.
app.get('/sounds/file/:deviceId/:id', (req, res) => {
  const deviceId = normalizeDeviceId(req.params.deviceId);
  const entry = soundsByDevice[deviceId] && soundsByDevice[deviceId][req.params.id];
  if (!entry) return res.status(404).end();
  res.sendFile(entry.path);
});

const PORT = 3000;
const server = app.listen(PORT, '0.0.0.0', () => {
  console.log(`Server on http://0.0.0.0:${PORT}`);
});

// The machine this runs on may have several network interfaces (Wi-Fi,
// Ethernet, WSL/Hyper-V virtual adapters, ...). Advertising all of them
// over mDNS confuses ESP32 clients, which expect a single A record. This
// picks the one real IP that's actually used to reach the LAN/internet.
function getLocalIp() {
  return new Promise((resolve, reject) => {
    const socket = dgram.createSocket('udp4');
    socket.connect(80, '8.8.8.8', () => {
      const { address } = socket.address();
      socket.close();
      resolve(address);
    });
    socket.on('error', reject);
  });
}

// Set once getLocalIp() resolves; used both for mDNS answers and to build
// the download URL an ESP32 fetches an uploaded sound from (sync_file).
let localIp = null;

getLocalIp().then((ip) => {
  localIp = ip;

  // Pin the socket to the real LAN interface. Without this, Windows'
  // routing table decides which of the machine's several interfaces
  // (Ethernet, WSL/Hyper-V virtual adapters, ...) outgoing multicast
  // replies go out on, and it can pick a virtual one that never reaches
  // devices on the actual Wi-Fi network.
  const mdns = require('multicast-dns')({ interface: localIp });

  mdns.on('query', (query) => {
    const hit = query.questions.some(
      (q) => q.name === MDNS_HOST && (q.type === 'A' || q.type === 'ANY')
    );
    if (hit) {
      mdns.respond({ answers: [{ name: MDNS_HOST, type: 'A', ttl: 120, data: localIp }] });
    }
  });

  console.log(`Advertising ${MDNS_HOST} -> ${localIp} via mDNS`);
});

const wss = new WebSocketServer({ server });

function connectedDeviceIds() {
  return [...wss.clients].filter((c) => c.role === 'device').map((c) => c.deviceId);
}

// Only devices this specific browser has paired, and that are currently online.
function deviceListFor(ws) {
  if (!ws.clientId) return [];
  const online = new Set(connectedDeviceIds());
  return Object.entries(claims)
    .filter(([deviceId, ownerId]) => ownerId === ws.clientId && online.has(deviceId))
    .map(([deviceId]) => deviceId);
}

function sendDeviceList(target) {
  target.send(JSON.stringify({ type: 'devices', devices: deviceListFor(target) }));
}

function broadcastDeviceListToBrowsers() {
  wss.clients.forEach((client) => {
    if (client.role === 'browser' && client.readyState === 1) sendDeviceList(client);
  });
}

wss.on('connection', (ws) => {
  console.log('Client connected');

  ws.on('message', (raw) => {
    let msg;
    try { msg = JSON.parse(raw); }
    catch { console.log('Non-JSON:', raw.toString()); return; }

    console.log('Received:', msg);

    if (msg.type === 'hello' && msg.role === 'device') {
      ws.role = 'device';
      ws.deviceId = normalizeDeviceId(msg.deviceId);
      console.log(`Device identified: ${ws.deviceId}`);
      broadcastDeviceListToBrowsers();
      return;
    }

    if (msg.type === 'hello' && msg.role === 'browser') {
      ws.role = 'browser';
      ws.clientId = msg.clientId;
      sendDeviceList(ws);
      return;
    }

    if (msg.type === 'claim_device') {
      const deviceId = normalizeDeviceId(msg.deviceId);
      const currentOwner = claims[deviceId];
      if (currentOwner && currentOwner !== ws.clientId) {
        ws.send(JSON.stringify({ type: 'claim_result', ok: false, deviceId, message: 'Already paired to another browser' }));
        return;
      }
      claims[deviceId] = ws.clientId;
      saveClaims();
      ws.send(JSON.stringify({ type: 'claim_result', ok: true, deviceId }));
      sendDeviceList(ws);
      return;
    }

    if (msg.type === 'list_devices') {
      sendDeviceList(ws);
      return;
    }

    if (msg.type === 'list_sounds') {
      const target = normalizeDeviceId(msg.target);
      if (claims[target] !== ws.clientId) return;
      ws.send(JSON.stringify({ type: 'sounds', deviceId: target, sounds: soundsListFor(target) }));
      return;
    }

    // Browser -> device commands. All subject to the same ownership check:
    // the server only relays it if the requesting browser's clientId has
    // paired that deviceId.
    if (['flash', 'reset_wifi', 'play_sound', 'stop_sound', 'delete_sound'].includes(msg.type)) {
      const target = normalizeDeviceId(msg.target);
      if (claims[target] !== ws.clientId) {
        console.log(`Rejected: ${ws.clientId} does not own ${target}`);
        return;
      }
      const delivered = sendToDevice(target, { ...msg, target });

      if (msg.type === 'delete_sound' && delivered && soundsByDevice[target]) {
        // Optimistic: the device's next fs_report is what actually
        // reconciles this, but there's no reason to show a stale entry
        // in the meantime.
        delete soundsByDevice[target][msg.id];
        saveSounds();
        broadcastSoundsFor(target);
      }
      return;
    }

    // Device -> server reports, all keyed off ws.deviceId (never a
    // client-supplied target — a device can only ever report on itself).
    if (msg.type === 'sync_result' && ws.role === 'device') {
      const entry = soundsByDevice[ws.deviceId] && soundsByDevice[ws.deviceId][msg.id];
      if (entry) {
        entry.status = msg.ok ? 'ready' : 'failed';
        entry.reason = msg.ok ? undefined : (msg.reason || 'unknown');
        saveSounds();
        broadcastSoundsFor(ws.deviceId);
      }
      return;
    }

    if (msg.type === 'fs_report' && ws.role === 'device') {
      // The device is the source of truth for what's physically on its
      // flash chip — this overwrites the server's manifest accordingly,
      // including marking a sound "missing" if the device no longer has
      // it even though the server's last record said "ready".
      const deviceId = ws.deviceId;
      soundsByDevice[deviceId] = soundsByDevice[deviceId] || {};
      const known = soundsByDevice[deviceId];
      const reported = new Map((msg.files || []).map((f) => [f.id, f.size]));

      for (const [id, entry] of Object.entries(known)) {
        if (entry.status === 'ready' && !reported.has(id)) entry.status = 'missing';
      }
      for (const [id, size] of reported) {
        if (!known[id]) known[id] = { name: id, size, status: 'ready' };
        else { known[id].status = 'ready'; known[id].size = size; }
      }
      saveSounds();
      broadcastSoundsFor(deviceId);
      return;
    }

    if (msg.type === 'play_failed' && ws.role === 'device') {
      const ownerId = claims[ws.deviceId];
      wss.clients.forEach((client) => {
        if (client.role === 'browser' && client.clientId === ownerId && client.readyState === 1) {
          client.send(JSON.stringify({ type: 'play_failed', deviceId: ws.deviceId, id: msg.id, reason: msg.reason }));
        }
      });
      return;
    }
  });

  ws.on('close', () => {
    console.log('Client disconnected');
    if (ws.role === 'device') broadcastDeviceListToBrowsers();
  });
});
