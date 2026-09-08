const WebSocket = require('ws');
const http = require('http');
const fs = require('fs');
const path = require('path');

const PORT = 8080;
const publicDir = path.join(__dirname, 'public');

const server = http.createServer((req, res) => {
  const url = req.url === '/' ? '/index.html' : req.url;
  const filePath = path.join(publicDir, url);

  fs.readFile(filePath, (err, data) => {
    if (err) {
      res.writeHead(404, { 'Content-Type': 'text/plain' });
      res.end('Not found');
      return;
    }

    const ext = path.extname(filePath).toLowerCase();
    const type = {
      '.html': 'text/html',
      '.js': 'application/javascript',
      '.css': 'text/css'
    }[ext] || 'application/octet-stream';

    res.writeHead(200, { 'Content-Type': type });
    res.end(data);
  });
});

const wss = new WebSocket.Server({ server });
const clients = new Map();

function send(ws, type, payload) {
  if (ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify({ type, payload }));
  }
}

wss.on('connection', (ws) => {
  const id = `${Date.now()}-${Math.random().toString(16).slice(2)}`;
  clients.set(id, ws);
  ws.id = id;

  ws.on('message', (raw) => {
    try {
      const msg = JSON.parse(raw.toString());
      const { type, payload } = msg;

      console.log('[signal] ', type, payload && payload.type ? payload.type : '');

      if (type === 'register') {
        ws.role = payload.role || 'viewer';
        ws.name = payload.name || ws.role;
        send(ws, 'registered', { id: ws.id, role: ws.role });
        return;
      }

      if (type === 'offer' || type === 'answer' || type === 'candidate' || type === 'hangup' || type === 'ice') {
        for (const [peerId, peerWs] of clients.entries()) {
          if (peerWs !== ws && peerWs.role !== ws.role) {
            send(peerWs, type, payload);
          }
        }
      }

      if (type === 'hello') {
        for (const [peerId, peerWs] of clients.entries()) {
          if (peerWs !== ws) {
            send(peerWs, 'hello', { from: ws.id, role: ws.role });
          }
        }
      }
    } catch (e) {
      console.error('Invalid message', e.message);
    }
  });

  ws.on('close', () => {
    clients.delete(ws.id);
    console.log('Client disconnected', ws.id);
  });
});

server.listen(PORT, () => {
  console.log(`Signal server running at http://localhost:${PORT}`);
});
