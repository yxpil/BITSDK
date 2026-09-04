# BIT HTTP API Contract (BITSDK reference)

This is the single source of truth every language SDK in this repo implements.
It mirrors BIT `v0.5.2` (`src-tauri/src/http_api.rs`). If BIT and this doc ever
disagree, this doc is wrong — fix it here first.

## Base URL

```
http://127.0.0.1:7777        # BIT desktop app default remote port
```

The actual port is shown in BIT settings (远程访问 / Remote Access). All
examples below use `BIT_URL` for the base URL.

## Authentication (read carefully — this is the #1 integration mistake)

| Path prefix        | Client Key | X-Access-Password |
|--------------------|------------|-------------------|
| `GET /api/health`  | not needed | not needed        |
| `/api/*` (rest)    | required   | required          |
| `/v1/*` (OpenAI)   | required   | **exempt**        |
| `/mcp`             | required   | **exempt**        |

Client Key is sent as `Authorization: Bearer <key>` **or** `?key=<key>` query
parameter. The access password is sent as `X-Access-Password: <password>`.

Special statuses:

- `503` on every authenticated path when BIT has **no Client Key configured**
  (remote access disabled). Body: `{"error":"BIT 尚未配置 Client Key，远程访问已禁用"}`.
- `401` when key is wrong: `{"error":{"message":"无效的 API Key（BIT Client Key）","type":"invalid_request_error","code":"invalid_api_key"}}`.
- `401` when password missing/wrong: `{"error":"访问密码错误或缺失（需 X-Access-Password 头）"}`.
- All errors are JSON with an `error` field (string, or OpenAI-style object on
  `/v1/*`).

## Endpoints

### GET /api/health

```json
{"ok": true, "version": "0.5.2"}
```

### GET /api/tools

```json
{"tools": [ToolDef, ...]}
```

`ToolDef`:

```json
{
  "id": "t_1",
  "name": "weather",
  "description": "query weather",
  "parameters": {"type": "object", "properties": {}},
  "kind": {"type": "remote", "url": "http://127.0.0.1:9000/hook"},
  "created_by": "user",
  "created_at": "2026-09-04 12:00:00",
  "enabled": true
}
```

`kind.type` ∈ `builtin | remote | script | interpreter | mcp`.

### POST /api/tools  (register a remote tool)

Request:

```json
{
  "name": "weather",
  "description": "query weather",
  "parameters": {"type": "object", "properties": {}},
  "url": "http://127.0.0.1:9000/hook"
}
```

- `201` → `{"tool": ToolDef}` (BIT will POST tool calls to `url`)
- `409` → `{"error": "..."}` (name already registered)
- `400` → `{"error": "缺少回调 url"}`

### DELETE /api/tools/{id}

- `200` → `{"removed": "<id>"}`
- `404` → `{"error": "..."}`

### POST /api/tools/{id}/invoke

Request: `{"params": {...}}` (missing `params` is treated as `{}`).

- `200` → `{"result": <arbitrary JSON>}`  (the tool's own return value)
- `400` → `{"error": "..."}`

### POST /api/chat  (run one agent turn)

Request:

```json
{
  "message": "hello",
  "session_id": "mysession",        // optional; creates the session if missing; omit = current active session
  "images": ["data:image/png;base64,..."]  // optional, only for vision models
}
```

- `200` → `{"reply": "<last assistant message>", "messages": [full session messages]}`
- `400` → `{"error": "缺少 message 字段"}` or provider error string

This is a full agent turn: BIT may internally call tools/MCP before replying.

### GET /api/audit

```json
{"entries": [{"ts": "...", "actor": "agent:xxxx", "action": "http.request", "target": "/api/tools", "detail": {}, "ok": true}]}
```

### GET /api/debug/state (read-only, for ADB-style debugging)

```json
{
  "ai": {"active": {"name": "...", "protocol": "openai", "model": "...", "base_url": "...", "api_key_hint": "sk-abc…(20)"} , "providers": [...]},
  "tools": [ToolDef...],
  "mcp": [...],
  "sessions": {"count": 3, "messages": 42},
  "memories": 5,
  "skills": 2,
  "remote": {"client_key_hint": "...", "port": 7777}
}
```

Field set is informational — treat unknown fields as optional.

### GET /api/debug/sessions

```json
{"sessions": [{"id": "s1", "title": "...", "messages": 4, "ts": "..."}]}
```

### GET /api/debug/sessions/{id}

Full session: `{"id": "...", "messages": [{"role": "user|assistant|tool", "content": "...", ...}]}` or `404`.

### GET /api/debug/mcp

```json
{"tools": [{"server": "srv", "name": "tool_a", "description": "..."}]}
```

### POST /mcp  (MCP Streamable HTTP, JSON-RPC 2.0)

Single JSON-RPC request → single JSON-RPC response (BIT acts as MCP **server**).
Common calls:

```json
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"sdk","version":"1.0"}}}
{"jsonrpc":"2.0","id":2,"method":"tools/list"}
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"echo","arguments":{"text":"hi"}}}
```

Responses: `{"jsonrpc":"2.0","id":N,"result":{...}}` or
`{"jsonrpc":"2.0","id":N,"error":{"code":-32601,"message":"..."}}`.

### GET /v1/models

```json
{"object":"list","data":[{"id":"<active model>","object":"model","created":0,"owned_by":"bit"},{"id":"bit","object":"model","created":0,"owned_by":"bit"}]}
```

### POST /v1/chat/completions  (OpenAI compatible; password exempt)

Request: standard OpenAI `chat.completions` body (`model` accepted but ignored —
BIT always routes to its active provider). `stream: true` switches to SSE.

Non-streaming response: standard OpenAI `chat.completion` object.

Streaming response (`Content-Type: text/event-stream`):

```
data: {"id":"...","choices":[{"index":0,"delta":{"content":"Hel"},"finish_reason":null}]}

data: {"id":"...","choices":[{"index":0,"delta":{"content":"lo"},"finish_reason":null}]}

data: {"id":"...","choices":[{"index":0,"delta":{},"finish_reason":"stop"}],"usage":{...}}

data: [DONE]
```

Notes:
- Lines are `data: <json>` separated by blank lines; a terminating `data: [DONE]` ends the stream.
- Multi-byte UTF-8 characters **can be split across TCP chunks** — decoders must buffer bytes and split only on `\n`.
- The final chunk before `[DONE]` may carry `usage` (prompt/completion tokens incl. cached tokens).

## Conventions for every SDK in this repo

1. Client constructor: `base_url`, `client_key`, optional `access_password`, optional timeout (default 30s).
2. Method names (snake or camel per language convention, same meaning):
   `health`, `listTools`, `registerTool`, `removeTool`, `invokeTool`,
   `chat`, `audit`, `debugState`, `debugSessions`, `debugSession`, `debugMcp`,
   `mcp` (raw JSON-RPC), `models`, `chatCompletions` (+ streaming variant).
3. Errors: throw/return a single error type carrying HTTP status + parsed
   message from the `error` field. Never silently swallow.
4. `?key=` query fallback MUST be available for clients that cannot set
   headers (some Lua/C environments) — expose it via an option `key_in_query`.
5. Every SDK ships a smoke test that runs against `test/fake_bit_server.js`
   (`node test/fake_bit_server.js` on port 9803; env `FAKE_KEY`, `FAKE_PWD`).
