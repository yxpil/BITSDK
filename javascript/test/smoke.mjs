// Smoke test for bit-sdk-js. Runs against the fake BIT server
// (test/fake_bit_server.js on port 9803) — start it first if it is not running:
//
//   node test/fake_bit_server.js
//
// Usage:
//   BIT_URL=http://127.0.0.1:9803 BIT_KEY=bit_test_key_123456 BIT_PWD=test-pwd-1 \
//     node test/smoke.mjs
//
// Note: the fake server state is global and shared — tests only assert on
// objects they created, never on exact global list lengths.
import assert from 'node:assert/strict';
import { BitClient, BitError } from '../index.js';

const BASE_URL = process.env.BIT_URL || 'http://127.0.0.1:9803';
const CLIENT_KEY = process.env.BIT_KEY || 'bit_test_key_123456';
const ACCESS_PASSWORD = process.env.BIT_PWD || 'test-pwd-1';

const client = new BitClient({
  baseUrl: BASE_URL,
  clientKey: CLIENT_KEY,
  accessPassword: ACCESS_PASSWORD,
});

let passed = 0;
const failures = [];
async function test(name, fn) {
  try {
    await fn();
    passed++;
    console.log(`PASS ${name}`);
  } catch (e) {
    failures.push(name);
    console.log(`FAIL ${name}\n${e && e.stack ? e.stack : e}`);
  }
}

const run = async () => {
  await test('health() returns ok === true', async () => {
    const h = await client.health();
    assert.equal(h.ok, true);
  });

  await test('registerTool returns tool with id; removeTool returns removed === id', async () => {
    const name = `js_smoke_${process.pid}_${Date.now()}`;
    const tool = await client.registerTool({
      name,
      description: 'smoke test tool',
      parameters: { type: 'object', properties: {} },
      url: 'http://127.0.0.1:9000/hook',
    });
    assert.ok(tool && tool.id, `tool id missing: ${JSON.stringify(tool)}`);
    assert.equal(tool.name, name);
    const removed = await client.removeTool(tool.id);
    assert.equal(removed.removed, tool.id);
  });

  // Re-register a fresh tool for the invoke test (keeps global state sane).
  /** @type {{id: string, name: string}} */
  let tool;
  await test('re-register tool for invoke test', async () => {
    const name = `js_smoke_invoke_${process.pid}_${Date.now()}`;
    tool = await client.registerTool({
      name,
      description: 'smoke invoke tool',
      parameters: { type: 'object', properties: {} },
      url: 'http://127.0.0.1:9000/hook',
    });
    assert.ok(tool && tool.id);
  });

  await test('invokeTool returns result.via === tool name', async () => {
    const result = await client.invokeTool(tool.id, { ping: 'pong' });
    assert.equal(result.via, tool.name);
    assert.deepEqual(result.echoed, { ping: 'pong' });
    await client.removeTool(tool.id);
  });

  await test('chat({message:"ping"}) reply starts with "fake reply"', async () => {
    const res = await client.chat({ message: 'ping' });
    assert.ok(typeof res.reply === 'string' && res.reply.startsWith('fake reply'), `reply: ${JSON.stringify(res.reply)}`);
  });

  await test('debugState has ai.active.model', async () => {
    const state = await client.debugState();
    assert.ok(state.ai && state.ai.active && typeof state.ai.active.model === 'string' && state.ai.active.model.length > 0);
  });

  await test('debugSessions lists at least one session', async () => {
    const res = await client.debugSessions();
    assert.ok(Array.isArray(res.sessions) && res.sessions.length >= 1);
  });

  await test("debugSession('default') has messages", async () => {
    const sess = await client.debugSession('default');
    assert.equal(sess.id, 'default');
    assert.ok(Array.isArray(sess.messages) && sess.messages.length >= 1);
  });

  await test('mcp initialize returns serverInfo.name === "fake-bit"', async () => {
    const res = await client.mcp({
      jsonrpc: '2.0',
      id: 1,
      method: 'initialize',
      params: { protocolVersion: '2025-03-26', capabilities: {}, clientInfo: { name: 'bit-sdk-js-smoke', version: '1.0.0' } },
    });
    assert.equal(res.jsonrpc, '2.0');
    assert.equal(res.id, 1);
    assert.equal(res.result.serverInfo.name, 'fake-bit');
  });

  await test('models().data.length === 2', async () => {
    const res = await client.models();
    assert.equal(res.data.length, 2);
  });

  await test("chatCompletions non-stream returns '你好，世界!'", async () => {
    const res = await client.chatCompletions({
      model: 'ignored',
      messages: [{ role: 'user', content: '你好，世界!' }],
    });
    assert.equal(res.choices[0].message.content, '你好，世界!');
  });

  await test("chatCompletionsStream assembles exactly '你好，世界!' with usage final chunk", async () => {
    let assembled = '';
    let finalChunk = null;
    const text = await client.chatCompletionsStream(
      { model: 'ignored', messages: [{ role: 'user', content: 'hi' }] },
      (delta, chunk) => {
        if (delta === null) finalChunk = chunk;
        else assembled += delta;
      }
    );
    assert.equal(text, '你好，世界!');
    assert.equal(assembled, '你好，世界!');
    assert.ok(finalChunk, 'final onDelta(null, chunk) call missing');
    assert.equal(finalChunk.choices[0].finish_reason, 'stop');
    assert.equal(finalChunk.usage.total_tokens, 17);
  });

  await test('wrong key -> BitError status 401', async () => {
    const bad = new BitClient({ baseUrl: BASE_URL, clientKey: 'definitely-wrong-key', accessPassword: ACCESS_PASSWORD });
    await assert.rejects(
      () => bad.listTools(),
      (e) => {
        assert.ok(e instanceof BitError);
        assert.equal(e.status, 401);
        assert.ok(e.message.includes('API Key'), `message: ${e.message}`);
        assert.equal(e.raw.error.code, 'invalid_api_key');
        return true;
      }
    );
  });

  await test('missing access password -> BitError status 401', async () => {
    const noPwd = new BitClient({ baseUrl: BASE_URL, clientKey: CLIENT_KEY });
    await assert.rejects(
      () => noPwd.listTools(),
      (e) => {
        assert.ok(e instanceof BitError);
        assert.equal(e.status, 401);
        assert.ok(e.message.includes('X-Access-Password'), `message: ${e.message}`);
        return true;
      }
    );
  });

  await test('keyInQuery:true works for listTools (?key= accepted)', async () => {
    const q = new BitClient({ baseUrl: BASE_URL, clientKey: CLIENT_KEY, accessPassword: ACCESS_PASSWORD, keyInQuery: true });
    const res = await q.listTools();
    assert.ok(Array.isArray(res.tools));
  });

  await test('network error -> BitError status 0', async () => {
    const dead = new BitClient({ baseUrl: 'http://127.0.0.1:1', clientKey: CLIENT_KEY, accessPassword: ACCESS_PASSWORD, timeoutMs: 3000 });
    await assert.rejects(
      () => dead.listTools(),
      (e) => {
        assert.ok(e instanceof BitError);
        assert.equal(e.status, 0);
        assert.ok(e.message.startsWith('network error:'), `message: ${e.message}`);
        return true;
      }
    );
  });
};

try {
  await run();
} finally {
  console.log(`\n${passed} passed, ${failures.length} failed`);
  if (failures.length > 0) {
    console.log(`failed: ${failures.join(', ')}`);
    process.exitCode = 1;
  }
}
