# BITSDK — C

C11 client for the BIT HTTP API, built on libcurl and a vendored cJSON (MIT,
in `src/cjson/`). The binding contract is [`../docs/API.md`](../docs/API.md).

## Layout

- `include/bitsdk.h` — public API (all symbols prefixed `bit_`)
- `src/bitsdk.c` — implementation
- `src/cjson/cJSON.c`, `src/cjson/cJSON.h` — vendored cJSON (MIT license header preserved)
- `examples/example.c` — usage example
- `test/test_smoke.c` — smoke test against the fake BIT server
- `Makefile` — build targets

## Build & test

Requires `cc` (clang/gcc), `make` and libcurl (headers ships with the macOS SDK).

```bash
make smoke && ./build/smoke      # build + run the smoke test
make all                         # build libbitsdk.a
make example && ./build/example  # build + run the example
make clean
```

The smoke test runs against `node ../test/fake_bit_server.js` on
`http://127.0.0.1:9803`; override with `BIT_URL`, `BIT_KEY`, `BIT_PWD`.

## Usage

```c
#include "bitsdk.h"

bit_client *c = bit_client_new("http://127.0.0.1:7777", "<client key>");
bit_set_access_password(c, "<access password>"); /* required for /api/* (not /v1/*, /mcp) */
bit_set_timeout_ms(c, 30000);                    /* per-request timeout, default 30000 */
/* bit_set_key_in_query(c, true);                — send the key as ?key= instead */

cJSON *health = bit_health(c);                   /* {"ok":true,...} */
cJSON *tools  = bit_list_tools(c);               /* {"tools":[...]} */
cJSON *reg = bit_register_tool(c, "weather", "query weather",
                               "{\"type\":\"object\",\"properties\":{}}",
                               "http://127.0.0.1:9000/hook");   /* {"tool":{...}} */
cJSON *inv = bit_invoke_tool(c, "t_1", "{\"city\":\"SF\"}");    /* {"result":...} */
cJSON *rem = bit_remove_tool(c, "t_1");                          /* {"removed":"t_1"} */
cJSON *chat = bit_chat(c, "hello", "mysession");                 /* {"reply":...} */
cJSON *state = bit_debug_state(c);
cJSON *models = bit_models(c);
cJSON *cmpl = bit_chat_completions(c,
    "{\"model\":\"ignored\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}");
/* ... cJSON_Delete() every response you receive ... */

/* Raw JSON-RPC (BIT as MCP server) */
cJSON *mcp = bit_mcp(c,
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}");

/* Streaming (SSE): print deltas, collect the full text, inspect final chunk */
char text[4096] = {0};
int rc = bit_chat_completions_stream(c, body_json,
        /* on_delta */ my_callback, /* userdata */ ctx_ptr,
        text, sizeof(text));

bit_client_free(c);
```

### Streaming callback

```c
static void my_callback(const char *chunk, const cJSON *final_chunk, void *ud) {
    if (chunk != NULL) {
        printf("%s", chunk);            /* incremental delta content (UTF-8 safe) */
    } else if (final_chunk != NULL) {
        /* last parsed chunk; may carry "usage" */
    }
}
```

The stream reader accumulates bytes and splits lines only on `\n`, so
multi-byte UTF-8 characters split across TCP chunks are never decoded
partially.

## Authentication

| Path prefix       | Client Key              | X-Access-Password      |
|-------------------|-------------------------|------------------------|
| `GET /api/health` | not needed              | not needed             |
| `/api/*` (rest)   | required                | required               |
| `/v1/*` (OpenAI)  | required                | **exempt**             |
| `/mcp`            | required                | **exempt**             |

The Client Key is sent as `Authorization: Bearer <key>`. For environments
that cannot set headers, enable `bit_set_key_in_query(c, true)` to send it
as `?key=<key>` instead.

## Errors

- **Transport failures** (connect, timeout, invalid JSON) return `NULL` with
  `bit_last_error(c)` set to an English description.
- **HTTP error responses are NOT NULL returns** — the wrapper returns the
  parsed body; check the HTTP status (`bit_request` takes `status_out`) and
  the body's `error` field. `bit_error_message(body, status)` extracts a
  message: `{"error":{"message":...}}` → inner message,
  `{"error":"..."}` → string, otherwise `"HTTP <status>"` (caller frees).
