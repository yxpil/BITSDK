package bitsdk

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"strings"
	"sync"
	"testing"
	"time"
)

const (
	defURL = "http://127.0.0.1:9803"
	defKey = "bit_test_key_123456"
	defPwd = "test-pwd-1"
)

func envOr(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

func newTestClient(t *testing.T) *Client {
	t.Helper()
	return NewClient(
		envOr("BIT_URL", defURL),
		envOr("BIT_KEY", defKey),
		WithAccessPassword(envOr("BIT_PWD", defPwd)),
	)
}

func testCtx(t *testing.T) context.Context {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	t.Cleanup(cancel)
	return ctx
}

// TestFakeBIT is the smoke test against test/fake_bit_server.js.
func TestFakeBIT(t *testing.T) {
	if err := pingHealth(); err != nil {
		t.Skipf("fake BIT server not reachable at %s: %v", envOr("BIT_URL", defURL), err)
	}

	t.Run("Health", func(t *testing.T) {
		res, err := newTestClient(t).Health(testCtx(t))
		if err != nil {
			t.Fatalf("Health: %v", err)
		}
		if !res.OK {
			t.Errorf("Health.OK = false, want true")
		}
		if res.Version == "" {
			t.Errorf("Health.Version = empty")
		}
	})

	t.Run("ToolRegisterInvokeRemove", func(t *testing.T) {
		c := newTestClient(t)
		ctx := testCtx(t)
		name := fmt.Sprintf("go_sdk_tool_%d", time.Now().UnixNano())
		tool, err := c.RegisterTool(ctx, RegisterToolInput{
			Name:        name,
			Description: "go sdk smoke tool",
			Parameters:  json.RawMessage(`{"type":"object","properties":{}}`),
			URL:         "http://127.0.0.1:9999/hook",
		})
		if err != nil {
			t.Fatalf("RegisterTool: %v", err)
		}
		if tool.Name != name || tool.ID == "" {
			t.Fatalf("registered tool = %+v, want name %q and non-empty id", tool, name)
		}
		if tool.Kind.Type != "remote" || tool.Kind.URL != "http://127.0.0.1:9999/hook" {
			t.Errorf("tool.Kind = %+v, want remote hook url", tool.Kind)
		}

		raw, err := c.InvokeTool(ctx, tool.ID, map[string]any{"a": 1})
		if err != nil {
			t.Fatalf("InvokeTool: %v", err)
		}
		var result struct {
			Echoed map[string]any `json:"echoed"`
			Via    string         `json:"via"`
		}
		if err := json.Unmarshal(raw, &result); err != nil {
			t.Fatalf("decode invoke result %s: %v", raw, err)
		}
		if result.Via != name {
			t.Errorf("result.via = %q, want %q", result.Via, name)
		}
		if result.Echoed["a"] != float64(1) {
			t.Errorf("result.echoed.a = %v, want 1", result.Echoed["a"])
		}

		removed, err := c.RemoveTool(ctx, tool.ID)
		if err != nil {
			t.Fatalf("RemoveTool: %v", err)
		}
		if removed != tool.ID {
			t.Errorf("RemoveTool = %q, want %q", removed, tool.ID)
		}

		if _, err := c.InvokeTool(ctx, tool.ID, nil); err == nil {
			t.Errorf("InvokeTool after remove: want error, got nil")
		} else {
			var bitErr *Error
			if !errors.As(err, &bitErr) {
				t.Errorf("InvokeTool after remove: error is %T, want *Error", err)
			} else if bitErr.Status != 400 {
				t.Errorf("InvokeTool after remove: status = %d, want 400", bitErr.Status)
			}
		}
	})

	t.Run("Chat", func(t *testing.T) {
		res, err := newTestClient(t).Chat(testCtx(t), ChatInput{Message: "go-sdk-hello"})
		if err != nil {
			t.Fatalf("Chat: %v", err)
		}
		if !strings.HasPrefix(res.Reply, "fake reply to:") {
			t.Errorf("reply = %q, want prefix %q", res.Reply, "fake reply to:")
		}
		if len(res.Messages) == 0 {
			t.Errorf("messages = empty, want session messages")
		}
	})

	t.Run("DebugState", func(t *testing.T) {
		st, err := newTestClient(t).DebugState(testCtx(t))
		if err != nil {
			t.Fatalf("DebugState: %v", err)
		}
		ai, ok := st["ai"].(map[string]any)
		if !ok {
			t.Fatalf("state.ai missing: %v", st)
		}
		active, ok := ai["active"].(map[string]any)
		if !ok {
			t.Fatalf("state.ai.active missing: %v", ai)
		}
		if _, ok := active["model"].(string); !ok || active["model"] == "" {
			t.Errorf("state.ai.active.model = %v, want non-empty string", active["model"])
		}
	})

	t.Run("DebugSessions", func(t *testing.T) {
		sessions, err := newTestClient(t).DebugSessions(testCtx(t))
		if err != nil {
			t.Fatalf("DebugSessions: %v", err)
		}
		if len(sessions) < 1 {
			t.Errorf("sessions = %d, want >= 1", len(sessions))
		}
	})

	t.Run("DebugSessionDefault", func(t *testing.T) {
		sess, err := newTestClient(t).DebugSession(testCtx(t), "default")
		if err != nil {
			t.Fatalf("DebugSession(default): %v", err)
		}
		msgs, ok := sess["messages"].([]any)
		if !ok || len(msgs) == 0 {
			t.Errorf("session.messages = %v, want non-empty array", sess["messages"])
		}
	})

	t.Run("DebugMCP", func(t *testing.T) {
		mcp, err := newTestClient(t).DebugMCP(testCtx(t))
		if err != nil {
			t.Fatalf("DebugMCP: %v", err)
		}
		if _, ok := mcp["tools"].([]any); !ok {
			t.Errorf("debug mcp.tools missing: %v", mcp)
		}
	})

	t.Run("Audit", func(t *testing.T) {
		entries, err := newTestClient(t).Audit(testCtx(t))
		if err != nil {
			t.Fatalf("Audit: %v", err)
		}
		if len(entries) < 1 {
			t.Errorf("audit entries = %d, want >= 1 after auth traffic", len(entries))
		}
	})

	t.Run("MCPInitialize", func(t *testing.T) {
		c := newTestClient(t)
		resp, err := c.MCP(testCtx(t), map[string]any{
			"jsonrpc": "2.0",
			"id":      1,
			"method":  "initialize",
			"params": map[string]any{
				"protocolVersion": "2025-03-26",
				"capabilities":    map[string]any{},
				"clientInfo":      map[string]any{"name": "go-sdk", "version": "0.0.1"},
			},
		})
		if err != nil {
			t.Fatalf("MCP initialize: %v", err)
		}
		result, ok := resp["result"].(map[string]any)
		if !ok {
			t.Fatalf("jsonrpc result missing: %v", resp)
		}
		info, ok := result["serverInfo"].(map[string]any)
		if !ok {
			t.Fatalf("serverInfo missing: %v", result)
		}
		if info["name"] != "fake-bit" {
			t.Errorf("serverInfo.name = %v, want fake-bit", info["name"])
		}
	})

	t.Run("Models", func(t *testing.T) {
		res, err := newTestClient(t).Models(testCtx(t))
		if err != nil {
			t.Fatalf("Models: %v", err)
		}
		if len(res.Data) != 2 {
			t.Errorf("models = %d, want 2", len(res.Data))
		}
		if res.Object != "list" {
			t.Errorf("models.object = %q, want list", res.Object)
		}
	})

	t.Run("ChatCompletions", func(t *testing.T) {
		res, err := newTestClient(t).ChatCompletions(testCtx(t), map[string]any{
			"model":    "fake-model",
			"messages": []map[string]any{{"role": "user", "content": "hi"}},
		})
		if err != nil {
			t.Fatalf("ChatCompletions: %v", err)
		}
		choices, ok := res["choices"].([]any)
		if !ok || len(choices) == 0 {
			t.Fatalf("choices missing: %v", res)
		}
		ch, _ := choices[0].(map[string]any)
		msg, _ := ch["message"].(map[string]any)
		if msg["content"] != "你好，世界!" {
			t.Errorf("content = %v, want 你好，世界!", msg["content"])
		}
	})

	t.Run("ChatCompletionsStream", func(t *testing.T) {
		var mu sync.Mutex
		var parts []string
		var finalChunk map[string]any
		finalCalls := 0

		text, err := newTestClient(t).ChatCompletionsStream(testCtx(t), map[string]any{
			"model":    "fake-model",
			"messages": []map[string]any{{"role": "user", "content": "hi"}},
		}, func(delta string, final map[string]any) error {
			mu.Lock()
			defer mu.Unlock()
			if final != nil {
				finalChunk = final
				finalCalls++
			} else {
				parts = append(parts, delta)
			}
			return nil
		})
		if err != nil {
			t.Fatalf("ChatCompletionsStream: %v", err)
		}
		if text != "你好，世界!" {
			t.Errorf("assembled text = %q, want %q", text, "你好，世界!")
		}
		if got := strings.Join(parts, ""); got != text {
			t.Errorf("onDelta deltas = %q, want sum == %q (multi-byte chunks must not be corrupted)", got, text)
		}
		if finalCalls != 1 {
			t.Errorf("final onDelta calls = %d, want 1", finalCalls)
		}
		if finalChunk == nil || finalChunk["usage"] == nil {
			t.Errorf("final chunk = %v, want one carrying usage", finalChunk)
		}
	})

	t.Run("WrongKey401", func(t *testing.T) {
		c := NewClient(envOr("BIT_URL", defURL), "definitely-wrong-key", WithAccessPassword(envOr("BIT_PWD", defPwd)))
		_, err := c.ListTools(testCtx(t))
		if err == nil {
			t.Fatalf("ListTools with wrong key: want error, got nil")
		}
		var bitErr *Error
		if !errors.As(err, &bitErr) {
			t.Fatalf("error type = %T, want *Error", err)
		}
		if bitErr.Status != 401 {
			t.Errorf("status = %d, want 401", bitErr.Status)
		}
		if bitErr.Message == "" {
			t.Errorf("message = empty, want server-provided text")
		}
		if !strings.HasPrefix(bitErr.Error(), "bit: status 401:") {
			t.Errorf("Error() = %q, want prefix %q", bitErr.Error(), "bit: status 401:")
		}
		if len(bitErr.Raw) == 0 {
			t.Errorf("raw body = empty, want original bytes")
		}
	})

	t.Run("MissingPassword401", func(t *testing.T) {
		c := NewClient(envOr("BIT_URL", defURL), envOr("BIT_KEY", defKey)) // no WithAccessPassword
		_, err := c.ListTools(testCtx(t))
		if err == nil {
			t.Fatalf("ListTools without password: want error, got nil")
		}
		var bitErr *Error
		if !errors.As(err, &bitErr) {
			t.Fatalf("error type = %T, want *Error", err)
		}
		if bitErr.Status != 401 {
			t.Errorf("status = %d, want 401", bitErr.Status)
		}
	})

	t.Run("KeyInQuery", func(t *testing.T) {
		c := NewClient(envOr("BIT_URL", defURL), envOr("BIT_KEY", defKey),
			WithAccessPassword(envOr("BIT_PWD", defPwd)), WithKeyInQuery(true))
		tools, err := c.ListTools(testCtx(t))
		if err != nil {
			t.Fatalf("ListTools with key in query: %v", err)
		}
		_ = tools // do not assert on the shared global list length

		// A wrong key via query must still be rejected, proving the key
		// is actually transported through the query parameter.
		bad := NewClient(envOr("BIT_URL", defURL), "definitely-wrong-key",
			WithAccessPassword(envOr("BIT_PWD", defPwd)), WithKeyInQuery(true))
		if _, err := bad.ListTools(testCtx(t)); err == nil {
			t.Fatalf("ListTools with wrong key in query: want error, got nil")
		} else {
			var bitErr *Error
			if !errors.As(err, &bitErr) || bitErr.Status != 401 {
				t.Errorf("wrong key in query: got %v, want *Error status 401", err)
			}
		}
	})
}

// pingHealth checks the fake server is reachable without auth.
func pingHealth() error {
	c := NewClient(envOr("BIT_URL", defURL), "")
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	_, err := c.Health(ctx)
	return err
}
