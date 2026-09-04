package space.osbt.bitsdk;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.net.URI;
import java.net.URLEncoder;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.function.BiConsumer;

/**
 * Java client for the BIT HTTP API (see docs/API.md).
 *
 * <p>Authentication: the Client Key is sent as {@code Authorization: Bearer}
 * header, or as a {@code ?key=} query parameter when {@link #keyInQuery(boolean)}
 * is enabled. The access password (required for {@code /api/*} except
 * {@code /api/health}; exempt for {@code /v1/*} and {@code /mcp}) is sent as the
 * {@code X-Access-Password} header when set via {@link #accessPassword(String)}.
 *
 * <p>Methods throw {@link BitException} for HTTP error responses (status >= 400).
 */
public class BitClient {

    private final String baseUrl;
    private final String clientKey;
    private final HttpClient http;

    private String accessPassword;
    private int timeoutMs = 30000;
    private boolean keyInQuery = false;

    public BitClient(String baseUrl, String clientKey) {
        String trimmed = (baseUrl == null) ? "" : baseUrl.trim();
        while (trimmed.endsWith("/")) {
            trimmed = trimmed.substring(0, trimmed.length() - 1);
        }
        this.baseUrl = trimmed;
        this.clientKey = clientKey;
        this.http = HttpClient.newBuilder()
                .connectTimeout(Duration.ofSeconds(10))
                .build();
    }

    /** Sets the access password sent as the X-Access-Password header. */
    public BitClient accessPassword(String accessPassword) {
        this.accessPassword = accessPassword;
        return this;
    }

    /** Per-request timeout in milliseconds (default 30000). */
    public BitClient timeoutMs(int timeoutMs) {
        this.timeoutMs = timeoutMs;
        return this;
    }

    /** When true, the Client Key is sent as a ?key= query parameter instead of the Authorization header. */
    public BitClient keyInQuery(boolean keyInQuery) {
        this.keyInQuery = keyInQuery;
        return this;
    }

    // ------------------------------------------------------------------
    // API methods (docs/API.md)
    // ------------------------------------------------------------------

    /** GET /api/health */
    public Map<String, Object> health() {
        return send("GET", "/api/health", null);
    }

    /** GET /api/tools */
    public Map<String, Object> listTools() {
        return send("GET", "/api/tools", null);
    }

    /** POST /api/tools — map with name/description/parameters/url. */
    public Map<String, Object> registerTool(Map<String, Object> tool) {
        return send("POST", "/api/tools", tool);
    }

    /** DELETE /api/tools/{id} */
    public Map<String, Object> removeTool(String id) {
        return send("DELETE", "/api/tools/" + encodeSegment(id), null);
    }

    /** POST /api/tools/{id}/invoke */
    public Map<String, Object> invokeTool(String id, Map<String, Object> params) {
        Map<String, Object> body = new LinkedHashMap<String, Object>();
        body.put("params", params == null ? new LinkedHashMap<String, Object>() : params);
        return send("POST", "/api/tools/" + encodeSegment(id) + "/invoke", body);
    }

    /** POST /api/chat — simple form: message only. */
    public Map<String, Object> chat(String message) {
        Map<String, Object> body = new LinkedHashMap<String, Object>();
        body.put("message", message);
        return chat(body);
    }

    /** POST /api/chat — full body with message/sessionId/images. */
    public Map<String, Object> chat(Map<String, Object> body) {
        return send("POST", "/api/chat", body);
    }

    /** GET /api/audit */
    public Map<String, Object> audit() {
        return send("GET", "/api/audit", null);
    }

    /** GET /api/debug/state */
    public Map<String, Object> debugState() {
        return send("GET", "/api/debug/state", null);
    }

    /** GET /api/debug/sessions */
    public Map<String, Object> debugSessions() {
        return send("GET", "/api/debug/sessions", null);
    }

    /** GET /api/debug/sessions/{id} */
    public Map<String, Object> debugSession(String id) {
        return send("GET", "/api/debug/sessions/" + encodeSegment(id), null);
    }

    /** GET /api/debug/mcp */
    public Map<String, Object> debugMcp() {
        return send("GET", "/api/debug/mcp", null);
    }

    /** POST /mcp — raw JSON-RPC 2.0 payload. */
    public Map<String, Object> mcp(Map<String, Object> jsonrpcPayload) {
        return send("POST", "/mcp", jsonrpcPayload);
    }

    /** GET /v1/models */
    public Map<String, Object> models() {
        return send("GET", "/v1/models", null);
    }

    /** POST /v1/chat/completions (non-streaming OpenAI call). */
    public Map<String, Object> chatCompletions(Map<String, Object> body) {
        return send("POST", "/v1/chat/completions", body);
    }

    /**
     * POST /v1/chat/completions with {@code stream:true}, consuming the SSE
     * response. For every content chunk {@code onDelta.accept(chunk, null)} is
     * called; when the stream ends, {@code onDelta.accept(null, finalChunk)}
     * is invoked once with the last parsed chunk (may carry usage). Lines are
     * decoded with a BufferedReader so multi-byte UTF-8 split across TCP
     * chunks is handled safely. Returns the assembled full text.
     */
    public String chatCompletionsStream(Map<String, Object> body,
                                        BiConsumer<String, Map<String, Object>> onDelta) {
        Map<String, Object> payload = new LinkedHashMap<String, Object>(body);
        payload.put("stream", true);

        HttpResponse<java.io.InputStream> response =
                sendRaw("POST", "/v1/chat/completions", payload, HttpResponse.BodyHandlers.ofInputStream());

        if (response.statusCode() >= 400) {
            String errorBody = readAll(response.body());
            throw new BitException(response.statusCode(), errorBody);
        }

        StringBuilder full = new StringBuilder();
        Map<String, Object> lastChunk = null;
        try (BufferedReader reader =
                     new BufferedReader(new InputStreamReader(response.body(), StandardCharsets.UTF_8))) {
            String line;
            while ((line = reader.readLine()) != null) {
                if (!line.startsWith("data:")) {
                    continue; // ignore blank lines, comments and event: fields
                }
                String data = line.substring("data:".length()).trim();
                if (data.equals("[DONE]")) {
                    break;
                }
                if (data.isEmpty()) {
                    continue;
                }
                Object parsed = BitJson.parse(data);
                if (!(parsed instanceof Map)) {
                    continue;
                }
                @SuppressWarnings("unchecked")
                Map<String, Object> chunk = (Map<String, Object>) parsed;
                lastChunk = chunk;
                String content = deltaContent(chunk);
                if (content != null && !content.isEmpty()) {
                    full.append(content);
                    if (onDelta != null) {
                        onDelta.accept(content, null);
                    }
                }
            }
        } catch (java.io.IOException e) {
            throw new RuntimeException("failed reading SSE stream", e);
        }
        if (onDelta != null && lastChunk != null) {
            onDelta.accept(null, lastChunk);
        }
        return full.toString();
    }

    /** Extracts choices[0].delta.content from a streaming chunk, or null. */
    @SuppressWarnings("unchecked")
    private static String deltaContent(Map<String, Object> chunk) {
        Object choices = chunk.get("choices");
        if (choices instanceof List && !((List<?>) choices).isEmpty()) {
            Object first = ((List<?>) choices).get(0);
            if (first instanceof Map) {
                Object delta = ((Map<String, Object>) first).get("delta");
                if (delta instanceof Map) {
                    Object content = ((Map<String, Object>) delta).get("content");
                    if (content instanceof String) {
                        return (String) content;
                    }
                }
            }
        }
        return null;
    }

    private static String readAll(java.io.InputStream in) {
        try {
            return new String(in.readAllBytes(), StandardCharsets.UTF_8);
        } catch (java.io.IOException e) {
            return "";
        }
    }

    // ------------------------------------------------------------------
    // Transport
    // ------------------------------------------------------------------

    /** Sends a request and returns the parsed JSON object, throwing BitException on HTTP errors. */
    private Map<String, Object> send(String method, String path, Object body) {
        HttpResponse<String> response = sendRaw(method, path, body, HttpResponse.BodyHandlers.ofString());
        if (response.statusCode() >= 400) {
            throw new BitException(response.statusCode(), response.body());
        }
        String text = response.body();
        if (text == null || text.isEmpty()) {
            return new LinkedHashMap<String, Object>();
        }
        Object parsed = BitJson.parse(text);
        if (parsed instanceof Map) {
            @SuppressWarnings("unchecked")
            Map<String, Object> map = (Map<String, Object>) parsed;
            return map;
        }
        throw new IllegalStateException("expected a JSON object response, got: " + text);
    }

    private <T> HttpResponse<T> sendRaw(String method, String path, Object body,
                                        HttpResponse.BodyHandler<T> handler) {
        String url = baseUrl + path;
        if (keyInQuery && clientKey != null && !clientKey.isEmpty()) {
            String encodedKey = URLEncoder.encode(clientKey, StandardCharsets.UTF_8);
            url += (url.contains("?") ? "&" : "?") + "key=" + encodedKey;
        }

        HttpRequest.Builder builder = HttpRequest.newBuilder(URI.create(url))
                .timeout(Duration.ofMillis(timeoutMs));

        if (!keyInQuery && clientKey != null && !clientKey.isEmpty()) {
            builder.header("Authorization", "Bearer " + clientKey);
        }
        if (accessPassword != null && !accessPassword.isEmpty()) {
            builder.header("X-Access-Password", accessPassword);
        }
        if (body != null) {
            builder.header("Content-Type", "application/json");
            builder.method(method, HttpRequest.BodyPublishers.ofString(BitJson.write(body), StandardCharsets.UTF_8));
        } else {
            builder.method(method, HttpRequest.BodyPublishers.noBody());
        }

        try {
            return http.send(builder.build(), handler);
        } catch (java.io.IOException | InterruptedException e) {
            if (e instanceof InterruptedException) {
                Thread.currentThread().interrupt();
            }
            throw new RuntimeException("request to " + url + " failed: " + e.getMessage(), e);
        }
    }

    private static String encodeSegment(String segment) {
        return URLEncoder.encode(segment, StandardCharsets.UTF_8);
    }
}
