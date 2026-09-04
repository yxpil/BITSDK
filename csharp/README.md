# BITSDK for C# (.NET 8)

Single-file SDK for the BIT HTTP API. The full endpoint/auth contract lives in
[`../docs/API.md`](../docs/API.md). Uses only `System.Net.Http.HttpClient` and
`System.Text.Json` — no external packages.

> **Status: not yet executed locally.** This SDK was written against a .NET 8
> target but the authoring machine had no .NET SDK installed, so the smoke
> test has not been run. Run it once with `dotnet run --project test` against
> `test/fake_bit_server.js` (see below).

## Files

| File | Purpose |
|---|---|
| `BitSdk.cs` | The SDK — `BitClient` + `BitException` (single file, namespace `BitSdk`) |
| `BitSdk.csproj` | net8.0 class library |
| `examples/Program.cs` | Runnable console example (`examples/Examples.csproj`) |
| `test/Program.cs` | Smoke test — plain assertions, exits non-zero on failure (`test/BitSdkSmoke.csproj`) |

## Usage

```csharp
using BitSdk;

using var bit = new BitClient("http://127.0.0.1:7777", "YOUR_CLIENT_KEY")
    .WithAccessPassword("your-access-password") // X-Access-Password on /api/*
    .WithTimeout(TimeSpan.FromSeconds(30));     // default is 30s

var health = await bit.HealthAsync();
var chat = await bit.ChatAsync("hello", sessionId: "mysession");
Console.WriteLine(chat.GetProperty("reply").GetString());
```

### Options (fluent)

| Method | Default | Meaning |
|---|---|---|
| `WithAccessPassword(string)` | `""` | Sent as `X-Access-Password` on `/api/*` paths |
| `WithTimeout(TimeSpan)` | 30 s | Per-request `HttpClient` timeout |
| `WithKeyInQuery(bool)` | `false` | `?key=<key>` query param instead of `Authorization: Bearer` — for clients that cannot set headers |

## Authentication (mirrors `../docs/API.md`)

| Path prefix | Client Key | X-Access-Password |
|---|---|---|
| `GET /api/health` | not needed | not needed |
| `/api/*` (rest) | required (`Bearer` or `?key=`) | required |
| `/v1/*` (OpenAI) | required | **exempt** |
| `/mcp` | required | **exempt** |

## Methods

Async primary (all return parsed `JsonElement` and throw `BitException` on
failure): `HealthAsync`, `ListToolsAsync`, `RegisterToolAsync(name,
description, JsonElement parameters, url)` (+ a `string parametersJson`
overload), `RemoveToolAsync(id)`, `InvokeToolAsync(id, JsonElement? params)`,
`ChatAsync(message, sessionId, images)`, `AuditAsync`, `DebugStateAsync`,
`DebugSessionsAsync`, `DebugSessionAsync(id)`, `DebugMcpAsync`,
`McpAsync(JsonElement jsonRpcPayload)` (raw JSON-RPC 2.0), `ModelsAsync`,
`ChatCompletionsAsync(JsonElement body)` (non-streaming OpenAI call),
`ChatCompletionsStreamAsync(...)`.

Sync wrappers (`Health()`, `ListTools()`, `Chat()`, ...) are provided for
convenience — they are trivial `GetAwaiter().GetResult()` bridges and must not
be called from UI threads / `async` contexts that expect no blocking.

## Errors

```csharp
public class BitException : Exception
{
    public int Status;   // HTTP status; 0 for transport failures (timeout, unreachable)
    public string Raw;   // raw response body
}
```

`Message` is prefixed in English (`BIT API error 401: ...`) and carries the
server text verbatim, extracted from the BIT error body's `error` field — a
plain string (`{"error":"..."}`) or an OpenAI-style object
(`{"error":{"message":"..."}}`).

## Streaming (SSE)

`ChatCompletionsStreamAsync(JsonElement body, Func<string?, JsonElement?,
Task> onDelta)` sets `stream: true` on the request body, issues the POST with
`HttpCompletionOption.ResponseHeadersRead`, and reads the
`text/event-stream` response through a UTF-8 `StreamReader` splitting lines
only on `\n` — so multi-byte UTF-8 characters split across TCP chunks are
decoded safely (the .NET UTF-8 decoder maintains partial-sequence state across
reads).

- `onDelta(chunkText, null)` fires per content chunk.
- `onDelta(null, finalChunk)` fires once for the last parsed chunk before
  `data: [DONE]` (it may carry `usage`).
- The method returns the assembled full text.

```csharp
var full = await bit.ChatCompletionsStreamAsync(body, (delta, final) =>
{
    if (delta != null) Console.Write(delta);
    if (final.HasValue && final.Value.TryGetProperty("usage", out _)) Console.WriteLine();
    return Task.CompletedTask;
});
```

## Running the smoke test

```bash
node test/fake_bit_server.js &        # from the repo root (port 9803)
dotnet run --project csharp/test      # prints [PASS]/[FAIL] per assertion,
                                      # exits non-zero on failure
```

Env overrides: `BIT_URL`, `BIT_KEY`, `BIT_PWD`. The suite only asserts on
objects it creates itself (the fake server is shared between concurrent test
runs).
