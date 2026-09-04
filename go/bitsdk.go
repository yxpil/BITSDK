// Package bitsdk is the Go SDK for the BIT HTTP API (see docs/API.md).
// It covers agent tools, chat, audit, debug introspection, raw MCP
// JSON-RPC and the OpenAI-compatible completion endpoints (including
// SSE streaming). Standard library only.
package bitsdk

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"
)

const defaultTimeout = 30 * time.Second

// Error is the single error type returned for non-2xx HTTP responses.
// Message carries the server-provided text from the `error` field
// (string form or OpenAI-style object form); it is never translated.
type Error struct {
	Status  int
	Message string
	Raw     []byte
}

func (e *Error) Error() string {
	return fmt.Sprintf("bit: status %d: %s", e.Status, e.Message)
}

// parseError converts a BIT error body into an *Error. The body's
// `error` field is either a plain string or an object with `message`.
func parseError(status int, body []byte) *Error {
	msg := ""
	var probe struct {
		Err json.RawMessage `json:"error"`
	}
	if err := json.Unmarshal(body, &probe); err == nil && len(probe.Err) > 0 {
		var obj struct {
			Message string `json:"message"`
		}
		var str string
		switch {
		case json.Unmarshal(probe.Err, &obj) == nil && obj.Message != "":
			msg = obj.Message
		case json.Unmarshal(probe.Err, &str) == nil:
			msg = str
		default:
			msg = string(probe.Err)
		}
	}
	if msg == "" {
		msg = strings.TrimSpace(string(body))
	}
	if msg == "" {
		msg = http.StatusText(status)
	}
	return &Error{Status: status, Message: msg, Raw: body}
}

// Client talks to a BIT instance over HTTP.
type Client struct {
	baseURL        string
	clientKey      string
	accessPassword string
	httpClient     *http.Client
	keyInQuery     bool
}

// Option configures a Client.
type Option func(*Client)

// WithAccessPassword sets the access password, sent as X-Access-Password
// on every request. Required for /api/* (except /api/health); /v1/* and
// /mcp are password-exempt but sending it is harmless.
func WithAccessPassword(p string) Option {
	return func(c *Client) { c.accessPassword = p }
}

// WithTimeout sets a per-request timeout (default 30s).
func WithTimeout(d time.Duration) Option {
	return func(c *Client) { c.httpClient = &http.Client{Timeout: d} }
}

// WithHTTPClient supplies a custom *http.Client (overrides WithTimeout).
func WithHTTPClient(hc *http.Client) Option {
	return func(c *Client) { c.httpClient = hc }
}

// WithKeyInQuery sends the Client Key as ?key=<key> instead of the
// Authorization header, for environments that cannot set headers.
func WithKeyInQuery(v bool) Option {
	return func(c *Client) { c.keyInQuery = v }
}

// NewClient creates a Client for the BIT instance at baseUrl.
func NewClient(baseUrl, clientKey string, opts ...Option) *Client {
	c := &Client{
		baseURL:    strings.TrimRight(baseUrl, "/"),
		clientKey:  clientKey,
		httpClient: &http.Client{Timeout: defaultTimeout},
	}
	for _, opt := range opts {
		opt(c)
	}
	return c
}

// newRequest builds an authenticated request. Body may be nil.
func (c *Client) newRequest(ctx context.Context, method, path string, body any) (*http.Request, error) {
	var reader io.Reader
	if body != nil {
		b, err := json.Marshal(body)
		if err != nil {
			return nil, fmt.Errorf("bit: encode request: %w", err)
		}
		reader = bytes.NewReader(b)
	}
	u, err := url.Parse(c.baseURL + path)
	if err != nil {
		return nil, fmt.Errorf("bit: bad base url: %w", err)
	}
	if c.keyInQuery {
		q := u.Query()
		q.Set("key", c.clientKey)
		u.RawQuery = q.Encode()
	}
	req, err := http.NewRequestWithContext(ctx, method, u.String(), reader)
	if err != nil {
		return nil, fmt.Errorf("bit: build request: %w", err)
	}
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	if !c.keyInQuery {
		req.Header.Set("Authorization", "Bearer "+c.clientKey)
	}
	if c.accessPassword != "" {
		req.Header.Set("X-Access-Password", c.accessPassword)
	}
	return req, nil
}

// do performs a JSON request and returns the raw response body.
func (c *Client) do(ctx context.Context, method, path string, body any) ([]byte, error) {
	req, err := c.newRequest(ctx, method, path, body)
	if err != nil {
		return nil, err
	}
	resp, err := c.httpClient.Do(req)
	if err != nil {
		return nil, fmt.Errorf("bit: request %s %s: %w", method, path, err)
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, fmt.Errorf("bit: read response %s %s: %w", method, path, err)
	}
	if resp.StatusCode >= 400 {
		return nil, parseError(resp.StatusCode, data)
	}
	return data, nil
}

func decodeJSON(data []byte, out any) error {
	if err := json.Unmarshal(data, out); err != nil {
		return fmt.Errorf("bit: decode response: %w", err)
	}
	return nil
}

// HealthResult is the response of GET /api/health.
type HealthResult struct {
	OK      bool   `json:"ok"`
	Version string `json:"version"`
}

// Health checks that BIT is up. No authentication required.
func (c *Client) Health(ctx context.Context) (HealthResult, error) {
	data, err := c.do(ctx, http.MethodGet, "/api/health", nil)
	if err != nil {
		return HealthResult{}, err
	}
	var out HealthResult
	err = decodeJSON(data, &out)
	return out, err
}

// ToolKind describes how a tool is executed.
type ToolKind struct {
	Type string `json:"type"` // builtin | remote | script | interpreter | mcp
	URL  string `json:"url,omitempty"`
}

// ToolDef mirrors the BIT tool definition.
type ToolDef struct {
	ID          string          `json:"id"`
	Name        string          `json:"name"`
	Description string          `json:"description,omitempty"`
	Parameters  json.RawMessage `json:"parameters,omitempty"`
	Kind        ToolKind        `json:"kind"`
	CreatedBy   string          `json:"created_by,omitempty"`
	CreatedAt   string          `json:"created_at,omitempty"`
	Enabled     bool            `json:"enabled"`
}

// ListTools returns all registered tools.
func (c *Client) ListTools(ctx context.Context) ([]ToolDef, error) {
	data, err := c.do(ctx, http.MethodGet, "/api/tools", nil)
	if err != nil {
		return nil, err
	}
	var env struct {
		Tools []ToolDef `json:"tools"`
	}
	if err := decodeJSON(data, &env); err != nil {
		return nil, err
	}
	return env.Tools, nil
}

// RegisterToolInput is the request body of POST /api/tools.
type RegisterToolInput struct {
	Name        string          `json:"name"`
	Description string          `json:"description,omitempty"`
	Parameters  json.RawMessage `json:"parameters,omitempty"`
	URL         string          `json:"url"`
}

// RegisterTool registers a remote tool; BIT POSTs tool calls to Input.URL.
func (c *Client) RegisterTool(ctx context.Context, in RegisterToolInput) (ToolDef, error) {
	data, err := c.do(ctx, http.MethodPost, "/api/tools", in)
	if err != nil {
		return ToolDef{}, err
	}
	var env struct {
		Tool ToolDef `json:"tool"`
	}
	if err := decodeJSON(data, &env); err != nil {
		return ToolDef{}, err
	}
	return env.Tool, nil
}

// RemoveTool deletes a tool by id and returns the removed id.
func (c *Client) RemoveTool(ctx context.Context, id string) (string, error) {
	data, err := c.do(ctx, http.MethodDelete, "/api/tools/"+url.PathEscape(id), nil)
	if err != nil {
		return "", err
	}
	var env struct {
		Removed string `json:"removed"`
	}
	if err := decodeJSON(data, &env); err != nil {
		return "", err
	}
	return env.Removed, nil
}

// InvokeTool runs a tool by id with the given params and returns the
// tool's own raw JSON result.
func (c *Client) InvokeTool(ctx context.Context, id string, params map[string]any) (json.RawMessage, error) {
	if params == nil {
		params = map[string]any{}
	}
	data, err := c.do(ctx, http.MethodPost, "/api/tools/"+url.PathEscape(id)+"/invoke", map[string]any{"params": params})
	if err != nil {
		return nil, err
	}
	var env struct {
		Result json.RawMessage `json:"result"`
	}
	if err := decodeJSON(data, &env); err != nil {
		return nil, err
	}
	return env.Result, nil
}

// ChatInput is the request body of POST /api/chat.
type ChatInput struct {
	Message   string   `json:"message"`
	SessionID string   `json:"session_id,omitempty"`
	Images    []string `json:"images,omitempty"`
}

// ChatResult is the response of POST /api/chat: the last assistant
// message plus the full session message list.
type ChatResult struct {
	Reply    string            `json:"reply"`
	Messages []json.RawMessage `json:"messages"`
}

// Chat runs one full agent turn (BIT may call tools/MCP internally).
func (c *Client) Chat(ctx context.Context, in ChatInput) (ChatResult, error) {
	data, err := c.do(ctx, http.MethodPost, "/api/chat", in)
	if err != nil {
		return ChatResult{}, err
	}
	var out ChatResult
	err = decodeJSON(data, &out)
	return out, err
}

// AuditEntry is one audit-log record.
type AuditEntry struct {
	TS     string         `json:"ts"`
	Actor  string         `json:"actor"`
	Action string         `json:"action"`
	Target string         `json:"target"`
	Detail map[string]any `json:"detail"`
	OK     bool           `json:"ok"`
}

// Audit returns recent audit entries (newest first).
func (c *Client) Audit(ctx context.Context) ([]AuditEntry, error) {
	data, err := c.do(ctx, http.MethodGet, "/api/audit", nil)
	if err != nil {
		return nil, err
	}
	var env struct {
		Entries []AuditEntry `json:"entries"`
	}
	if err := decodeJSON(data, &env); err != nil {
		return nil, err
	}
	return env.Entries, nil
}

// DebugState returns the read-only debug snapshot of GET /api/debug/state.
// The field set is informational; treat unknown fields as optional.
func (c *Client) DebugState(ctx context.Context) (map[string]any, error) {
	data, err := c.do(ctx, http.MethodGet, "/api/debug/state", nil)
	if err != nil {
		return nil, err
	}
	var out map[string]any
	err = decodeJSON(data, &out)
	return out, err
}

// SessionSummary is one entry of GET /api/debug/sessions.
type SessionSummary struct {
	ID       string `json:"id"`
	Title    string `json:"title,omitempty"`
	Messages int    `json:"messages"`
	TS       string `json:"ts,omitempty"`
}

// DebugSessions lists all sessions.
func (c *Client) DebugSessions(ctx context.Context) ([]SessionSummary, error) {
	data, err := c.do(ctx, http.MethodGet, "/api/debug/sessions", nil)
	if err != nil {
		return nil, err
	}
	var env struct {
		Sessions []SessionSummary `json:"sessions"`
	}
	if err := decodeJSON(data, &env); err != nil {
		return nil, err
	}
	return env.Sessions, nil
}

// DebugSession returns one full session (id + messages) or a 404 error.
func (c *Client) DebugSession(ctx context.Context, id string) (map[string]any, error) {
	data, err := c.do(ctx, http.MethodGet, "/api/debug/sessions/"+url.PathEscape(id), nil)
	if err != nil {
		return nil, err
	}
	var out map[string]any
	err = decodeJSON(data, &out)
	return out, err
}

// DebugMCP returns the MCP tools exposed to the agent.
func (c *Client) DebugMCP(ctx context.Context) (map[string]any, error) {
	data, err := c.do(ctx, http.MethodGet, "/api/debug/mcp", nil)
	if err != nil {
		return nil, err
	}
	var out map[string]any
	err = decodeJSON(data, &out)
	return out, err
}

// MCP posts a raw JSON-RPC 2.0 request to POST /mcp (BIT acts as MCP
// server) and returns the full JSON-RPC response map, including any
// `error` member. No response-level unwrapping is performed.
func (c *Client) MCP(ctx context.Context, payload map[string]any) (map[string]any, error) {
	data, err := c.do(ctx, http.MethodPost, "/mcp", payload)
	if err != nil {
		return nil, err
	}
	var out map[string]any
	err = decodeJSON(data, &out)
	return out, err
}

// Model is one entry of GET /v1/models.
type Model struct {
	ID      string `json:"id"`
	Object  string `json:"object"`
	Created int64  `json:"created"`
	OwnedBy string `json:"owned_by"`
}

// ModelsResult is the response of GET /v1/models.
type ModelsResult struct {
	Object string  `json:"object"`
	Data   []Model `json:"data"`
}

// Models lists the OpenAI-compatible models BIT exposes.
func (c *Client) Models(ctx context.Context) (ModelsResult, error) {
	data, err := c.do(ctx, http.MethodGet, "/v1/models", nil)
	if err != nil {
		return ModelsResult{}, err
	}
	var out ModelsResult
	err = decodeJSON(data, &out)
	return out, err
}

// ChatCompletions posts a standard OpenAI chat.completions body
// (non-streaming) and returns the raw chat.completion object.
func (c *Client) ChatCompletions(ctx context.Context, body map[string]any) (map[string]any, error) {
	data, err := c.do(ctx, http.MethodPost, "/v1/chat/completions", body)
	if err != nil {
		return nil, err
	}
	var out map[string]any
	err = decodeJSON(data, &out)
	return out, err
}

// ChatCompletionsStream posts the OpenAI body with stream:true and reads
// the SSE response. It calls onDelta for every content chunk with
// (delta, nil), and exactly once for the final chunk (finish_reason set,
// may carry usage) with ("", finalChunk). It returns the assembled full
// text. Streaming stops at `data: [DONE]`.
func (c *Client) ChatCompletionsStream(ctx context.Context, body map[string]any, onDelta func(delta string, final map[string]any) error) (string, error) {
	payload := make(map[string]any, len(body)+1)
	for k, v := range body {
		payload[k] = v
	}
	payload["stream"] = true

	req, err := c.newRequest(ctx, http.MethodPost, "/v1/chat/completions", payload)
	if err != nil {
		return "", err
	}
	req.Header.Set("Accept", "text/event-stream")

	resp, err := c.httpClient.Do(req)
	if err != nil {
		return "", fmt.Errorf("bit: request POST /v1/chat/completions: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode >= 400 {
		data, readErr := io.ReadAll(resp.Body)
		if readErr != nil {
			return "", fmt.Errorf("bit: read error response: %w", readErr)
		}
		return "", parseError(resp.StatusCode, data)
	}

	var full strings.Builder
	sc := bufio.NewScanner(resp.Body)
	// Enlarged buffer: SSE lines are small, but never cap mid-line.
	sc.Buffer(make([]byte, 0, 64*1024), 4*1024*1024)

	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line == "" || !strings.HasPrefix(line, "data:") {
			continue // skip blank lines and non-data fields
		}
		data := strings.TrimSpace(strings.TrimPrefix(line, "data:"))
		if data == "" {
			continue
		}
		if data == "[DONE]" {
			break
		}
		var chunk map[string]any
		if err := json.Unmarshal([]byte(data), &chunk); err != nil {
			return full.String(), fmt.Errorf("bit: decode SSE chunk: %w", err)
		}
		delta, isFinal := chunkContent(chunk)
		if delta != "" {
			full.WriteString(delta)
			if onDelta != nil {
				if err := onDelta(delta, nil); err != nil {
					return full.String(), err
				}
			}
		}
		if isFinal {
			if onDelta != nil {
				if err := onDelta("", chunk); err != nil {
					return full.String(), err
				}
			}
		}
	}
	if err := sc.Err(); err != nil {
		return full.String(), fmt.Errorf("bit: read SSE stream: %w", err)
	}
	return full.String(), nil
}

// chunkContent extracts the delta content of a chat.completion.chunk and
// reports whether the chunk is the final one (finish_reason present and
// non-null).
func chunkContent(chunk map[string]any) (delta string, isFinal bool) {
	choices, ok := chunk["choices"].([]any)
	if !ok || len(choices) == 0 {
		return "", false
	}
	ch, ok := choices[0].(map[string]any)
	if !ok {
		return "", false
	}
	if fr, present := ch["finish_reason"]; present && fr != nil {
		isFinal = true
	}
	if d, ok := ch["delta"].(map[string]any); ok {
		if c, ok := d["content"].(string); ok {
			delta = c
		}
	}
	return delta, isFinal
}
