# BITSDK for Lua

Zero-dependency Lua 5.4 SDK for the BIT HTTP API. HTTP transport is a `curl`
subprocess (curl ships with macOS and virtually every Linux); JSON is handled
by the bundled pure-Lua `json.lua`.

The full endpoint/auth contract lives in [`../docs/API.md`](../docs/API.md).

## Files

| File | Purpose |
|---|---|
| `bitsdk.lua` | The SDK — `BitClient` class |
| `json.lua` | Bundled pure-Lua JSON encoder/decoder |
| `examples/example.lua` | Runnable example |
| `test/smoke.lua` | Smoke test against `test/fake_bit_server.js` |

## Usage

```lua
local BitClient = require('bitsdk')

local bit = BitClient.new('http://127.0.0.1:7777', 'YOUR_CLIENT_KEY', {
  access_password = 'your-access-password', -- X-Access-Password on /api/*
  timeout_ms = 30000,                       -- per-request timeout (default 30s)
  key_in_query = false,                     -- true: send ?key= instead of Bearer
  curl_path = 'curl',                       -- override curl binary path
})

local health = bit:health()
print(health.ok, health.version)

local chat = bit:chat('hello', nil, nil)    -- (message, session_id, images)
print(chat.reply)
```

### Options

| Option | Default | Meaning |
|---|---|---|
| `access_password` | `nil` | Sent as `X-Access-Password` on `/api/*` paths |
| `timeout_ms` | `30000` | Passed to curl as `--max-time` (seconds) |
| `key_in_query` | `false` | `?key=<key>` query param instead of `Authorization: Bearer` — for clients that cannot set headers |
| `curl_path` | `"curl"` | Path to the curl binary |

### Methods

`health`, `list_tools`, `register_tool{name=, description=, parameters=, url=}`,
`remove_tool(id)`, `invoke_tool(id, params)`,
`chat(message, session_id, images)` / `chat_table{message=, session_id=, images=}`,
`audit`, `debug_state`, `debug_sessions`, `debug_session(id)`, `debug_mcp`,
`mcp(payload)` (raw JSON-RPC 2.0), `models`,
`chat_completions(body)` (non-streaming OpenAI call),
`chat_completions_stream(body, on_delta)`.

## Error handling

Every method follows the Lua idiom:

- **success** → `(parsed_table)`
- **failure** → `(nil, err)` where `err` is a table:

```lua
{
  status  = 401,            -- HTTP status; 0 for transport failures
  message = "无效的 API Key（BIT Client Key）",  -- server text, verbatim
  raw     = "{\"error\":...}" -- raw response body
}
```

The `message` is extracted from the BIT error body's `error` field (plain
string or OpenAI-style object with `message`). Transport failures (server
unreachable, curl missing, timeout) return `status = 0` and an English
`message` such as `curl failed: no HTTP response (...)`.

```lua
local res, err = bit:list_tools()
if not res then
  print('failed: ' .. tostring(err.status) .. ' ' .. tostring(err.message))
end
```

## Streaming (SSE)

`chat_completions_stream` sets `stream: true` on the body and reads the
`text/event-stream` response line by line. Lines are only ever split on `\n`,
so multi-byte UTF-8 characters split across TCP chunks are handled correctly.

- `on_delta(chunk_text, nil)` is called once per content chunk.
- `on_delta(nil, final_chunk)` is called once for the last parsed chunk before
  `data: [DONE]` (this chunk may carry `usage`).
- The method returns the assembled full text (or `nil, err`).

```lua
local text = bit:chat_completions_stream(
  { model = 'bit', messages = { { role = 'user', content = '你好' } } },
  function(delta, fin)
    if delta then io.write(delta) end
    if fin and fin.usage then io.write('\n') end
  end
)
```

## Running the smoke test

```bash
node test/fake_bit_server.js &        # from the repo root (port 9803)
cd lua && lua test/smoke.lua
```

Env overrides: `BIT_URL`, `BIT_KEY`, `BIT_PWD`. The test prints `[PASS] name`
per assertion and exits non-zero if anything fails.
