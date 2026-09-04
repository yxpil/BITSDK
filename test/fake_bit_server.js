#!/usr/bin/env node
// Fake BIT server for BITSDK smoke tests.
// Faithfully mirrors BIT v0.5.2 auth rules and endpoint shapes (docs/API.md).
//
//   node test/fake_bit_server.js          # port 9803
//   FAKE_KEY=bit_x FAKE_PWD=p node ...    # override credentials
//
// Env used by smoke tests:
//   FAKE_PORT  (default 9803)  FAKE_KEY (default bit_test_key_123456)  FAKE_PWD (default test-pwd-1)
'use strict';
const http = require('http');

const PORT = Number(process.env.FAKE_PORT || 9803);
const KEY = process.env.FAKE_KEY || 'bit_test_key_123456';
const PWD = process.env.FAKE_PWD || 'test-pwd-1';
const START = Date.now();

const tools = new Map();
let audit = [];
let toolSeq = 0;

function record(actor, action, target, ok) {
  audit.unshift({ ts: new Date().toISOString(), actor, action, target, detail: { method: 'x' }, ok: ok !== false });
}

function json(res, code, obj) {
  const body = JSON.stringify(obj);
  res.writeHead(code, { 'content-type': 'application/json; charset=utf-8' });
  res.end(body);
}

function errBody(clientKeyValid) {
  return clientKeyValid
    ? { error: '访问密码错误或缺失（需 X-Access-Password 头）' }
    : { error: { message: '无效的 API Key（BIT Client Key）', type: 'invalid_request_error', code: 'invalid_api_key' } };
}

// Returns null if authorized, else responds and returns true.
function checkAuth(req, res, urlObj, path) {
  if (path === '/api/health') return false;
  if (!KEY) {
    json(res, 503, { error: 'BIT 尚未配置 Client Key，远程访问已禁用' });
    return true;
  }
  const auth = req.headers['authorization'] || '';
  const bearer = auth.startsWith('Bearer ') ? auth.slice(7) : '';
  const qkey = (urlObj.searchParams.get('key') || '');
  if (bearer !== KEY && qkey !== KEY) {
    record('agent:unknown', 'http.auth_failed', path, false);
    json(res, 401, errBody(false));
    return true;
  }
  const needsPwd = !path.startsWith('/v1/') && path !== '/mcp';
  if (needsPwd && (req.headers['x-access-password'] || '') !== PWD) {
    record('agent:invalid', 'http.auth_failed', path, false);
    json(res, 401, errBody(true));
    return true;
  }
  return false;
}

function readBody(req) {
  return new Promise((resolve) => {
    let b = '';
    req.on('data', (c) => (b += c));
    req.on('end', () => {
      try { resolve(JSON.parse(b || '{}')); } catch { resolve({ __badjson: true }); }
    });
  });
}

const session = {
  id: 'default',
  messages: [
    { role: 'user', content: 'hi' },
    { role: 'assistant', content: 'Hello from fake BIT. 你好 👋' },
  ],
};

const server = http.createServer(async (req, res) => {
  const urlObj = new URL(req.url, 'http://127.0.0.1');
  const path = urlObj.pathname;

  try {
    // ---- OpenAI streaming (no password) ----
    if (path === '/v1/chat/completions' && req.method === 'POST') {
      if (checkAuth(req, res, urlObj, path)) return;
      const body = await readBody(req);
      const actor = actorOf(req);
      if (body.stream) {
        res.writeHead(200, { 'content-type': 'text/event-stream', 'cache-control': 'no-cache' });
        const chunks = ['你好', '，世', '界!'];
        let i = 0;
        const tick = () => {
          if (i < chunks.length) {
            res.write(`data: ${JSON.stringify({ id: 'chatcmpl-1', object: 'chat.completion.chunk', choices: [{ index: 0, delta: { content: chunks[i] }, finish_reason: null }] })}\n\n`);
            i++;
            setTimeout(tick, 10);
          } else if (i === chunks.length) {
            res.write(`data: ${JSON.stringify({ id: 'chatcmpl-1', object: 'chat.completion.chunk', choices: [{ index: 0, delta: {}, finish_reason: 'stop' }], usage: { prompt_tokens: 12, completion_tokens: 5, total_tokens: 17, prompt_tokens_details: { cached_tokens: 8 } } })}\n\n`);
            res.write('data: [DONE]\n\n');
            res.end();
            record(actor, 'chat.openai', '/v1/chat/completions', true);
          }
        };
        tick();
        return;
      }
      record(actorOf(req), 'chat.openai', '/v1/chat/completions', true);
      return json(res, 200, {
        id: 'chatcmpl-1', object: 'chat.completion', created: Math.floor(Date.now() / 1000), model: 'fake-model',
        choices: [{ index: 0, message: { role: 'assistant', content: '你好，世界!' }, finish_reason: 'stop' }],
        usage: { prompt_tokens: 12, completion_tokens: 5, total_tokens: 17 },
      });
    }

    if (path === '/v1/models' && req.method === 'GET') {
      if (checkAuth(req, res, urlObj, path)) return;
      return json(res, 200, { object: 'list', data: [{ id: 'fake-model', object: 'model', created: 0, owned_by: 'bit' }, { id: 'bit', object: 'model', created: 0, owned_by: 'bit' }] });
    }

    // ---- MCP (no password) ----
    if (path === '/mcp' && req.method === 'POST') {
      if (checkAuth(req, res, urlObj, path)) return;
      const body = await readBody(req);
      if (body.method === 'initialize') {
        return json(res, 200, { jsonrpc: '2.0', id: body.id, result: { protocolVersion: '2025-03-26', capabilities: { tools: {} }, serverInfo: { name: 'fake-bit', version: '0.5.2' } } });
      }
      if (body.method === 'tools/list') {
        return json(res, 200, { jsonrpc: '2.0', id: body.id, result: { tools: [{ name: 'echo', description: 'echo text', inputSchema: { type: 'object', properties: { text: { type: 'string' } } } }] } });
      }
      if (body.method === 'tools/call') {
        return json(res, 200, { jsonrpc: '2.0', id: body.id, result: { content: [{ type: 'text', text: `echo:${(body.params.arguments || {}).text}` }] } });
      }
      return json(res, 200, { jsonrpc: '2.0', id: body.id, error: { code: -32601, message: 'method not found' } });
    }

    // ---- /api/* ----
    if (path === '/api/health' && req.method === 'GET') {
      return json(res, 200, { ok: true, version: '0.5.2', uptime: (Date.now() - START) / 1000 });
    }

    if (checkAuth(req, res, urlObj, path)) return;
    const actor = actorOf(req);

    if (path === '/api/tools' && req.method === 'GET') {
      return json(res, 200, { tools: [...tools.values()] });
    }
    if (path === '/api/tools' && req.method === 'POST') {
      const body = await readBody(req);
      if (!body.url || !String(body.url).trim()) return json(res, 400, { error: '缺少回调 url' });
      if ([...tools.values()].some((t) => t.name === body.name)) return json(res, 409, { error: `tool ${body.name} already exists` });
      const tool = {
        id: `t_${++toolSeq}`, name: body.name, description: body.description || '',
        parameters: body.parameters || { type: 'object', properties: {} },
        kind: { type: 'remote', url: String(body.url).trim() }, created_by: actor, created_at: new Date().toISOString(), enabled: true,
      };
      tools.set(tool.id, tool);
      record(actor, 'tool.register', tool.name, true);
      return json(res, 201, { tool });
    }
    const mInvoke = path.match(/^\/api\/tools\/([^/]+)\/invoke$/);
    if (mInvoke && req.method === 'POST') {
      const body = await readBody(req);
      if (!tools.has(mInvoke[1])) return json(res, 400, { error: `tool ${mInvoke[1]} not found` });
      return json(res, 200, { result: { echoed: body.params || {}, via: tools.get(mInvoke[1]).name } });
    }
    const mTool = path.match(/^\/api\/tools\/([^/]+)$/);
    if (mTool && req.method === 'DELETE') {
      if (!tools.has(mTool[1])) return json(res, 404, { error: 'tool not found' });
      tools.delete(mTool[1]);
      return json(res, 200, { removed: mTool[1] });
    }
    if (path === '/api/chat' && req.method === 'POST') {
      const body = await readBody(req);
      if (!body.message || !String(body.message).trim()) return json(res, 400, { error: '缺少 message 字段' });
      const reply = `fake reply to: ${body.message}`;
      session.messages.push({ role: 'user', content: body.message }, { role: 'assistant', content: reply });
      record(actor, 'chat.remote', 'ai', true);
      return json(res, 200, { reply, messages: session.messages });
    }
    if (path === '/api/audit' && req.method === 'GET') {
      return json(res, 200, { entries: audit.slice(0, 50) });
    }
    if (path === '/api/debug/state' && req.method === 'GET') {
      return json(res, 200, {
        ai: { active: { name: 'fake', protocol: 'openai', model: 'fake-model', base_url: 'http://127.0.0.1:9901/v1', api_key_hint: 'sk-fak…(12)' }, providers: [] },
        tools: [...tools.values()],
        mcp: [],
        sessions: { count: 1, messages: session.messages.length },
        memories: 3, skills: 1,
        remote: { port: PORT },
      });
    }
    if (path === '/api/debug/sessions' && req.method === 'GET') {
      return json(res, 200, { sessions: [{ id: session.id, title: 'Default', messages: session.messages.length, ts: new Date().toISOString() }] });
    }
    const mSess = path.match(/^\/api\/debug\/sessions\/([^/]+)$/);
    if (mSess && req.method === 'GET') {
      if (mSess[1] !== session.id) return json(res, 404, { error: 'session not found' });
      return json(res, 200, { id: session.id, messages: session.messages });
    }
    if (path === '/api/debug/mcp' && req.method === 'GET') {
      return json(res, 200, { tools: [{ server: 'fake-srv', name: 'echo', description: 'echo' }] });
    }

    json(res, 404, { error: 'not found' });
  } catch (e) {
    if (!res.headersSent) json(res, 500, { error: e.message });
    else try { res.end(); } catch {}
  }
});

function actorOf(req) {
  const auth = req.headers['authorization'] || '';
  const key = auth.startsWith('Bearer ') ? auth.slice(7) : '';
  return key ? `agent:${key.slice(4, 12)}` : 'agent:unknown';
}

if (require.main === module) {
  server.listen(PORT, '127.0.0.1', () => {
    console.log(`[fake-bit] listening on http://127.0.0.1:${PORT} key=${KEY ? 'set' : 'EMPTY'} pwd=${PWD ? 'set' : 'EMPTY'}`);
  });
}

module.exports = { server, KEY, PWD, PORT };
