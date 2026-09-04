# bit-sdk (Rust)

Rust client for the BIT desktop agent HTTP API (BIT v0.5.2).

The binding contract — endpoints, auth rules, error body shapes, SSE
streaming format — lives in [`../docs/API.md`](../docs/API.md) and is the
single source of truth for this SDK.

Sync HTTP only (`ureq` + `serde`); no async runtime required.

## Installation

From crates.io (package name `bit-sdk`):

```toml
[dependencies]
bit-sdk = "1.0.0"
```

Or as a path dependency inside this repo:

```toml
[dependencies]
bit-sdk = { path = "rust" }
```

## Authentication

- **Client Key** — sent as `Authorization: Bearer <key>` by default. Build
  the client with `.key_in_query(true)` to send it as `?key=<key>` instead
  (for environments that cannot set headers).
- **Access password** — sent as `X-Access-Password: <pw>` when configured
  via `.access_password(...)`. Required on `/api/*`; `/v1/*` and `/mcp` are
  password-exempt, so the header is simply ignored there.

| Path prefix        | Client Key | X-Access-Password |
|--------------------|------------|-------------------|
| `GET /api/health`  | not needed | not needed        |
| `/api/*` (rest)    | required   | required          |
| `/v1/*` (OpenAI)   | required   | **exempt**        |
| `/mcp`             | required   | **exempt**        |

Every failure surfaces as a single error type:

```rust
pub struct BitError {
    pub status: u16,                     // 0 = transport-level failure
    pub message: String,                 // parsed from the `error` field
    pub raw: Option<serde_json::Value>,  // raw JSON body when parseable
}
```

Both BIT error shapes are handled: `{"error": "..."}` and the OpenAI-style
`{"error": {"message": ..., ...}}`.

## Usage

```rust
use bit_sdk::{BitClient, ChatInput, RegisterToolInput};
use serde_json::json;
use std::time::Duration;

let client = BitClient::new("http://127.0.0.1:7777", "your-client-key")
    .access_password("your-access-password")
    .timeout(Duration::from_secs(30));

let health = client.health()?;
let tools = client.list_tools()?;

let tool = client.register_tool(RegisterToolInput {
    name: "weather".into(),
    description: "query weather".into(),
    parameters: json!({"type": "object", "properties": {}}),
    url: "http://127.0.0.1:9000/hook".into(),
})?;
let result = client.invoke_tool(&tool.id, json!({"city": "SF"}))?;
client.remove_tool(&tool.id)?;

let chat = client.chat(ChatInput {
    message: "hello".into(),
    session_id: None,   // omit = current active session
    images: vec![],     // data-URLs, vision models only
})?;
println!("{}", chat.reply);

let state = client.debug_state()?;
let sessions = client.debug_sessions()?;
let session = client.debug_session("default")?;
let entries = client.audit()?;
```

### Streaming (OpenAI compatible, SSE)

```rust
let full = client.chat_completions_stream(
    json!({"model": "bit", "messages": [{"role": "user", "content": "hi"}]}),
    |delta, last| {
        if let Some(chunk) = delta {
            print!("{chunk}"); // incremental content chunk
        }
        if let Some(chunk) = last {
            // last parsed chunk before `data: [DONE]` (may carry `usage`)
            eprintln!("\nfinal: {chunk}");
        }
    },
)?;
println!("\nfull text: {full}");
```

The stream decoder buffers raw bytes and splits only on `\n`
(`BufRead::read_until` + `String::from_utf8` per completed line), so
multi-byte UTF-8 characters split across TCP chunks are handled safely.

### Raw JSON-RPC over MCP (password-exempt)

```rust
let resp = client.mcp(json!({
    "jsonrpc": "2.0", "id": 1, "method": "initialize",
    "params": {
        "protocolVersion": "2025-03-26",
        "capabilities": {},
        "clientInfo": {"name": "sdk", "version": "1.0"}
    }
}))?;
```

`models()` and `chat_completions(body)` cover the rest of the OpenAI-compatible
surface (`/v1/models`, non-streaming `/v1/chat/completions`).

## Tests

Start the fake BIT server, then run cargo test:

```sh
cd rust
node ../test/fake_bit_server.js &   # http://127.0.0.1:9803
cargo test
```

Env overrides: `BIT_URL` (default `http://127.0.0.1:9803`),
`BIT_KEY` (default `bit_test_key_123456`), `BIT_PWD` (default `test-pwd-1`).
