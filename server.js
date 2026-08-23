'use strict';

const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const { randomUUID } = require('node:crypto');
const { EventEmitter } = require('node:events');
const { spawn } = require('node:child_process');
const { Client } = require('ssh2');

const ROOT = __dirname;
const MAX_BODY = 64 * 1024;
const TERMINAL = new Set(['completed', 'failed', 'cancelled']);
const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.json': 'application/json; charset=utf-8'
};

function validateTransfer(body) {
  if (!body || typeof body !== 'object' || Array.isArray(body)) throw new Error('请求体必须是 JSON 对象');

  const direction = body.direction;
  if (!['upload', 'download'].includes(direction)) throw new Error('direction 必须是 upload 或 download');

  const required = ['host', 'username', 'sourcePath', 'destinationPath'];
  for (const key of required) {
    if (typeof body[key] !== 'string' || !body[key].trim()) throw new Error(`${key} 不能为空`);
  }

  const port = body.port === undefined ? 22 : Number(body.port);
  if (!Number.isInteger(port) || port < 1 || port > 65535) throw new Error('port 必须是 1-65535 的整数');

  const hasPassword = typeof body.password === 'string' && body.password.length > 0;
  const hasKey = typeof body.privateKeyPath === 'string' && body.privateKeyPath.trim().length > 0;
  const useAgent = body.useAgent === true;
  if (!hasPassword && !hasKey && !useAgent) throw new Error('请提供 password、privateKeyPath 或 useAgent');
  if (useAgent && !process.env.SSH_AUTH_SOCK) throw new Error('当前环境没有可用的 SSH Agent');

  return {
    direction,
    host: body.host.trim(),
    port,
    username: body.username.trim(),
    password: hasPassword ? body.password : undefined,
    privateKeyPath: hasKey ? path.resolve(body.privateKeyPath.trim()) : undefined,
    passphrase: typeof body.passphrase === 'string' ? body.passphrase : undefined,
    useAgent,
    sourcePath: body.sourcePath.trim(),
    destinationPath: body.destinationPath.trim()
  };
}

function publicTask(task) {
  return {
    id: task.id,
    status: task.status,
    direction: task.direction,
    sourcePath: task.sourcePath,
    destinationPath: task.destinationPath,
    transferredBytes: task.transferredBytes,
    totalBytes: task.totalBytes,
    progress: task.progress,
    bytesPerSecond: task.bytesPerSecond,
    elapsedSeconds: task.elapsedSeconds,
    error: task.error,
    createdAt: task.createdAt,
    startedAt: task.startedAt,
    finishedAt: task.finishedAt
  };
}

class TransferManager extends EventEmitter {
  constructor({ ClientCtor = Client, fsModule = fs } = {}) {
    super();
    this.ClientCtor = ClientCtor;
    this.fs = fsModule;
    this.tasks = new Map();
  }

  create(rawInput) {
    const input = validateTransfer(rawInput);
    const task = {
      id: randomUUID(),
      status: 'queued',
      direction: input.direction,
      sourcePath: input.sourcePath,
      destinationPath: input.destinationPath,
      transferredBytes: 0,
      totalBytes: 0,
      progress: 0,
      bytesPerSecond: 0,
      elapsedSeconds: 0,
      error: null,
      createdAt: new Date().toISOString(),
      startedAt: null,
      finishedAt: null,
      connection: null,
      lastBytes: 0,
      lastTick: 0,
      lastEmit: 0
    };

    this.tasks.set(task.id, task);
    queueMicrotask(() => this.run(task, input));
    return publicTask(task);
  }

  get(id) {
    const task = this.tasks.get(id);
    return task ? publicTask(task) : null;
  }

  cancel(id) {
    const task = this.tasks.get(id);
    if (!task) return null;
    if (TERMINAL.has(task.status)) return publicTask(task);
    this.finish(task, 'cancelled');
    return publicTask(task);
  }

  update(task, force = false) {
    const now = Date.now();
    if (task.startedAt) task.elapsedSeconds = Number(((now - Date.parse(task.startedAt)) / 1000).toFixed(2));
    if (task.totalBytes > 0) task.progress = Number(Math.min(100, task.transferredBytes / task.totalBytes * 100).toFixed(2));

    if (task.lastTick) {
      const seconds = (now - task.lastTick) / 1000;
      if (seconds > 0) task.bytesPerSecond = Math.max(0, Math.round((task.transferredBytes - task.lastBytes) / seconds));
    }
    task.lastTick = now;
    task.lastBytes = task.transferredBytes;

    if (force || now - task.lastEmit >= 100) {
      task.lastEmit = now;
      this.emit(`task:${task.id}`, publicTask(task));
    }
  }

  finish(task, status, error) {
    if (TERMINAL.has(task.status)) return;
    task.status = status;
    task.error = error ? String(error.message || error) : null;
    task.finishedAt = new Date().toISOString();
    if (status === 'completed') {
      task.transferredBytes = task.totalBytes;
      task.progress = 100;
    }
    this.update(task, true);
    if (task.connection) task.connection.end();

    const cleanup = setTimeout(() => {
      this.tasks.delete(task.id);
      this.removeAllListeners(`task:${task.id}`);
    }, 10 * 60 * 1000);
    cleanup.unref();
  }

  async run(task, input) {
    if (TERMINAL.has(task.status)) return;
    const connection = new this.ClientCtor();
    task.connection = connection;
    task.status = 'connecting';
    this.update(task, true);

    const fail = (error) => this.finish(task, task.status === 'cancelled' ? 'cancelled' : 'failed', error);
    connection.once('error', fail);
    connection.once('close', () => {
      if (!TERMINAL.has(task.status)) fail(new Error('SSH 连接意外关闭'));
    });
    connection.once('ready', () => {
      if (task.status === 'cancelled') return connection.end();
      connection.sftp((error, sftp) => {
        if (error) return fail(error);
        this.transfer(task, input, sftp).catch(fail);
      });
    });

    try {
      const options = {
        host: input.host,
        port: input.port,
        username: input.username,
        readyTimeout: 15000,
        keepaliveInterval: 10000,
        keepaliveCountMax: 3
      };
      if (input.password) options.password = input.password;
      if (input.privateKeyPath) options.privateKey = await this.fs.promises.readFile(input.privateKeyPath);
      if (input.passphrase) options.passphrase = input.passphrase;
      if (input.useAgent) options.agent = process.env.SSH_AUTH_SOCK;
      connection.connect(options);
    } catch (error) {
      fail(error);
    }
  }

  async transfer(task, input, sftp) {
    task.status = 'transferring';
    task.startedAt = new Date().toISOString();
    task.lastTick = Date.now();

    if (input.direction === 'upload') {
      const stats = await this.fs.promises.stat(input.sourcePath);
      if (!stats.isFile()) throw new Error('sourcePath 必须是本地文件');
      task.totalBytes = stats.size;
    } else {
      const stats = await new Promise((resolve, reject) => {
        sftp.stat(input.sourcePath, (error, value) => error ? reject(error) : resolve(value));
      });
      if (!stats.isFile()) throw new Error('sourcePath 必须是远程文件');
      task.totalBytes = stats.size;
      await this.fs.promises.mkdir(path.dirname(path.resolve(input.destinationPath)), { recursive: true });
    }

    this.update(task, true);
    const step = (transferred, _chunk, total) => {
      task.transferredBytes = transferred;
      if (total > 0) task.totalBytes = total;
      this.update(task);
    };
    const method = input.direction === 'upload' ? 'fastPut' : 'fastGet';

    await new Promise((resolve, reject) => {
      sftp[method](input.sourcePath, input.destinationPath, { step }, (error) => error ? reject(error) : resolve());
    });
    this.finish(task, 'completed');
  }
}

function sendJson(response, status, body) {
  const data = JSON.stringify(body);
  response.writeHead(status, {
    'Content-Type': 'application/json; charset=utf-8',
    'Content-Length': Buffer.byteLength(data),
    'Cache-Control': 'no-store'
  });
  response.end(data);
}

async function readJson(request) {
  const chunks = [];
  let size = 0;
  for await (const chunk of request) {
    size += chunk.length;
    if (size > MAX_BODY) throw new Error('请求体不能超过 64 KB');
    chunks.push(chunk);
  }
  try {
    return JSON.parse(Buffer.concat(chunks).toString('utf8') || '{}');
  } catch {
    throw new Error('JSON 格式无效');
  }
}

function createAppServer({ manager = new TransferManager() } = {}) {
  return http.createServer(async (request, response) => {
    const url = new URL(request.url, 'http://127.0.0.1');

    try {
      if (request.method === 'GET' && url.pathname === '/api/health') {
        return sendJson(response, 200, { ok: true, service: 'AirSFTP' });
      }

      if (request.method === 'POST' && url.pathname === '/api/transfers') {
        const task = manager.create(await readJson(request));
        return sendJson(response, 202, {
          ...task,
          statusUrl: `/api/transfers/${task.id}`,
          eventsUrl: `/api/transfers/${task.id}/events`
        });
      }

      const match = url.pathname.match(/^\/api\/transfers\/([0-9a-f-]+)(\/events)?$/i);
      if (match) {
        const id = match[1];
        const task = manager.get(id);
        if (!task) return sendJson(response, 404, { error: '传输任务不存在或已过期' });

        if (request.method === 'GET' && match[2]) {
          response.writeHead(200, {
            'Content-Type': 'text/event-stream; charset=utf-8',
            'Cache-Control': 'no-cache, no-transform',
            'Connection': 'keep-alive',
            'X-Accel-Buffering': 'no'
          });
          const eventName = `task:${id}`;
          const heartbeat = setInterval(() => response.write(': ping\n\n'), 15000);
          heartbeat.unref();
          let closed = false;
          const cleanup = () => {
            if (closed) return;
            closed = true;
            clearInterval(heartbeat);
            manager.off(eventName, push);
          };
          const push = (value) => {
            if (closed) return;
            response.write(`event: progress\ndata: ${JSON.stringify(value)}\n\n`);
            if (TERMINAL.has(value.status)) {
              cleanup();
              response.end();
            }
          };
          manager.on(eventName, push);
          push(task);
          request.on('close', cleanup);
          return;
        }

        if (request.method === 'GET') return sendJson(response, 200, task);
        if (request.method === 'DELETE') return sendJson(response, 200, manager.cancel(id));
        return sendJson(response, 405, { error: '方法不允许' });
      }

      if (request.method !== 'GET' && request.method !== 'HEAD') return sendJson(response, 404, { error: '接口不存在' });

      const requested = url.pathname === '/' ? 'index.html' : decodeURIComponent(url.pathname.slice(1));
      const filePath = path.resolve(ROOT, requested);
      if (!filePath.startsWith(`${ROOT}${path.sep}`) && filePath !== path.join(ROOT, 'index.html')) {
        return sendJson(response, 403, { error: '禁止访问' });
      }
      const stats = await fs.promises.stat(filePath);
      if (!stats.isFile()) return sendJson(response, 404, { error: '文件不存在' });
      response.writeHead(200, {
        'Content-Type': MIME[path.extname(filePath)] || 'application/octet-stream',
        'Content-Length': stats.size,
        'Cache-Control': 'no-cache'
      });
      if (request.method === 'HEAD') return response.end();
      fs.createReadStream(filePath).pipe(response);
    } catch (error) {
      if (!response.headersSent) sendJson(response, 400, { error: error.message || '请求失败' });
      else response.end();
    }
  });
}

function openBrowser(url) {
  const commands = {
    win32: ['rundll32.exe', ['url.dll,FileProtocolHandler', url]],
    darwin: ['open', [url]],
    linux: ['xdg-open', [url]]
  };
  const command = commands[process.platform];
  if (!command) return;
  const child = spawn(command[0], command[1], {
    detached: true,
    stdio: 'ignore',
    windowsHide: true
  });
  child.unref();
}

if (require.main === module) {
  const host = process.env.AIRSFTP_HOST || '127.0.0.1';
  const port = Number(process.env.AIRSFTP_PORT || 4173);
  if (!Number.isInteger(port) || port < 1 || port > 65535) {
    console.error('AIRSFTP_PORT must be an integer between 1 and 65535');
    process.exitCode = 1;
    return;
  }
  const server = createAppServer();
  server.once('error', (error) => {
    console.error(error.code === 'EADDRINUSE' ? `Port ${port} is already in use` : error.message);
    process.exitCode = 1;
  });
  server.listen(port, host, () => {
    const url = `http://${host}:${port}`;
    console.log(`AirSFTP running at ${url}`);
    const shouldOpen = process.env.AIRSFTP_NO_OPEN !== '1' && (process.pkg || process.env.AIRSFTP_OPEN === '1');
    if (shouldOpen) setTimeout(() => openBrowser(url), 150);
  });
}

module.exports = { TransferManager, createAppServer, openBrowser, publicTask, validateTransfer };
