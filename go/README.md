# BITSDK — Go

Go SDK for the BIT HTTP API. Standard library only, no external
dependencies. The binding contract is [`../docs/API.md`](../docs/API.md).

## Install

```sh
go get github.com/yxpil/BITSDK/go
```

Requires Go 1.22+.

## Authentication

Every request carries two credentials:

- **Client Key** — sent as `Authorization: Bearer <key>` by default, or as
  `?key=<key>` when the client is built with `WithKeyInQuery(true)` (for
  environments that cannot set headers).
- **Access password** — sent as `X-Access-Password: <password>` when
  configured via `WithAccessPassword`.

Which paths require what (see `../docs/API.md`):

| Path prefix        | Client Key | X-Access-Password |
|--------------------|------------|-------------------|
| `GET /api/health`  | not needed | not needed        |
| `/api/*` (rest)    | required   | required          |
| `/v1/*` (OpenAI)   | required   | exempt            |
| `/mcp`             | required   | exempt            |

All non-2xx responses are returned as a single `*bitsdk.Error` carrying
the HTTP `Status`, the server-provided `Message` (parsed from the JSON
`error` field — string or OpenAI-style object — and never translated),
and the `Raw` body.

## Usage

```go
package main

import (
	"context"
	"fmt"

	bitsdk "github.com/yxpil/BITSDK/go"
)

func main() {
	client := bitsdk.NewClient("http://127.0.0.1:7777", "your-client-key",
		bitsdk.WithAccessPassword("your-access-password"), // optional
		bitsdk.WithTimeout(30*time.Second),                // default 30s
	)
	ctx := context.Background()

	health, err := client.Health(ctx)
	if err != nil {
		panic(err)
	}
	fmt.Println("BIT", health.Version, health.OK)

	// Register a remote tool, invoke it, remove it.
	tool, err := client.RegisterTool(ctx, bitsdk.RegisterToolInput{
		Name: "weather",
		Description: "query weather",
		Parameters: json.RawMessage(`{"type":"object","properties":{}}`),
		URL: "http://127.0.0.1:9000/hook",
	})
	if err != nil {
		panic(err)
	}
	result, err := client.InvokeTool(ctx, tool.ID, map[string]any{"city": "shanghai"})
	if err != nil {
		panic(err)
	}
	fmt.Println("tool result:", string(result))
	if _, err := client.RemoveTool(ctx, tool.ID); err != nil {
		panic(err)
	}

	// Agent turn.
	chat, err := client.Chat(ctx, bitsdk.ChatInput{Message: "hello"})
	if err != nil {
		panic(err)
	}
	fmt.Println(chat.Reply)

	// OpenAI-compatible completion, streaming. The callback receives each
	// content delta, then one final call with delta=="" and the last chunk
	// (which may carry usage). The SDK assembles and returns the full text.
	text, err := client.ChatCompletionsStream(ctx, map[string]any{
		"model":    "bit",
		"messages": []map[string]any{{"role": "user", "content": "你好"}},
	}, func(delta string, final map[string]any) error {
		fmt.Print(delta)
		if final != nil {
			fmt.Printf("\nusage: %v\n", final["usage"])
		}
		return nil
	})
	if err != nil {
		panic(err)
	}
	fmt.Println("full:", text)
}
```

## Test

The smoke test runs against the fake BIT server:

```sh
node ../test/fake_bit_server.js   # http://127.0.0.1:9803
cd go && go test -v ./...
```

Environment overrides: `BIT_URL`, `BIT_KEY`, `BIT_PWD`.
