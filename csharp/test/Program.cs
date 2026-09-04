// Smoke test for the BIT C# SDK against the fake BIT server
// (test/fake_bit_server.js on port 9803). Plain assertions, no test framework.
//
// Run (once a .NET 8 SDK is available):
//   dotnet run --project csharp/test          # starts server first if needed:
//   node test/fake_bit_server.js
//
// Env overrides: BIT_URL / BIT_KEY / BIT_PWD
// (defaults match the fake server: http://127.0.0.1:9803,
//  bit_test_key_123456, test-pwd-1)
//
// NOTE: the fake server is shared between concurrent test runs — this suite
// only asserts on objects it created itself (plus fixed endpoints), never on
// global tool/audit counts. Exits non-zero when any assertion fails.

using System;
using System.Collections.Generic;
using System.Text.Json;
using System.Threading.Tasks;
using BitSdk;

internal static class Program
{
    private static int _passed;
    private static readonly List<string> Failed = new();

    private static async Task<int> Main()
    {
        var baseUrl = Environment.GetEnvironmentVariable("BIT_URL") ?? "http://127.0.0.1:9803";
        var key = Environment.GetEnvironmentVariable("BIT_KEY") ?? "bit_test_key_123456";
        var pwd = Environment.GetEnvironmentVariable("BIT_PWD") ?? "test-pwd-1";

        using var client = new BitClient(baseUrl, key).WithAccessPassword(pwd);

        // 1. health (no auth required) — also exercises the sync wrapper.
        try
        {
            var health = client.Health();
            Check("health ok=true", health.TryGetProperty("ok", out var ok) && ok.GetBoolean());
        }
        catch (BitException e)
        {
            Check("health ok=true", false, e.Message);
        }

        // 2. tools lifecycle: register -> invoke -> remove (unique name: shared server).
        var toolName = $"csharp_sdk_tool_{DateTimeOffset.UtcNow.ToUnixTimeSeconds()}_{Random.Shared.Next(1000, 9999)}";
        var parameters = JsonDocument.Parse("""{"type":"object","properties":{}}""").RootElement.Clone();
        var toolId = "";
        try
        {
            var reg = await client.RegisterToolAsync(toolName, "created by csharp smoke test", parameters, "http://127.0.0.1:9803/hook");
            toolId = Str(reg, "tool", "id");
            Check("register tool returns id", toolId.Length > 0);
        }
        catch (BitException e)
        {
            Check("register tool returns id", false, e.Message);
        }

        try
        {
            var inv = await client.InvokeToolAsync(toolId, JsonDocument.Parse("""{"hello":"csharp"}""").RootElement.Clone());
            Check("invoke tool result.via==name",
                Str(inv, "result", "via") == toolName && Str(inv, "result", "echoed", "hello") == "csharp");
        }
        catch (BitException e)
        {
            Check("invoke tool result.via==name", false, e.Message);
        }

        try
        {
            var rem = await client.RemoveToolAsync(toolId);
            Check("remove tool removed==id", Str(rem, "removed") == toolId);
        }
        catch (BitException e)
        {
            Check("remove tool removed==id", false, e.Message);
        }

        // 3. chat
        try
        {
            var chat = await client.ChatAsync("csharp smoke " + DateTimeOffset.UtcNow.ToUnixTimeSeconds());
            Check("chat reply prefix \"fake reply to:\"", Str(chat, "reply").StartsWith("fake reply to:", StringComparison.Ordinal));
        }
        catch (BitException e)
        {
            Check("chat reply prefix \"fake reply to:\"", false, e.Message);
        }

        // 4. chat with an explicit session_id
        try
        {
            var chat = await client.ChatAsync("csharp session chat", sessionId: "csharp-smoke");
            Check("chat with session_id works", Str(chat, "reply").StartsWith("fake reply to:", StringComparison.Ordinal));
        }
        catch (BitException e)
        {
            Check("chat with session_id works", false, e.Message);
        }

        // 5. debug_state
        try
        {
            var state = await client.DebugStateAsync();
            Check("debug_state.ai.active.model exists",
                Nav(state, "ai", "active", "model").ValueKind == JsonValueKind.String &&
                Str(state, "ai", "active", "model").Length > 0);
        }
        catch (BitException e)
        {
            Check("debug_state.ai.active.model exists", false, e.Message);
        }

        // 6. debug_sessions (>=1; shared server — no exact count)
        try
        {
            var sessions = await client.DebugSessionsAsync();
            Check("debug_sessions >=1",
                Nav(sessions, "sessions").ValueKind == JsonValueKind.Array &&
                Nav(sessions, "sessions").GetArrayLength() >= 1);
        }
        catch (BitException e)
        {
            Check("debug_sessions >=1", false, e.Message);
        }

        // 7. debug_session('default')
        try
        {
            var sess = await client.DebugSessionAsync("default");
            Check("debug_session('default') has messages",
                Nav(sess, "messages").ValueKind == JsonValueKind.Array &&
                Nav(sess, "messages").GetArrayLength() >= 1);
        }
        catch (BitException e)
        {
            Check("debug_session('default') has messages", false, e.Message);
        }

        // 8. debug_mcp
        try
        {
            var mcpInfo = await client.DebugMcpAsync();
            Check("debug_mcp returns tools",
                Nav(mcpInfo, "tools").ValueKind == JsonValueKind.Array);
        }
        catch (BitException e)
        {
            Check("debug_mcp returns tools", false, e.Message);
        }

        // 9. audit (structure only; entries are shared global state)
        try
        {
            var audit = await client.AuditAsync();
            Check("audit returns entries",
                Nav(audit, "entries").ValueKind == JsonValueKind.Array);
        }
        catch (BitException e)
        {
            Check("audit returns entries", false, e.Message);
        }

        // 10. mcp initialize (raw JSON-RPC, password exempt)
        try
        {
            var payload = JsonDocument.Parse("""
                {"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"csharp-sdk","version":"1.0"}}}
                """).RootElement.Clone();
            var res = await client.McpAsync(payload);
            Check("mcp initialize result.serverInfo.name==\"fake-bit\"",
                Str(res, "result", "serverInfo", "name") == "fake-bit");
        }
        catch (BitException e)
        {
            Check("mcp initialize result.serverInfo.name==\"fake-bit\"", false, e.Message);
        }

        // 11. models (fixed endpoint: exactly 2 models)
        try
        {
            var models = await client.ModelsAsync();
            var data = Nav(models, "data");
            Check("models data len 2", data.ValueKind == JsonValueKind.Array && data.GetArrayLength() == 2);
        }
        catch (BitException e)
        {
            Check("models data len 2", false, e.Message);
        }

        // 12. chat_completions non-streaming (password exempt)
        try
        {
            var body = JsonDocument.Parse("""
                {"model":"fake-model","messages":[{"role":"user","content":"hi"}]}
                """).RootElement.Clone();
            var res = await client.ChatCompletionsAsync(body);
            Check("chat_completions content==\"你好，世界!\"",
                Str(res, "choices", 0, "message", "content") == "你好，世界!");
        }
        catch (BitException e)
        {
            Check("chat_completions content==\"你好，世界!\"", false, e.Message);
        }

        // 13. streaming: server chunks 你好 / ，世 / 界! — exercises multi-byte-safe
        //     line splitting (chunks are split mid-character across TCP writes).
        try
        {
            var body = JsonDocument.Parse("""
                {"model":"fake-model","messages":[{"role":"user","content":"hi"}]}
                """).RootElement.Clone();
            var parts = new List<string>();
            JsonElement? finalChunk = null;
            var full = await client.ChatCompletionsStreamAsync(body, (delta, fin) =>
            {
                if (delta != null) parts.Add(delta);
                if (fin.HasValue) finalChunk = fin;
                return Task.CompletedTask;
            });
            var assembled = string.Concat(parts);
            Check("stream assembles \"你好，世界!\"", full == "你好，世界!" && assembled == "你好，世界!");
            Check("stream final chunk has usage",
                finalChunk.HasValue &&
                Nav(finalChunk.Value, "usage").TryGetProperty("total_tokens", out var tt) &&
                tt.GetInt32() == 17);
        }
        catch (BitException e)
        {
            Check("stream assembles \"你好，世界!\"", false, e.Message);
            Check("stream final chunk has usage", false, e.Message);
        }

        // 14. wrong key -> 401
        try
        {
            using var bad = new BitClient(baseUrl, "bit_wrong_key").WithAccessPassword(pwd);
            await bad.ListToolsAsync();
            Check("wrong key -> 401", false, "no exception thrown");
        }
        catch (BitException e)
        {
            Check("wrong key -> 401", e.Status == 401, $"status {e.Status}: {e.Message}");
        }

        // 15. wrong/missing password -> 401
        try
        {
            using var noPwd = new BitClient(baseUrl, key).WithAccessPassword("totally-wrong");
            await noPwd.ListToolsAsync();
            Check("missing password -> 401", false, "no exception thrown");
        }
        catch (BitException e)
        {
            Check("missing password -> 401", e.Status == 401, $"status {e.Status}: {e.Message}");
        }

        // 16. key_in_query (?key= instead of Authorization header)
        try
        {
            using var inq = new BitClient(baseUrl, key).WithAccessPassword(pwd).WithKeyInQuery();
            var tools = await inq.ListToolsAsync();
            Check("key_in_query works", Nav(tools, "tools").ValueKind == JsonValueKind.Array);
        }
        catch (BitException e)
        {
            Check("key_in_query works", false, e.Message);
        }

        Console.WriteLine($"\n{_passed} passed, {Failed.Count} failed");
        if (Failed.Count > 0)
        {
            foreach (var name in Failed)
            {
                Console.WriteLine("  FAILED: " + name);
            }
            return 1;
        }
        return 0;
    }

    private static void Check(string name, bool ok, string detail = "")
    {
        if (ok)
        {
            _passed++;
            Console.WriteLine($"[PASS] {name}");
        }
        else
        {
            Failed.Add(name);
            Console.WriteLine($"[FAIL] {name}{(detail.Length > 0 ? $" - {detail}" : "")}");
        }
    }

    // Navigation helpers: default(JsonElement) (ValueKind == Undefined) on
    // missing path segments, "" for missing strings. An int path segment
    // indexes into a JSON array; a string segment reads an object property.
    private static JsonElement Nav(JsonElement e, params object[] path)
    {
        var cur = e;
        foreach (var p in path)
        {
            if (p is int index)
            {
                if (cur.ValueKind != JsonValueKind.Array ||
                    index < 0 || index >= cur.GetArrayLength())
                {
                    return default;
                }
                cur = cur[index];
            }
            else
            {
                if (cur.ValueKind != JsonValueKind.Object ||
                    !cur.TryGetProperty((string)p, out var next))
                {
                    return default;
                }
                cur = next;
            }
        }
        return cur;
    }

    private static string Str(JsonElement e, params object[] path)
    {
        var cur = Nav(e, path);
        return cur.ValueKind == JsonValueKind.String ? cur.GetString()! : "";
    }
}
