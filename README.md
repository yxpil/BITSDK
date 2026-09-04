# BITSDK — Call BIT from any language

Official client SDKs for [BIT](https://github.com/yxpil/bit) (本地优先的 AI Agent 工具集).
Drive your local BIT from Java, JavaScript/Node, Go, Rust, C, C# and Lua:
chat with the agent, register/remote-call tools, read audit and debug state,
or use BIT as an OpenAI-compatible endpoint and MCP server.

| Language | Directory | Transport | Streaming | Package |
|----------|-----------|-----------|-----------|---------|
| JavaScript / Node 18+ | [`javascript/`](javascript/) | fetch (zero-dep) | SSE, async-friendly | `bit-sdk-js` |
| Go 1.22+ | [`go/`](go/) | net/http (std-only) | SSE callback | `github.com/yxpil/BITSDK/go` |
| Rust | [`rust/`](rust/) | ureq (sync) | SSE iterator/callback | `bit-sdk` |
| Java 17+ | [`java/`](java/) | java.net.http (zero-dep) | SSE callback | copy the 3 source files |
| C | [`c/`](c/) | libcurl + bundled cJSON | SSE callback | `make` in `c/` |
| C# | [`csharp/`](csharp/) | HttpClient (zero-dep) | SSE (IAsyncEnumerable) | single file `BitSdk.cs` |
| Lua | [`lua/`](lua/) | curl subprocess (zero-dep) | line streaming | single file `bitsdk.lua` |

Full HTTP contract (endpoints, auth rules, error bodies, SSE format):
[`docs/API.md`](docs/API.md). Every SDK implements it and ships a smoke test
against the same fake BIT server (`test/fake_bit_server.js`).

## What BIT exposes

- `POST /api/chat` — one full agent turn (tools/MCP executed internally), returns the reply.
- `/api/tools` — list / register / remove / invoke tools. Remote tools: BIT POSTs tool calls to your webhook.
- `/api/audit`, `/api/debug/*` — audit log and read-only runtime snapshots (sessions, providers, MCP).
- `/v1/chat/completions`, `/v1/models` — OpenAI-compatible endpoint (streaming supported).
- `POST /mcp` — BIT as an MCP server (JSON-RPC 2.0).

## Authentication (30 seconds)

In BIT: 设置 → 远程访问 → enable, copy the **Client Key**, set an **访问密码 (access password)**.

| Path | Client Key | Access password |
|------|-----------|-----------------|
| `GET /api/health` | - | - |
| `/api/*` | Bearer header or `?key=` | `X-Access-Password` header |
| `/v1/*`, `/mcp` | Bearer header or `?key=` | exempt (OpenAI clients can't send custom headers) |

```python
# pseudocode for every SDK
client = BitClient("http://127.0.0.1:7777", client_key="bit_xxx", access_password="your-pwd")
print(client.health())                       # {"ok": true, ...}
print(client.chat({"message": "总结今天的待办"}))  # agent turn
```

Errors: any non-2xx raises/returns an error with the HTTP status and the
server's `error` message. `503` = BIT has no Client Key configured (remote
access disabled). `401` = wrong key or password.

## Quick start per language

<details><summary><b>JavaScript / Node</b></summary>

```js
import { BitClient } from "bit-sdk-js";
const client = new BitClient({
  baseUrl: "http://127.0.0.1:7777",
  clientKey: "bit_...",
  accessPassword: "...", // optional
});
const { reply } = await client.chat({ message: "你好" });
await client.chatCompletionsStream(
  { messages: [{ role: "user", content: "hi" }] },
  (delta) => { if (delta !== null) process.stdout.write(delta); },
);
```
</details>

<details><summary><b>Go</b></summary>

```go
bit := bitsdk.NewClient("http://127.0.0.1:7777", "bit_...",
    bitsdk.WithAccessPassword("..."))
chat, _ := bit.Chat(ctx, bitsdk.ChatInput{Message: "你好"})
fmt.Println(chat.Reply)
```
</details>

<details><summary><b>Rust</b></summary>

```rust
let bit = BitClient::new("http://127.0.0.1:7777", "bit_...")
    .access_password("...");
let chat = bit.chat(ChatInput {
    message: "你好".into(),
    session_id: None,
    images: vec![],
})?;
println!("{}", chat.reply);
```
</details>

<details><summary><b>Java</b></summary>

```java
BitClient bit = new BitClient("http://127.0.0.1:7777", "bit_...")
        .accessPassword("...");
Map<String, Object> res = bit.chat("你好");
```
</details>

<details><summary><b>C</b></summary>

```c
bit_client *c = bit_client_new("http://127.0.0.1:7777", "bit_...");
bit_set_access_password(c, "...");
cJSON *res = bit_chat(c, "你好", NULL);
```
</details>

<details><summary><b>C#</b></summary>

```csharp
using var bit = new BitSdk.BitClient("http://127.0.0.1:7777", "bit_...")
        .WithAccessPassword("...");
var chat = await bit.ChatAsync("你好");
```
</details>

<details><summary><b>Lua</b></summary>

```lua
local BitClient = require("bitsdk")
local bit = BitClient.new("http://127.0.0.1:7777", "bit_xxx",
                          { access_password = "..." })
local chat, err = bit:chat("你好")
if chat then print(chat.reply) end
```
</details>

## Development

```bash
node test/fake_bit_server.js   # fake BIT on 127.0.0.1:9803 (key: bit_test_key_123456 / pwd: test-pwd-1)
# then run each language's smoke test, e.g.
(cd javascript && node test/smoke.mjs)
(cd go         && go test ./...)
(cd rust       && cargo test)
(cd java       && ./run-smoke.sh)
(cd c          && make smoke && ./build/smoke)
(cd lua        && lua test/smoke.lua)
(cd csharp     && dotnet run --project test)   # needs .NET 8
```

## Links

- BIT app: https://github.com/yxpil/bit
- Docs site: https://osbt.space
- ADB debug bridge (inspector for BIT traffic): https://github.com/yxpil/ADB

## License

Apache-2.0
