//go:build ignore

// Runnable usage snippet for the BITSDK Go SDK. Excluded from the module
// build via the `ignore` tag; run it directly with:
//
//	go run examples/main.go
package main

import (
	"context"
	"fmt"
	"time"

	bitsdk "github.com/yxpil/BITSDK/go"
)

func main() {
	client := bitsdk.NewClient("http://127.0.0.1:7777", "your-client-key",
		bitsdk.WithAccessPassword("your-access-password"),
		bitsdk.WithTimeout(30*time.Second),
	)
	ctx := context.Background()

	health, err := client.Health(ctx)
	if err != nil {
		panic(err)
	}
	fmt.Printf("BIT %s healthy=%v\n", health.Version, health.OK)

	tools, err := client.ListTools(ctx)
	if err != nil {
		panic(err)
	}
	fmt.Printf("registered tools visible: %d\n", len(tools))

	// Non-streaming OpenAI-style completion.
	res, err := client.ChatCompletions(ctx, map[string]any{
		"model":    "bit",
		"messages": []map[string]any{{"role": "user", "content": "你好"}},
	})
	if err != nil {
		panic(err)
	}
	fmt.Println("completion:", res)

	// Streaming completion: deltas are printed as they arrive; the final
	// chunk (delta == "", final != nil) may carry usage.
	text, err := client.ChatCompletionsStream(ctx, map[string]any{
		"model":    "bit",
		"messages": []map[string]any{{"role": "user", "content": "讲个笑话"}},
	}, func(delta string, final map[string]any) error {
		fmt.Print(delta)
		if final != nil {
			fmt.Printf("\n[usage] %v\n", final["usage"])
		}
		return nil
	})
	if err != nil {
		panic(err)
	}
	fmt.Println("full text:", text)
}
