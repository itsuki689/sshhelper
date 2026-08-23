'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { EventEmitter } = require('node:events');
const { TransferManager, createAppServer, validateTransfer } = require('../server');

class FakeClient extends EventEmitter {
  connect() { queueMicrotask(() => this.emit('ready')); }
  end() { queueMicrotask(() => this.emit('close')); }
  sftp(callback) {
    callback(null, {
      fastPut(_source, _destination, options, done) {
        options.step(5, 5, 10);
        options.step(10, 5, 10);
        queueMicrotask(() => done());
      }
    });
  }
}

test('validates transfer input without exposing credentials', () => {
  const value = validateTransfer({
    direction: 'upload', host: 'example.com', username: 'ubuntu', password: 'secret',
    sourcePath: 'a.zip', destinationPath: '/tmp/a.zip'
  });
  assert.equal(value.port, 22);
  assert.equal(value.password, 'secret');
  assert.throws(() => validateTransfer({ direction: 'upload' }), /host/);
});

test('reports upload progress and completion', async () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'airsftp-'));
  const source = path.join(directory, 'ten-bytes');
  fs.writeFileSync(source, '0123456789');
  const manager = new TransferManager({ ClientCtor: FakeClient });
  const task = manager.create({
    direction: 'upload', host: 'example.com', username: 'ubuntu', password: 'secret',
    sourcePath: source, destinationPath: '/tmp/ten-bytes'
  });

  await new Promise((resolve, reject) => {
    const timeout = setTimeout(() => reject(new Error('transfer timed out')), 1000);
    manager.on(`task:${task.id}`, (current) => {
      if (current.status === 'completed') {
        clearTimeout(timeout);
        assert.equal(current.progress, 100);
        assert.equal(current.transferredBytes, 10);
        assert.equal(JSON.stringify(current).includes('secret'), false);
        resolve();
      }
    });
  });
  fs.rmSync(directory, { recursive: true, force: true });
});

test('serves health endpoint and rejects invalid jobs', async () => {
  const server = createAppServer();
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const { port } = server.address();
  const health = await fetch(`http://127.0.0.1:${port}/api/health`).then((value) => value.json());
  assert.deepEqual(health, { ok: true, service: 'AirSFTP' });

  const invalid = await fetch(`http://127.0.0.1:${port}/api/transfers`, {
    method: 'POST', headers: { 'Content-Type': 'application/json' }, body: '{}'
  });
  assert.equal(invalid.status, 400);
  assert.match((await invalid.json()).error, /direction/);
  await new Promise((resolve) => server.close(resolve));
});

test('exposes completed progress through HTTP and SSE', async () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'airsftp-http-'));
  const source = path.join(directory, 'ten-bytes');
  fs.writeFileSync(source, '0123456789');
  const manager = new TransferManager({ ClientCtor: FakeClient });
  const server = createAppServer({ manager });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const { port } = server.address();

  const created = await fetch(`http://127.0.0.1:${port}/api/transfers`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      direction: 'upload', host: 'example.com', username: 'ubuntu', password: 'secret',
      sourcePath: source, destinationPath: '/tmp/ten-bytes'
    })
  }).then((value) => value.json());

  await new Promise((resolve) => setTimeout(resolve, 20));
  const status = await fetch(`http://127.0.0.1:${port}${created.statusUrl}`).then((value) => value.json());
  assert.equal(status.status, 'completed');
  assert.equal(status.progress, 100);

  const events = await fetch(`http://127.0.0.1:${port}${created.eventsUrl}`).then((value) => value.text());
  assert.match(events, /event: progress/);
  assert.match(events, /"status":"completed"/);

  await new Promise((resolve) => server.close(resolve));
  fs.rmSync(directory, { recursive: true, force: true });
});
