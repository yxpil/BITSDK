import space.osbt.bitsdk.BitClient;
import space.osbt.bitsdk.BitException;
import space.osbt.bitsdk.BitJson;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Plain-Java smoke test for BitClient against the fake BIT server
 * (test/fake_bit_server.js on http://127.0.0.1:9803).
 *
 * Env: BIT_URL (default http://127.0.0.1:9803), BIT_KEY (default
 * bit_test_key_123456), BIT_PWD (default test-pwd-1).
 */
public class BitClientSmokeTest {

    private static final String URL = env("BIT_URL", "http://127.0.0.1:9803");
    private static final String KEY = env("BIT_KEY", "bit_test_key_123456");
    private static final String PWD = env("BIT_PWD", "test-pwd-1");

    private static int passed = 0;
    private static int failed = 0;

    public static void main(String[] args) {
        BitClient client = new BitClient(URL, KEY).accessPassword(PWD);

        test("health", () -> {
            Map<String, Object> res = client.health();
            check(Boolean.TRUE.equals(res.get("ok")), "health.ok should be true, got " + res);
        });

        test("register_invoke_remove", () -> {
            String name = "java_echo_" + Long.toHexString(System.nanoTime());
            Map<String, Object> tool = new LinkedHashMap<>();
            tool.put("name", name);
            tool.put("description", "smoke test tool");
            tool.put("parameters", map("type", "object", "properties", new LinkedHashMap<>()));
            tool.put("url", "http://127.0.0.1:9000/hook");
            Map<String, Object> registered = client.registerTool(tool);
            Map<?, ?> created = (Map<?, ?>) registered.get("tool");
            check(created != null && created.get("id") != null, "no tool id in " + registered);
            String id = String.valueOf(created.get("id"));

            Map<String, Object> params = new LinkedHashMap<>();
            params.put("ping", "pong");
            Map<String, Object> invocation = client.invokeTool(id, params);
            Map<?, ?> result = (Map<?, ?>) invocation.get("result");
            check(result != null && name.equals(result.get("via")),
                    "result.via should be " + name + ", got " + invocation);

            Map<String, Object> removed = client.removeTool(id);
            check(id.equals(removed.get("removed")), "removed should be " + id + ", got " + removed);
        });

        test("chat", () -> {
            String message = "java hello " + Long.toHexString(System.nanoTime());
            Map<String, Object> res = client.chat(message);
            String reply = String.valueOf(res.get("reply"));
            check(reply.startsWith("fake reply to: " + message),
                    "unexpected reply: " + reply);
        });

        test("debug_state", () -> {
            Map<String, Object> state = client.debugState();
            Map<?, ?> ai = (Map<?, ?>) state.get("ai");
            Map<?, ?> active = ai == null ? null : (Map<?, ?>) ai.get("active");
            check(active != null && active.get("model") != null, "ai.active.model missing: " + state);
        });

        test("debug_sessions", () -> {
            Map<String, Object> res = client.debugSessions();
            List<?> sessions = (List<?>) res.get("sessions");
            check(sessions != null && sessions.size() >= 1, "expected >=1 session, got " + res);
        });

        test("debug_session", () -> {
            Map<String, Object> session = client.debugSession("default");
            List<?> messages = (List<?>) session.get("messages");
            check(messages != null && !messages.isEmpty(), "default session has no messages: " + session);
        });

        test("mcp_initialize", () -> {
            Map<String, Object> payload = new LinkedHashMap<>();
            payload.put("jsonrpc", "2.0");
            payload.put("id", 1);
            payload.put("method", "initialize");
            Map<String, Object> params = new LinkedHashMap<>();
            params.put("protocolVersion", "2025-03-26");
            params.put("capabilities", new LinkedHashMap<>());
            params.put("clientInfo", map("name", "java-smoke", "version", "1.0"));
            payload.put("params", params);

            Map<String, Object> res = client.mcp(payload);
            Map<?, ?> result = (Map<?, ?>) res.get("result");
            Map<?, ?> serverInfo = result == null ? null : (Map<?, ?>) result.get("serverInfo");
            check(serverInfo != null && "fake-bit".equals(serverInfo.get("name")),
                    "serverInfo.name should be fake-bit, got " + res);
        });

        test("models", () -> {
            Map<String, Object> res = client.models();
            List<?> data = (List<?>) res.get("data");
            check(data != null && data.size() == 2, "expected 2 models, got " + res);
        });

        test("chat_completions", () -> {
            Map<String, Object> body = new LinkedHashMap<>();
            body.put("model", "fake-model");
            body.put("messages", List.of(map("role", "user", "content", "hi")));
            Map<String, Object> res = client.chatCompletions(body);
            String content = firstChoiceContent(res);
            check("你好，世界!".equals(content), "content should be 你好，世界!, got " + content);
        });

        test("chat_completions_stream", () -> {
            Map<String, Object> body = new LinkedHashMap<>();
            body.put("model", "fake-model");
            body.put("messages", List.of(map("role", "user", "content", "hi")));
            List<String> chunks = new ArrayList<>();
            AtomicReference<Map<String, Object>> finalChunk = new AtomicReference<>();
            String text = client.chatCompletionsStream(body, (chunk, last) -> {
                if (chunk != null) {
                    chunks.add(chunk);
                } else {
                    finalChunk.set(last);
                }
            });
            check("你好，世界!".equals(text), "assembled text should be 你好，世界!, got " + text);
            check(chunks.size() == 3, "expected 3 content chunks, got " + chunks);
            check(finalChunk.get() != null, "final chunk callback missing");
            check(finalChunk.get().get("usage") != null,
                    "final chunk should carry usage, got " + finalChunk.get());
        });

        test("wrong_key_401", () -> {
            BitClient bad = new BitClient(URL, "definitely-wrong-key").accessPassword(PWD);
            try {
                bad.chat("should fail");
                check(false, "expected BitException");
            } catch (BitException e) {
                check(e.getStatus() == 401, "expected status 401, got " + e.getStatus());
                check(e.getMessage() != null && !e.getMessage().isEmpty(),
                        "expected server error message, got: " + e.getMessage());
                check(e.getRaw() instanceof Map, "expected parsed raw body");
            }
        });

        test("missing_password_401", () -> {
            BitClient noPwd = new BitClient(URL, KEY); // no accessPassword set
            try {
                noPwd.chat("should fail");
                check(false, "expected BitException");
            } catch (BitException e) {
                check(e.getStatus() == 401, "expected status 401, got " + e.getStatus());
                check(String.valueOf(e.getMessage()).contains("访问密码"),
                        "expected password error text, got: " + e.getMessage());
            }
        });

        test("key_in_query", () -> {
            BitClient query = new BitClient(URL, KEY).accessPassword(PWD).keyInQuery(true);
            Map<String, Object> res = query.chat("query key works");
            String reply = String.valueOf(res.get("reply"));
            check(reply.startsWith("fake reply to: "), "unexpected reply: " + reply);
            check(query.debugState().get("ai") != null, "debugState via ?key= failed");
        });

        test("bitjson_roundtrip", () -> {
            String json = "{\"n\":1.5,\"i\":42,\"s\":\"中文\\n\\\"x\\\"\",\"b\":true,\"z\":null,"
                    + "\"arr\":[1,\"two\"],\"obj\":{\"k\":\"v\"}}";
            Map<?, ?> parsed = (Map<?, ?>) BitJson.parse(json);
            check(Double.valueOf(1.5).equals(parsed.get("n")), "float parse failed");
            check(Long.valueOf(42).equals(parsed.get("i")), "int parse failed");
            check("中文\n\"x\"".equals(parsed.get("s")), "string parse failed");
            String out = BitJson.write(parsed);
            check(out.contains("中文"), "non-ASCII must not be escaped: " + out);
            check(!out.contains("\\u4e2d"), "non-ASCII must not be escaped: " + out);
            Map<?, ?> reparsed = (Map<?, ?>) BitJson.parse(out);
            check(reparsed.equals(parsed), "roundtrip mismatch: " + out);
        });

        System.out.println();
        System.out.println("passed=" + passed + " failed=" + failed);
        if (failed > 0) {
            System.exit(1);
        }
    }

    // ------------------------------------------------------------------

    private static void test(String name, Runnable body) {
        try {
            body.run();
            passed++;
            System.out.println("[PASS] " + name);
        } catch (Throwable t) {
            failed++;
            System.out.println("[FAIL] " + name + ": " + t);
        }
    }

    private static void check(boolean condition, String message) {
        if (!condition) {
            throw new AssertionError(message);
        }
    }

    @SuppressWarnings("unchecked")
    private static String firstChoiceContent(Map<String, Object> response) {
        List<?> choices = (List<?>) response.get("choices");
        if (choices == null || choices.isEmpty()) {
            return null;
        }
        Map<String, Object> choice = (Map<String, Object>) choices.get(0);
        Map<String, Object> message = (Map<String, Object>) choice.get("message");
        return message == null ? null : (String) message.get("content");
    }

    private static Map<String, Object> map(Object... kv) {
        Map<String, Object> m = new LinkedHashMap<>();
        for (int i = 0; i < kv.length; i += 2) {
            m.put((String) kv[i], kv[i + 1]);
        }
        return m;
    }

    private static String env(String name, String fallback) {
        String value = System.getenv(name);
        return (value == null || value.isEmpty()) ? fallback : value;
    }
}
