'use strict';
// BIT HTTP API client core (CommonJS). Shared by index.js (ESM) and index.cjs.
// Binding contract: ../docs/API.md
//
// Auth rules:
//   - Client Key: "Authorization: Bearer <key>" (or "?key=<key>" when keyInQuery).
//   - X-Access-Password: sent on all requests when configured; /v1/*, /mcp and
//     /api/health are password-exempt on the server side, sending it is harmless.

class BitError extends Error {
  /**
   * @param {number} status HTTP status code; 0 for network/timeout errors.
   * @param {string} message human-readable message parsed from the error body.
   * @param {*} raw parsed response body (raw text when the body was not JSON).
   */
  constructor(status, message, raw) {
    super(message);
    this.name = 'BitError';
    this.status = status;
    this.message = message;
    this.raw = raw;
  }
}

// Extract a message from BIT's JSON error body. Supports both shapes:
//   /v1/* style: {"error":{"message":"...","type":"...","code":"..."}}
//   plain style: {"error":"..."}
function messageFromBody(body, status) {
  if (body && typeof body === 'object' && body.error !== undefined) {
    const err = body.error;
    if (err && typeof err === 'object' && typeof err.message === 'string') return err.message;
    if (typeof err === 'string' && err.length > 0) return err;
  }
  return `request failed with status ${status}`;
}

class BitClient {
  /**
   * @param {object} options
   * @param {string} options.baseUrl e.g. "http://127.0.0.1:7777" (required)
   * @param {string} options.clientKey BIT Client Key (required)
   * @param {string} [options.accessPassword] sent as X-Access-Password
   * @param {number} [options.timeoutMs=30000] whole-request timeout
   * @param {boolean} [options.keyInQuery=false] send "?key=<key>" instead of the header
   */
  constructor(options = {}) {
    if (!options.baseUrl) throw new Error('baseUrl is required');
    if (!options.clientKey) throw new Error('clientKey is required');
    this.baseUrl = String(options.baseUrl).replace(/\/+$/, '');
    this.clientKey = options.clientKey;
    this.accessPassword = options.accessPassword;
    this.timeoutMs = options.timeoutMs === undefined ? 30000 : options.timeoutMs;
    this.keyInQuery = options.keyInQuery === true;
  }

  _url(path, query) {
    let url = this.baseUrl + path;
    const params = new URLSearchParams(query || {});
    if (this.keyInQuery) params.set('key', this.clientKey);
    const qs = params.toString();
    if (qs) url += (url.includes('?') ? '&' : '?') + qs;
    return url;
  }

  _headers(auth) {
    const headers = {};
    if (auth) {
      if (!this.keyInQuery) headers['Authorization'] = `Bearer ${this.clientKey}`;
      if (this.accessPassword !== undefined && this.accessPassword !== null) {
        headers['X-Access-Password'] = this.accessPassword;
      }
    }
    return headers;
  }

  // Single JSON request/response helper used by every non-streaming method.
  async _request(method, path, { body, query, auth = true } = {}) {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), this.timeoutMs);
    try {
      const headers = this._headers(auth);
      if (body !== undefined) headers['Content-Type'] = 'application/json';
      const res = await fetch(this._url(path, query), {
        method,
        headers,
        body: body !== undefined ? JSON.stringify(body) : undefined,
        signal: controller.signal,
      });
      const text = await res.text();
      let parsed = null;
      let parseFailed = false;
      try {
        parsed = text ? JSON.parse(text) : null;
      } catch {
        parseFailed = true;
      }
      if (!res.ok) {
        throw new BitError(res.status, messageFromBody(parsed, res.status), parseFailed ? text : parsed);
      }
      return parsed;
    } catch (e) {
      if (e instanceof BitError) throw e;
      const timedOut = controller.signal.aborted || (e && (e.name === 'AbortError' || e.name === 'TimeoutError'));
      const reason = timedOut ? `timeout after ${this.timeoutMs}ms` : (e && e.message) || String(e);
      throw new BitError(0, `network error: ${reason}`, null);
    } finally {
      clearTimeout(timer);
    }
  }

  // GET /api/health — no auth needed.
  async health() {
    return this._request('GET', '/api/health', { auth: false });
  }

  // GET /api/tools -> {"tools":[...]}
  async listTools() {
    return this._request('GET', '/api/tools');
  }

  // POST /api/tools -> returns the created ToolDef (unwrapped from {"tool":...}).
  async registerTool(tool) {
    const body = {
      name: tool.name,
      description: tool.description,
      parameters: tool.parameters,
      url: tool.url,
    };
    const res = await this._request('POST', '/api/tools', { body });
    return res && res.tool !== undefined ? res.tool : res;
  }

  // DELETE /api/tools/{id} -> {"removed":"<id>"}
  async removeTool(id) {
    return this._request('DELETE', `/api/tools/${encodeURIComponent(id)}`);
  }

  // POST /api/tools/{id}/invoke -> returns the tool's own result value.
  async invokeTool(id, params) {
    const res = await this._request('POST', `/api/tools/${encodeURIComponent(id)}/invoke`, {
      body: { params: params === undefined || params === null ? {} : params },
    });
    return res && res.result !== undefined ? res.result : res;
  }

  // POST /api/chat — one full agent turn -> {"reply","messages"}
  async chat({ message, sessionId, images } = {}) {
    const body = { message };
    if (sessionId !== undefined) body.session_id = sessionId;
    if (images !== undefined) body.images = images;
    return this._request('POST', '/api/chat', { body });
  }

  // GET /api/audit -> {"entries":[...]}
  async audit() {
    return this._request('GET', '/api/audit');
  }

  // GET /api/debug/state
  async debugState() {
    return this._request('GET', '/api/debug/state');
  }

  // GET /api/debug/sessions
  async debugSessions() {
    return this._request('GET', '/api/debug/sessions');
  }

  // GET /api/debug/sessions/{id}
  async debugSession(id) {
    return this._request('GET', `/api/debug/sessions/${encodeURIComponent(id)}`);
  }

  // GET /api/debug/mcp
  async debugMcp() {
    return this._request('GET', '/api/debug/mcp');
  }

  // POST /mcp — raw JSON-RPC 2.0 request -> raw JSON-RPC response.
  async mcp(payload) {
    return this._request('POST', '/mcp', { body: payload });
  }

  // GET /v1/models -> {"object":"list","data":[...]}
  async models() {
    return this._request('GET', '/v1/models');
  }

  // POST /v1/chat/completions (non-streaming).
  async chatCompletions(body) {
    return this._request('POST', '/v1/chat/completions', {
      body: Object.assign({}, body, { stream: false }),
    });
  }

  /**
   * POST /v1/chat/completions with stream:true (SSE).
   * Calls onDelta(textDelta) for every content chunk and onDelta(null, finalChunk)
   * once at the end (finalChunk is the last parsed JSON chunk, carries usage).
   * Returns the full assembled text.
   *
   * SSE notes (per docs/API.md): bytes are buffered and lines are split only on
   * "\n", so multi-byte UTF-8 split across TCP chunks never breaks decoding.
   */
  async chatCompletionsStream(body, onDelta) {
    const payload = Object.assign({}, body, { stream: true });
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), this.timeoutMs);
    let text = '';
    let lastChunk = null;
    try {
      const res = await fetch(this._url('/v1/chat/completions'), {
        method: 'POST',
        headers: Object.assign(
          { 'Content-Type': 'application/json', Accept: 'text/event-stream' },
          this._headers(true)
        ),
        body: JSON.stringify(payload),
        signal: controller.signal,
      });
      if (!res.ok) {
        const raw = await res.text();
        let parsed = null;
        let parseFailed = false;
        try {
          parsed = raw ? JSON.parse(raw) : null;
        } catch {
          parseFailed = true;
        }
        throw new BitError(res.status, messageFromBody(parsed, res.status), parseFailed ? raw : parsed);
      }
      if (!res.body) throw new Error('response has no body');

      const reader = res.body.getReader();
      // Split on "\n" at the byte level; decode one full line at a time.
      let buf = Buffer.alloc(0);
      let finished = false;
      const handleLine = (line) => {
        line = line.replace(/\r$/, '');
        if (!line.startsWith('data:')) return;
        const data = line.slice(5).trim();
        if (data === '[DONE]') {
          finished = true;
          return;
        }
        if (!data) return;
        let chunk;
        try {
          chunk = JSON.parse(data);
        } catch {
          return; // ignore malformed/comment SSE lines
        }
        lastChunk = chunk;
        const choice = chunk && chunk.choices && chunk.choices[0];
        const piece = choice && choice.delta && typeof choice.delta.content === 'string' ? choice.delta.content : '';
        if (piece) {
          text += piece;
          if (typeof onDelta === 'function') onDelta(piece, chunk);
        }
      };

      while (!finished) {
        const { done, value } = await reader.read();
        if (done) break;
        buf = Buffer.concat([buf, Buffer.from(value)]);
        let nl;
        while (!finished && (nl = buf.indexOf(0x0a)) !== -1) {
          handleLine(buf.subarray(0, nl).toString('utf8'));
          buf = buf.subarray(nl + 1);
        }
      }
      if (!finished && buf.length > 0) handleLine(buf.toString('utf8'));
      if (finished) {
        try {
          await reader.cancel();
        } catch {
          // stream already closed
        }
      }
      if (typeof onDelta === 'function') onDelta(null, lastChunk);
      return text;
    } catch (e) {
      if (e instanceof BitError) throw e;
      const timedOut = controller.signal.aborted || (e && (e.name === 'AbortError' || e.name === 'TimeoutError'));
      const reason = timedOut ? `timeout after ${this.timeoutMs}ms` : (e && e.message) || String(e);
      throw new BitError(0, `network error: ${reason}`, null);
    } finally {
      clearTimeout(timer);
    }
  }
}

module.exports = { BitClient, BitError };
