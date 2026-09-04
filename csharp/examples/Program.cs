// Example console app for the BIT C# SDK.
//
// Run (once a .NET 8 SDK is available):
//   dotnet run --project csharp/examples
//
// Env:
//   BIT_URL (default http://127.0.0.1:7777 — the BIT desktop app remote port)
//   BIT_KEY (required: your BIT Client Key)
//   BIT_PWD (required for /api/* endpoints: your BIT access password)

using System;
using System.Text.Json;
using System.Threading.Tasks;
using BitSdk;

internal static class Program
{
    private static async Task Main()
    {
        var baseUrl = Environment.GetEnvironmentVariable("BIT_URL") ?? "http://127.0.0.1:7777";
        var key = Environment.GetEnvironmentVariable("BIT_KEY") ?? "YOUR_CLIENT_KEY";
        var pwd = Environment.GetEnvironmentVariable("BIT_PWD") ?? "";

        using var bit = new BitClient(baseUrl, key)
            .WithAccessPassword(pwd)                    // X-Access-Password on /api/*
            .WithTimeout(TimeSpan.FromSeconds(30));

        // 1) Health — no auth needed.
        var health = await bit.HealthAsync();
        Console.WriteLine("health: " + JsonSerializer.Serialize(health));

        // 2) Agent chat (full agent turn; BIT may call tools/MCP internally).
        var chat = await bit.ChatAsync("hello from the csharp sdk");
        Console.WriteLine("reply: " + (chat.TryGetProperty("reply", out var reply) ? reply.GetString() : "?"));

        // 3) OpenAI-compatible streaming (access password exempt).
        var body = JsonDocument.Parse("""
            {"model":"bit","messages":[{"role":"user","content":"你好"}]}
            """).RootElement.Clone();

        var full = await bit.ChatCompletionsStreamAsync(body, (delta, final) =>
        {
            if (delta != null) Console.Write(delta);
            if (final.HasValue && final.Value.TryGetProperty("usage", out _))
            {
                Console.WriteLine("\n[stream done]");
            }
            return Task.CompletedTask;
        });
        Console.WriteLine("full text: " + full);
    }
}
