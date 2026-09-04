# BITSDK — Java

Java client for the BIT HTTP API. Zero external dependencies (JDK 11+, uses
`java.net.http.HttpClient`). The binding contract is [`../docs/API.md`](../docs/API.md).

## Layout

- `src/space/osbt/bitsdk/BitClient.java` — HTTP client
- `src/space/osbt/bitsdk/BitJson.java` — minimal JSON parser/serializer
- `src/space/osbt/bitsdk/BitException.java` — error type (HTTP status + parsed error body)
- `test/BitClientSmokeTest.java` — smoke test (plain `main`, no JUnit)
- `run-smoke.sh` — compile + run the smoke test

## Build & test

```bash
./run-smoke.sh
```

Runs against the fake BIT server (`node ../test/fake_bit_server.js`, default
`http://127.0.0.1:9803`), overridable with `BIT_URL`, `BIT_KEY`, `BIT_PWD`.

## Usage

```java
import space.osbt.bitsdk.BitClient;

BitClient client = new BitClient("http://127.0.0.1:7777", "<client key>")
        .accessPassword("<access password>") // required for /api/* (not for /v1/*, /mcp)
        .timeoutMs(30000);                   // per-request timeout, default 30000

client.health();                                  // {ok=true, version=0.5.2, ...}
client.listTools();                               // {tools=[...]}
client.registerTool(Map.of(                       // {tool={...}}
        "name", "weather",
        "description", "query weather",
        "parameters", Map.of("type", "object", "properties", Map.of()),
        "url", "http://127.0.0.1:9000/hook"));
client.invokeTool("t_1", Map.of("city", "SF"));   // {result=...}
client.removeTool("t_1");                         // {removed=t_1}
client.chat("hello");                             // {reply=..., messages=[...]}
client.chat(Map.of("message", "hi", "sessionId", "mysession"));
client.audit();                                   // {entries=[...]}
client.debugState();                              // {ai={active={...}}, ...}
client.debugSessions();                           // {sessions=[...]}
client.debugSession("default");                   // {id=..., messages=[...]}
client.debugMcp();                                // {tools=[...]}
client.mcp(Map.of("jsonrpc", "2.0", "id", 1, "method", "tools/list")); // JSON-RPC response
client.models();                                  // {object=list, data=[...]}
client.chatCompletions(Map.of(                    // OpenAI chat.completion (non-streaming)
        "model", "ignored",
        "messages", List.of(Map.of("role", "user", "content", "hi"))));
```

### Streaming (SSE)

```java
String text = client.chatCompletionsStream(
        Map.of("model", "ignored",
               "messages", List.of(Map.of("role", "user", "content", "hi"))),
        (chunk, last) -> {
            if (chunk != null) {
                System.out.print(chunk);        // incremental delta content
            } else {
                System.out.println();           // last parsed chunk; may carry "usage"
            }
        });
// text is the fully assembled assistant message
```

The stream reader buffers bytes and splits only on `\n`, so multi-byte UTF-8
characters split across TCP chunks are decoded correctly.

## Authentication

| Path prefix      | Client Key              | X-Access-Password      |
|------------------|-------------------------|------------------------|
| `GET /api/health`| not needed              | not needed             |
| `/api/*` (rest)  | required                | required               |
| `/v1/*` (OpenAI) | required                | **exempt**             |
| `/mcp`           | required                | **exempt**             |

The Client Key is sent as `Authorization: Bearer <key>`. For environments that
cannot set headers, enable the query-parameter fallback:

```java
BitClient client = new BitClient(url, key).keyInQuery(true); // ?key=<key>
```

## Errors

HTTP responses with status >= 400 throw `BitException`, which carries:

- `getStatus()` — HTTP status code
- `getMessage()` — server-provided error text (`{"error":{...}}` → inner `message`; `{"error":"..."}` → string)
- `getRaw()` — the parsed JSON body
