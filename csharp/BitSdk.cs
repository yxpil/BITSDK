// BitSdk.cs — single-file C# SDK for the BIT HTTP API.
//
// Contract: ../docs/API.md (BIT v0.5.2). Key rules:
//   * GET /api/health needs no auth.
//   * /api/*      needs Client Key + X-Access-Password header.
//   * /v1/*, /mcp need Client Key only (access password exempt).
//   * Client Key is sent as "Authorization: Bearer <key>" or "?key=<key>".
//   * Errors are JSON with an "error" field: a plain string, or an
//     OpenAI-style object with a "message" member.

using System;
using System.Collections.Generic;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;

namespace BitSdk;

/// <summary>
/// Error thrown for every failed BIT API call. Carries the HTTP status
/// (0 for transport failures) and the raw response body.
/// </summary>
public class BitException : Exception
{
    public int Status;
    public string Raw;

    public BitException(int status, string raw, string message) : base(message)
    {
        Status = status;
        Raw = raw ?? "";
    }

    /// <summary>Builds the exception from an HTTP error response.</summary>
    public static BitException From(int status, string raw)
    {
        var server = ExtractMessage(raw) ?? $"HTTP {status}";
        return new BitException(status, raw, $"BIT API error {status}: {server}");
    }

    /// <summary>Transport-level failure (unreachable server, timeout, ...).</summary>
    public static BitException Transport(string message)
        => new BitException(0, "", $"BIT request failed: {message}");

    /// <summary>
    /// Extracts the server message from a BIT error body:
    /// {"error":"..."} -> the string; {"error":{"message":"..."}} -> .error.message.
    /// Returns null when the body is not a parseable BIT error object.
    /// </summary>
    public static string? ExtractMessage(string? raw)
    {
        if (string.IsNullOrWhiteSpace(raw)) return null;
        try
        {
            using var doc = JsonDocument.Parse(raw);
            var root = doc.RootElement;
            if (root.ValueKind != JsonValueKind.Object ||
                !root.TryGetProperty("error", out var err))
            {
                return null;
            }
            if (err.ValueKind == JsonValueKind.String) return err.GetString();
            if (err.ValueKind == JsonValueKind.Object &&
                err.TryGetProperty("message", out var msg) &&
                msg.ValueKind == JsonValueKind.String)
            {
                return msg.GetString();
            }
            return err.GetRawText();
        }
        catch (JsonException)
        {
            return null;
        }
    }
}

/// <summary>
/// Client for the BIT HTTP API (see ../docs/API.md).
/// Construct with base URL and Client Key, then optionally configure via the
/// fluent With* methods. All methods throw <see cref="BitException"/> on
/// failure; async variants are primary, sync wrappers are provided for
/// convenience (do not call them from UI threads).
/// </summary>
public sealed class BitClient : IDisposable
{
    private readonly HttpClient _http;
    private readonly string _baseUrl;
    private readonly string _clientKey;
    private string _accessPassword = "";
    private bool _keyInQuery;
    private bool _disposed;

    public BitClient(string baseUrl, string clientKey)
    {
        _baseUrl = (baseUrl ?? "").TrimEnd('/');
        _clientKey = clientKey ?? "";
        _http = new HttpClient { Timeout = TimeSpan.FromSeconds(30) };
    }

    /// <summary>Sets the access password (X-Access-Password on /api/* paths).</summary>
    public BitClient WithAccessPassword(string password)
    {
        _accessPassword = password ?? "";
        return this;
    }

    /// <summary>Sets the per-request timeout (default 30 seconds).</summary>
    public BitClient WithTimeout(TimeSpan timeout)
    {
        _http.Timeout = timeout;
        return this;
    }

    /// <summary>
    /// Sends the Client Key as "?key=&lt;key&gt;" instead of the Authorization
    /// header (for environments that cannot set headers).
    /// </summary>
    public BitClient WithKeyInQuery(bool value = true)
    {
        _keyInQuery = value;
        return this;
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _http.Dispose();
    }

    // -------------------------------------------------------------- plumbing

    private string BuildUrl(string path)
    {
        var url = _baseUrl + path;
        if (_keyInQuery && _clientKey.Length > 0)
        {
            url += (url.Contains('?') ? "&" : "?") + "key=" + Uri.EscapeDataString(_clientKey);
        }
        return url;
    }

    private void AddAuth(HttpRequestMessage req, bool needPassword)
    {
        if (!_keyInQuery && _clientKey.Length > 0)
        {
            req.Headers.Authorization = new AuthenticationHeaderValue("Bearer", _clientKey);
        }
        if (needPassword)
        {
            req.Headers.Add("X-Access-Password", _accessPassword);
        }
    }

    // One JSON request -> parsed JsonElement (detached from its JsonDocument).
    // needPassword: true for /api/* endpoints, false for /api/health, /v1/*, /mcp.
    private async Task<JsonElement> SendAsync(
        HttpMethod method, string path, string? jsonBody, bool needPassword,
        CancellationToken ct = default)
    {
        using var req = new HttpRequestMessage(method, BuildUrl(path));
        AddAuth(req, needPassword);
        if (jsonBody != null)
        {
            req.Content = new StringContent(jsonBody, Encoding.UTF8, "application/json");
        }

        HttpResponseMessage resp;
        try
        {
            resp = await _http.SendAsync(req, ct).ConfigureAwait(false);
        }
        catch (HttpRequestException e)
        {
            throw BitException.Transport(e.Message);
        }
        catch (TaskCanceledException) when (!ct.IsCancellationRequested)
        {
            throw BitException.Transport("request timed out");
        }

        using (resp)
        {
            var raw = await resp.Content.ReadAsStringAsync(ct).ConfigureAwait(false);
            var status = (int)resp.StatusCode;
            if (status < 200 || status >= 300)
            {
                throw BitException.From(status, raw);
            }
            if (string.IsNullOrWhiteSpace(raw))
            {
                return JsonDocument.Parse("{}").RootElement.Clone();
            }
            return JsonDocument.Parse(raw).RootElement.Clone();
        }
    }

    private static JsonElement OrEmptyObject(JsonElement e)
        => e.ValueKind is JsonValueKind.Undefined or JsonValueKind.Null
            ? JsonDocument.Parse("{}").RootElement.Clone()
            : e;

    private static string RawJson(JsonElement e)
        => e.ValueKind == JsonValueKind.Undefined ? "{}" : e.GetRawText();

    private static JsonElement ParseJson(string json)
        => JsonDocument.Parse(string.IsNullOrWhiteSpace(json) ? "{}" : json).RootElement.Clone();

    // ---------------------------------------------------------------- /api/*

    /// <summary>GET /api/health — no auth required.</summary>
    public Task<JsonElement> HealthAsync(CancellationToken ct = default)
        => SendAsync(HttpMethod.Get, "/api/health", null, needPassword: false, ct);

    /// <summary>GET /api/tools.</summary>
    public Task<JsonElement> ListToolsAsync(CancellationToken ct = default)
        => SendAsync(HttpMethod.Get, "/api/tools", null, needPassword: true, ct);

    /// <summary>POST /api/tools — register a remote tool. 409 if the name exists.</summary>
    public Task<JsonElement> RegisterToolAsync(
        string name, string description, JsonElement parameters, string url,
        CancellationToken ct = default)
    {
        var body = JsonSerializer.Serialize(new
        {
            name,
            description,
            parameters = OrEmptyObject(parameters),
            url,
        });
        return SendAsync(HttpMethod.Post, "/api/tools", body, needPassword: true, ct);
    }

    /// <summary>POST /api/tools with the parameter schema given as raw JSON text.</summary>
    public Task<JsonElement> RegisterToolAsync(
        string name, string description, string parametersJson, string url,
        CancellationToken ct = default)
        => RegisterToolAsync(name, description, ParseJson(parametersJson), url, ct);

    /// <summary>DELETE /api/tools/{id}.</summary>
    public Task<JsonElement> RemoveToolAsync(string id, CancellationToken ct = default)
        => SendAsync(HttpMethod.Delete, "/api/tools/" + Uri.EscapeDataString(id), null, needPassword: true, ct);

    /// <summary>POST /api/tools/{id}/invoke. Missing parameters are sent as {}.</summary>
    public Task<JsonElement> InvokeToolAsync(string id, JsonElement? parameters = null, CancellationToken ct = default)
    {
        var body = JsonSerializer.Serialize(new { @params = OrEmptyObject(parameters ?? default) });
        var path = "/api/tools/" + Uri.EscapeDataString(id) + "/invoke";
        return SendAsync(HttpMethod.Post, path, body, needPassword: true, ct);
    }

    /// <summary>POST /api/chat — run one agent turn. sessionId/images optional.</summary>
    public Task<JsonElement> ChatAsync(
        string message, string? sessionId = null, string[]? images = null,
        CancellationToken ct = default)
    {
        var body = new Dictionary<string, object?>
        {
            ["message"] = message,
        };
        if (sessionId != null) body["session_id"] = sessionId;
        if (images != null) body["images"] = images;
        return SendAsync(HttpMethod.Post, "/api/chat", JsonSerializer.Serialize(body), needPassword: true, ct);
    }

    /// <summary>GET /api/audit.</summary>
    public Task<JsonElement> AuditAsync(CancellationToken ct = default)
        => SendAsync(HttpMethod.Get, "/api/audit", null, needPassword: true, ct);

    /// <summary>GET /api/debug/state — read-only debug snapshot.</summary>
    public Task<JsonElement> DebugStateAsync(CancellationToken ct = default)
        => SendAsync(HttpMethod.Get, "/api/debug/state", null, needPassword: true, ct);

    /// <summary>GET /api/debug/sessions.</summary>
    public Task<JsonElement> DebugSessionsAsync(CancellationToken ct = default)
        => SendAsync(HttpMethod.Get, "/api/debug/sessions", null, needPassword: true, ct);

    /// <summary>GET /api/debug/sessions/{id}.</summary>
    public Task<JsonElement> DebugSessionAsync(string id, CancellationToken ct = default)
        => SendAsync(HttpMethod.Get, "/api/debug/sessions/" + Uri.EscapeDataString(id), null, needPassword: true, ct);

    /// <summary>GET /api/debug/mcp.</summary>
    public Task<JsonElement> DebugMcpAsync(CancellationToken ct = default)
        => SendAsync(HttpMethod.Get, "/api/debug/mcp", null, needPassword: true, ct);

    // ---------------------------------------------------------- /mcp, /v1/*

    /// <summary>POST /mcp — raw JSON-RPC 2.0 request. Access password exempt.</summary>
    public Task<JsonElement> McpAsync(JsonElement jsonRpcPayload, CancellationToken ct = default)
        => SendAsync(HttpMethod.Post, "/mcp", RawJson(jsonRpcPayload), needPassword: false, ct);

    /// <summary>POST /mcp with the JSON-RPC payload as raw JSON text.</summary>
    public Task<JsonElement> McpAsync(string jsonRpcJson, CancellationToken ct = default)
        => SendAsync(HttpMethod.Post, "/mcp", jsonRpcJson, needPassword: false, ct);

    /// <summary>GET /v1/models — OpenAI-compatible model list. Password exempt.</summary>
    public Task<JsonElement> ModelsAsync(CancellationToken ct = default)
        => SendAsync(HttpMethod.Get, "/v1/models", null, needPassword: false, ct);

    /// <summary>POST /v1/chat/completions — non-streaming OpenAI call. Password exempt.</summary>
    public Task<JsonElement> ChatCompletionsAsync(JsonElement body, CancellationToken ct = default)
        => SendAsync(HttpMethod.Post, "/v1/chat/completions", RawJson(body), needPassword: false, ct);

    /// <summary>POST /v1/chat/completions with the request body as raw JSON text.</summary>
    public Task<JsonElement> ChatCompletionsAsync(string bodyJson, CancellationToken ct = default)
        => SendAsync(HttpMethod.Post, "/v1/chat/completions", bodyJson, needPassword: false, ct);

    /// <summary>
    /// POST /v1/chat/completions with stream:true. Reads the SSE response with
    /// HttpCompletionOption.ResponseHeadersRead; lines are split only on '\n'
    /// (UTF-8 multi-byte sequences split across TCP chunks are decoded safely).
    /// onDelta(chunkText, null) fires per content chunk; onDelta(null,
    /// finalChunk) fires once for the last parsed chunk before "data: [DONE]"
    /// (it may carry "usage"). Returns the assembled full text.
    /// </summary>
    public async Task<string> ChatCompletionsStreamAsync(
        JsonElement body, Func<string?, JsonElement?, Task> onDelta, CancellationToken ct = default)
    {
        using var req = new HttpRequestMessage(HttpMethod.Post, BuildUrl("/v1/chat/completions"));
        AddAuth(req, needPassword: false); // /v1/* is password exempt
        req.Content = new StringContent(WithStreamTrue(body), Encoding.UTF8, "application/json");

        HttpResponseMessage resp;
        try
        {
            resp = await _http.SendAsync(req, HttpCompletionOption.ResponseHeadersRead, ct).ConfigureAwait(false);
        }
        catch (HttpRequestException e)
        {
            throw BitException.Transport(e.Message);
        }
        catch (TaskCanceledException) when (!ct.IsCancellationRequested)
        {
            throw BitException.Transport("request timed out");
        }

        using (resp)
        {
            var status = (int)resp.StatusCode;
            if (status < 200 || status >= 300)
            {
                var errRaw = await resp.Content.ReadAsStringAsync(ct).ConfigureAwait(false);
                throw BitException.From(status, errRaw);
            }
            var stream = await resp.Content.ReadAsStreamAsync(ct).ConfigureAwait(false);
            using var reader = new StreamReader(stream, Encoding.UTF8);
            return await ReadSseAsync(reader, onDelta, ct).ConfigureAwait(false);
        }
    }

    private static string WithStreamTrue(JsonElement body)
    {
        var node = JsonNode.Parse(RawJson(body));
        if (node is JsonObject obj)
        {
            obj["stream"] = true;
        }
        return node?.ToJsonString() ?? "{\"stream\":true}";
    }

    private static async Task<string> ReadSseAsync(
        StreamReader reader, Func<string?, JsonElement?, Task> onDelta, CancellationToken ct)
    {
        var full = new StringBuilder();
        var one = new char[1];
        JsonElement? lastChunk = null;
        var line = new StringBuilder();

        while (true)
        {
            ct.ThrowIfCancellationRequested();
            line.Clear();
            bool hasLine = false;

            // Read chars until '\n' only — never split mid multi-byte sequence,
            // because StreamReader's UTF-8 decoder keeps partial sequences.
            while (true)
            {
                var n = await reader.ReadAsync(one.AsMemory(), ct).ConfigureAwait(false);
                if (n <= 0)
                {
                    hasLine = line.Length > 0; // final unterminated line
                    break;
                }
                hasLine = true;
                if (one[0] == '\n') break;
                line.Append(one[0]);
            }
            if (!hasLine) break;

            var text = line.ToString();
            if (!text.StartsWith("data:", StringComparison.Ordinal)) continue;

            var data = text.Substring(5).Trim();
            if (data == "[DONE]") break;

            JsonElement chunk;
            try
            {
                chunk = JsonDocument.Parse(data).RootElement.Clone();
            }
            catch (JsonException)
            {
                continue; // tolerate malformed keep-alive/comment lines
            }
            lastChunk = chunk;

            string? content = null;
            if (chunk.TryGetProperty("choices", out var choices) &&
                choices.ValueKind == JsonValueKind.Array &&
                choices.GetArrayLength() > 0)
            {
                var first = choices[0];
                if (first.ValueKind == JsonValueKind.Object &&
                    first.TryGetProperty("delta", out var delta) &&
                    delta.ValueKind == JsonValueKind.Object &&
                    delta.TryGetProperty("content", out var c) &&
                    c.ValueKind == JsonValueKind.String)
                {
                    content = c.GetString();
                }
            }
            if (content != null)
            {
                full.Append(content);
                if (onDelta != null) await onDelta(content, null).ConfigureAwait(false);
            }
        }

        if (lastChunk.HasValue && onDelta != null)
        {
            await onDelta(null, lastChunk.Value).ConfigureAwait(false);
        }
        return full.ToString();
    }

    // -------------------------------------------------------- sync wrappers

    /// <summary>Blocking variant of <see cref="HealthAsync"/>. Not for UI threads.</summary>
    public JsonElement Health() => HealthAsync().GetAwaiter().GetResult();

    /// <summary>Blocking variant of <see cref="ListToolsAsync"/>. Not for UI threads.</summary>
    public JsonElement ListTools() => ListToolsAsync().GetAwaiter().GetResult();

    /// <summary>Blocking variant of <see cref="RegisterToolAsync(string,string,JsonElement,string,System.Threading.CancellationToken)"/>.</summary>
    public JsonElement RegisterTool(string name, string description, JsonElement parameters, string url)
        => RegisterToolAsync(name, description, parameters, url).GetAwaiter().GetResult();

    /// <summary>Blocking variant of <see cref="RegisterToolAsync(string,string,string,string,System.Threading.CancellationToken)"/>.</summary>
    public JsonElement RegisterTool(string name, string description, string parametersJson, string url)
        => RegisterToolAsync(name, description, parametersJson, url).GetAwaiter().GetResult();

    /// <summary>Blocking variant of <see cref="RemoveToolAsync"/>. Not for UI threads.</summary>
    public JsonElement RemoveTool(string id) => RemoveToolAsync(id).GetAwaiter().GetResult();

    /// <summary>Blocking variant of <see cref="InvokeToolAsync"/>. Not for UI threads.</summary>
    public JsonElement InvokeTool(string id, JsonElement? parameters = null)
        => InvokeToolAsync(id, parameters).GetAwaiter().GetResult();

    /// <summary>Blocking variant of <see cref="ChatAsync"/>. Not for UI threads.</summary>
    public JsonElement Chat(string message, string? sessionId = null, string[]? images = null)
        => ChatAsync(message, sessionId, images).GetAwaiter().GetResult();

    /// <summary>Blocking variant of <see cref="AuditAsync"/>. Not for UI threads.</summary>
    public JsonElement Audit() => AuditAsync().GetAwaiter().GetResult();

    /// <summary>Blocking variant of <see cref="DebugStateAsync"/>. Not for UI threads.</summary>
    public JsonElement DebugState() => DebugStateAsync().GetAwaiter().GetResult();

    /// <summary>Blocking variant of <see cref="DebugSessionsAsync"/>. Not for UI threads.</summary>
    public JsonElement DebugSessions() => DebugSessionsAsync().GetAwaiter().GetResult();

    /// <summary>Blocking variant of <see cref="DebugSessionAsync"/>. Not for UI threads.</summary>
    public JsonElement DebugSession(string id) => DebugSessionAsync(id).GetAwaiter().GetResult();

    /// <summary>Blocking variant of <see cref="DebugMcpAsync"/>. Not for UI threads.</summary>
    public JsonElement DebugMcp() => DebugMcpAsync().GetAwaiter().GetResult();

    /// <summary>Blocking variant of <see cref="McpAsync(System.Text.Json.JsonElement,System.Threading.CancellationToken)"/>. Not for UI threads.</summary>
    public JsonElement Mcp(JsonElement jsonRpcPayload) => McpAsync(jsonRpcPayload).GetAwaiter().GetResult();

    /// <summary>Blocking variant of <see cref="ModelsAsync"/>. Not for UI threads.</summary>
    public JsonElement Models() => ModelsAsync().GetAwaiter().GetResult();

    /// <summary>Blocking variant of <see cref="ChatCompletionsAsync(System.Text.Json.JsonElement,System.Threading.CancellationToken)"/>. Not for UI threads.</summary>
    public JsonElement ChatCompletions(JsonElement body) => ChatCompletionsAsync(body).GetAwaiter().GetResult();
}
