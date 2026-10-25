// Test-only Node.js harness. The deployed MCP executable needs no Node.js runtime.
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { createInterface } from 'node:readline';

const fixture = await mkdtemp(join(tmpdir(), 'sshhelper-stdio-'));
const executable = resolve(process.argv[2]);
const child = spawn(executable, ['--stdio', '--credentials', join(fixture, 'missing.credentials')], { windowsHide: true });
const pending = new Map();
let stderr = '';
child.stderr.on('data', chunk => { stderr += chunk; });
const exited = new Promise((resolve, reject) => {
  child.once('error', reject);
  child.once('exit', (code, signal) => resolve({ code, signal }));
});
createInterface({ input: child.stdout }).on('line', line => {
  try {
    const response = JSON.parse(line);
    const request = pending.get(response.id);
    assert(request, `Unexpected stdout: ${line}`);
    pending.delete(response.id);
    clearTimeout(request.timeout);
    request.resolve(response);
  } catch (error) {
    for (const request of pending.values()) request.reject(error);
    child.kill();
  }
});
let id = 0;
function request(method, params = {}) {
  const requestId = ++id;
  return new Promise((resolve, reject) => {
    const timeout = setTimeout(() => reject(new Error(`${method} timed out; stderr=${stderr}`)), 10000);
    pending.set(requestId, { resolve, reject, timeout });
    child.stdin.write(JSON.stringify({ jsonrpc: '2.0', id: requestId, method, params }) + '\n');
  });
}
try {
  assert.equal((await request('tools/list')).error.code, -32002);
  const hello = await request('initialize', {
    protocolVersion: '2025-06-18', capabilities: {}, clientInfo: { name: 'native-stdio-test', version: '1' }
  });
  assert.equal(hello.result.serverInfo.name, 'sshhelper-tool');
  child.stdin.write(JSON.stringify({ jsonrpc: '2.0', method: 'notifications/initialized' }) + '\n');
  assert.deepEqual((await request('tools/list')).result.tools.map(x => x.name), ['list_servers', 'get_monitoring']);
  const servers = (await request('tools/call', { name: 'list_servers' })).result;
  assert.equal(servers.isError, false);
  assert.equal(servers.structuredContent.servers[0].server_id, 'local');
  const data = (await request('tools/call', { name: 'get_monitoring', arguments: { server: 'local', wait_seconds: 5 } })).result;
  assert.equal(data.isError, false);
  assert.deepEqual(JSON.parse(data.content[0].text), data.structuredContent);
  const local = data.structuredContent.servers[0];
  assert.equal(local.state, 'online');
  assert.equal(local.is_fresh, true);
  assert.equal(local.rates_ready, true);
  assert(local.metrics.memory.total_bytes > 0);
  assert(local.metrics.cpu.usage_percent >= 0);
  assert.equal(local.metrics.ssh_latency_ms, null);
  assert(local.age_ms < 3000);
  const begin = performance.now();
  const cached = (await request('tools/call', { name: 'get_monitoring', arguments: { server: 'local', fresh: false, wait_seconds: 0 } })).result;
  assert.equal(cached.structuredContent.requested_fresh, false);
  assert(performance.now() - begin < 1000);
  assert.equal((await request('tools/call', { name: 'get_monitoring', arguments: { command: 'whoami' } })).error.code, -32602);
  child.stdin.end();
  assert.equal((await exited).code, 0);
  assert.equal(stderr, '');
  console.log('Native MCP process: handshake, discovery, real Windows sampling, cache and clean shutdown passed');
} finally {
  child.kill();
  for (const item of pending.values()) clearTimeout(item.timeout);
  await exited.catch(() => {});
  await rm(fixture, { recursive: true, force: true });
}
