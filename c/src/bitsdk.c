/*
 * bitsdk.c — C client for the BIT HTTP API (docs/API.md).
 *
 * Transport is libcurl; JSON handling is the vendored cJSON in src/cjson.
 * HTTP error responses are returned to the caller as parsed cJSON; only
 * transport failures (curl errors, invalid JSON) return NULL with
 * bit_last_error() set.
 */
#include "bitsdk.h"

#include <curl/curl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct bit_client {
    char *base_url;
    char *client_key;
    char *access_password;
    long timeout_ms;
    bool key_in_query;
    char last_error[512];
};

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static char *bit_strdup(const char *s)
{
    if (s == NULL) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char *copy = malloc(n);
    if (copy != NULL) {
        memcpy(copy, s, n);
    }
    return copy;
}

static void set_error(bit_client *c, const char *fmt, ...)
{
    if (c == NULL) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vsnprintf(c->last_error, sizeof(c->last_error), fmt, args);
    va_end(args);
}

/* Percent-encode a string for use in a query parameter (RFC 3986 unreserved
 * characters are kept; everything else, including all non-ASCII bytes, is
 * %XX-escaped). Returns a malloc'd string or NULL. */
static char *percent_encode(const char *s)
{
    if (s == NULL) {
        return NULL;
    }
    size_t out_len = 0;
    for (const char *p = s; *p != '\0'; p++) {
        unsigned char ch = (unsigned char)*p;
        out_len += ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                    (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
                    ch == '.' || ch == '~')
                       ? 1
                       : 3;
    }
    char *out = malloc(out_len + 1);
    if (out == NULL) {
        return NULL;
    }
    char *o = out;
    for (const char *p = s; *p != '\0'; p++) {
        unsigned char ch = (unsigned char)*p;
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
            ch == '.' || ch == '~') {
            *o++ = (char)ch;
        } else {
            sprintf(o, "%%%02X", ch);
            o += 3;
        }
    }
    *o = '\0';
    return out;
}

static char *build_url(bit_client *c, const char *path)
{
    /* strip trailing slashes from base_url once, lazily */
    size_t base_len = strlen(c->base_url);
    while (base_len > 0 && c->base_url[base_len - 1] == '/') {
        c->base_url[--base_len] = '\0';
    }

    const char *sep = "?";
    if (strchr(path, '?') != NULL) {
        sep = "&";
    }

    size_t size = base_len + strlen(path) + 1;
    char *key_param = NULL;
    if (c->key_in_query && c->client_key != NULL) {
        char *escaped = percent_encode(c->client_key);
        if (escaped == NULL) {
            set_error(c, "out of memory");
            return NULL;
        }
        size_t key_len = strlen("key=") + strlen(escaped);
        key_param = malloc(key_len + 1);
        if (key_param == NULL) {
            free(escaped);
            set_error(c, "out of memory");
            return NULL;
        }
        snprintf(key_param, key_len + 1, "key=%s", escaped);
        free(escaped);
        size += 1 + key_len;
    }

    char *url = malloc(size);
    if (url == NULL) {
        free(key_param);
        set_error(c, "out of memory");
        return NULL;
    }
    snprintf(url, size, "%s%s%s", c->base_url, path, key_param ? sep : "");
    if (key_param != NULL) {
        strcat(url, key_param);
        free(key_param);
    }
    return url;
}

/* growable byte buffer for response bodies */
typedef struct {
    char *data;
    size_t size;
    size_t cap;
} membuf;

static void membuf_init(membuf *b)
{
    b->data = NULL;
    b->size = 0;
    b->cap = 0;
}

static void membuf_free(membuf *b)
{
    free(b->data);
    membuf_init(b);
}

static bool membuf_append(membuf *b, const char *bytes, size_t n)
{
    if (b->size + n + 1 > b->cap) {
        size_t newcap = b->cap ? b->cap : 1024;
        while (newcap < b->size + n + 1) {
            newcap *= 2;
        }
        char *grown = realloc(b->data, newcap);
        if (grown == NULL) {
            return false;
        }
        b->data = grown;
        b->cap = newcap;
    }
    memcpy(b->data + b->size, bytes, n);
    b->size += n;
    b->data[b->size] = '\0';
    return true;
}

static size_t write_mem_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    membuf *buf = (membuf *)userdata;
    size_t total = size * nmemb;
    if (!membuf_append(buf, ptr, total)) {
        return 0; /* signals curl an error */
    }
    return total;
}

/* ------------------------------------------------------------------ */
/* lifecycle & options                                                */
/* ------------------------------------------------------------------ */

bit_client *bit_client_new(const char *base_url, const char *client_key)
{
    if (base_url == NULL) {
        return NULL;
    }
    bit_client *c = calloc(1, sizeof(bit_client));
    if (c == NULL) {
        return NULL;
    }
    c->base_url = bit_strdup(base_url);
    c->client_key = bit_strdup(client_key);
    c->timeout_ms = 30000;
    if (c->base_url == NULL) {
        bit_client_free(c);
        return NULL;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    return c;
}

void bit_client_free(bit_client *c)
{
    if (c == NULL) {
        return;
    }
    free(c->base_url);
    free(c->client_key);
    free(c->access_password);
    free(c);
}

void bit_set_access_password(bit_client *c, const char *password)
{
    if (c == NULL) {
        return;
    }
    free(c->access_password);
    c->access_password = bit_strdup(password);
}

void bit_set_timeout_ms(bit_client *c, long timeout_ms)
{
    if (c != NULL && timeout_ms > 0) {
        c->timeout_ms = timeout_ms;
    }
}

void bit_set_key_in_query(bit_client *c, bool key_in_query)
{
    if (c != NULL) {
        c->key_in_query = key_in_query;
    }
}

const char *bit_last_error(bit_client *c)
{
    return (c == NULL) ? "" : c->last_error;
}

/* ------------------------------------------------------------------ */
/* core transport                                                     */
/* ------------------------------------------------------------------ */

static struct curl_slist *build_headers(bit_client *c, bool with_body)
{
    struct curl_slist *headers = NULL;
    char header[512];

    if (!c->key_in_query && c->client_key != NULL) {
        snprintf(header, sizeof(header), "Authorization: Bearer %s", c->client_key);
        headers = curl_slist_append(headers, header);
    }
    if (c->access_password != NULL) {
        snprintf(header, sizeof(header), "X-Access-Password: %s", c->access_password);
        headers = curl_slist_append(headers, header);
    }
    if (with_body) {
        headers = curl_slist_append(headers, "Content-Type: application/json");
    }
    return headers;
}

cJSON *bit_request(bit_client *c, const char *method, const char *path,
                   const cJSON *body_or_null, int *status_out)
{
    if (status_out != NULL) {
        *status_out = 0;
    }
    if (c == NULL || method == NULL || path == NULL) {
        return NULL;
    }
    c->last_error[0] = '\0';

    char *url = build_url(c, path);
    if (url == NULL) {
        return NULL;
    }

    char *payload = NULL;
    if (body_or_null != NULL) {
        payload = cJSON_PrintUnformatted((cJSON *)body_or_null);
        if (payload == NULL) {
            free(url);
            set_error(c, "failed to serialize request body");
            return NULL;
        }
    }

    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        free(url);
        free(payload);
        set_error(c, "failed to initialize curl");
        return NULL;
    }

    membuf buf;
    membuf_init(&buf);

    struct curl_slist *headers = build_headers(c, payload != NULL);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, c->timeout_ms);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_mem_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
#if LIBCURL_VERSION_NUM >= 0x072000
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
#endif
    if (payload != NULL) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(payload));
    }

    CURLcode rc = curl_easy_perform(curl);

    long status = 0;
    if (rc == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    }
    if (status_out != NULL) {
        *status_out = (rc == CURLE_OK) ? (int)status : 0;
    }

    cJSON *result = NULL;
    if (rc != CURLE_OK) {
        set_error(c, "request %s %s failed: %s", method, path, curl_easy_strerror(rc));
    } else if (buf.data == NULL) {
        /* empty body (e.g. 204): return an empty object */
        result = cJSON_CreateObject();
    } else {
        result = cJSON_Parse(buf.data);
        if (result == NULL) {
            set_error(c, "invalid JSON response from %s %s (HTTP %ld)", method, path, status);
        }
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    membuf_free(&buf);
    free(payload);
    free(url);
    return result;
}

char *bit_error_message(const cJSON *response_or_null, int status)
{
    if (response_or_null != NULL) {
        const cJSON *error = cJSON_GetObjectItemCaseSensitive(response_or_null, "error");
        if (cJSON_IsString(error) && error->valuestring != NULL) {
            return bit_strdup(error->valuestring);
        }
        if (cJSON_IsObject(error)) {
            const cJSON *message = cJSON_GetObjectItemCaseSensitive(error, "message");
            if (cJSON_IsString(message) && message->valuestring != NULL) {
                return bit_strdup(message->valuestring);
            }
        }
    }
    char fallback[32];
    snprintf(fallback, sizeof(fallback), "HTTP %d", status);
    return bit_strdup(fallback);
}

/* ------------------------------------------------------------------ */
/* convenience wrappers                                               */
/* ------------------------------------------------------------------ */

cJSON *bit_health(bit_client *c)
{
    return bit_request(c, "GET", "/api/health", NULL, NULL);
}

cJSON *bit_list_tools(bit_client *c)
{
    return bit_request(c, "GET", "/api/tools", NULL, NULL);
}

cJSON *bit_register_tool(bit_client *c, const char *name, const char *description,
                         const char *parameters_json, const char *url)
{
    cJSON *body = cJSON_CreateObject();
    if (body == NULL) {
        set_error(c, "out of memory");
        return NULL;
    }
    cJSON_AddStringToObject(body, "name", name ? name : "");
    if (description != NULL) {
        cJSON_AddStringToObject(body, "description", description);
    }
    cJSON *params = (parameters_json != NULL) ? cJSON_Parse(parameters_json) : NULL;
    if (params == NULL) {
        params = cJSON_Parse("{\"type\":\"object\",\"properties\":{}}");
    }
    if (params == NULL) {
        cJSON_Delete(body);
        set_error(c, "failed to parse parameters_json");
        return NULL;
    }
    cJSON_AddItemToObject(body, "parameters", params);
    cJSON_AddStringToObject(body, "url", url ? url : "");

    cJSON *response = bit_request(c, "POST", "/api/tools", body, NULL);
    cJSON_Delete(body);
    return response;
}

cJSON *bit_remove_tool(bit_client *c, const char *tool_id)
{
    if (tool_id == NULL) {
        set_error(c, "tool_id is NULL");
        return NULL;
    }
    size_t size = strlen("/api/tools/") + strlen(tool_id) + 1;
    char *path = malloc(size);
    if (path == NULL) {
        set_error(c, "out of memory");
        return NULL;
    }
    snprintf(path, size, "/api/tools/%s", tool_id);
    cJSON *response = bit_request(c, "DELETE", path, NULL, NULL);
    free(path);
    return response;
}

cJSON *bit_invoke_tool(bit_client *c, const char *tool_id, const char *params_json)
{
    if (tool_id == NULL) {
        set_error(c, "tool_id is NULL");
        return NULL;
    }
    cJSON *params = (params_json != NULL) ? cJSON_Parse(params_json) : NULL;
    if (params == NULL) {
        params = cJSON_CreateObject();
    }
    if (params == NULL) {
        set_error(c, "out of memory");
        return NULL;
    }
    cJSON *body = cJSON_CreateObject();
    if (body == NULL) {
        cJSON_Delete(params);
        set_error(c, "out of memory");
        return NULL;
    }
    cJSON_AddItemToObject(body, "params", params);

    size_t size = strlen("/api/tools//invoke") + strlen(tool_id) + 1;
    char *path = malloc(size);
    if (path == NULL) {
        cJSON_Delete(body);
        set_error(c, "out of memory");
        return NULL;
    }
    snprintf(path, size, "/api/tools/%s/invoke", tool_id);
    cJSON *response = bit_request(c, "POST", path, body, NULL);
    free(path);
    cJSON_Delete(body);
    return response;
}

cJSON *bit_chat(bit_client *c, const char *message, const char *session_id_or_null)
{
    cJSON *body = cJSON_CreateObject();
    if (body == NULL) {
        set_error(c, "out of memory");
        return NULL;
    }
    cJSON_AddStringToObject(body, "message", message ? message : "");
    if (session_id_or_null != NULL) {
        cJSON_AddStringToObject(body, "session_id", session_id_or_null);
    }
    cJSON *response = bit_request(c, "POST", "/api/chat", body, NULL);
    cJSON_Delete(body);
    return response;
}

cJSON *bit_audit(bit_client *c)
{
    return bit_request(c, "GET", "/api/audit", NULL, NULL);
}

cJSON *bit_debug_state(bit_client *c)
{
    return bit_request(c, "GET", "/api/debug/state", NULL, NULL);
}

cJSON *bit_debug_sessions(bit_client *c)
{
    return bit_request(c, "GET", "/api/debug/sessions", NULL, NULL);
}

cJSON *bit_debug_session(bit_client *c, const char *session_id)
{
    if (session_id == NULL) {
        set_error(c, "session_id is NULL");
        return NULL;
    }
    size_t size = strlen("/api/debug/sessions/") + strlen(session_id) + 1;
    char *path = malloc(size);
    if (path == NULL) {
        set_error(c, "out of memory");
        return NULL;
    }
    snprintf(path, size, "/api/debug/sessions/%s", session_id);
    cJSON *response = bit_request(c, "GET", path, NULL, NULL);
    free(path);
    return response;
}

cJSON *bit_debug_mcp(bit_client *c)
{
    return bit_request(c, "GET", "/api/debug/mcp", NULL, NULL);
}

cJSON *bit_mcp(bit_client *c, const char *jsonrpc_body_json)
{
    cJSON *body = (jsonrpc_body_json != NULL) ? cJSON_Parse(jsonrpc_body_json) : NULL;
    if (body == NULL) {
        set_error(c, "failed to parse jsonrpc_body_json");
        return NULL;
    }
    cJSON *response = bit_request(c, "POST", "/mcp", body, NULL);
    cJSON_Delete(body);
    return response;
}

cJSON *bit_models(bit_client *c)
{
    return bit_request(c, "GET", "/v1/models", NULL, NULL);
}

cJSON *bit_chat_completions(bit_client *c, const char *body_json)
{
    cJSON *body = (body_json != NULL) ? cJSON_Parse(body_json) : NULL;
    if (body == NULL) {
        set_error(c, "failed to parse body_json");
        return NULL;
    }
    cJSON *response = bit_request(c, "POST", "/v1/chat/completions", body, NULL);
    cJSON_Delete(body);
    return response;
}

/* ------------------------------------------------------------------ */
/* streaming (SSE)                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    bit_client *c;
    CURL *curl;
    membuf line;      /* partial line bytes (split only on '\n') */
    membuf text;      /* assembled full text */
    cJSON *last;      /* last parsed chunk (may carry usage) */
    void (*on_delta)(const char *chunk, const cJSON *final_chunk, void *ud);
    void *userdata;
    bool done;        /* saw "data: [DONE]" */
    long status;      /* HTTP status (from headers) */
    membuf errbody;   /* accumulates body when status >= 400 */
} stream_ctx;

/* Extract choices[0].delta.content, or NULL. Returns a malloc'd string or NULL. */
static char *chunk_content(const cJSON *chunk)
{
    const cJSON *choices = cJSON_GetObjectItemCaseSensitive(chunk, "choices");
    if (!cJSON_IsArray(choices)) {
        return NULL;
    }
    const cJSON *first = cJSON_GetArrayItem(choices, 0);
    if (first == NULL) {
        return NULL;
    }
    const cJSON *delta = cJSON_GetObjectItemCaseSensitive(first, "delta");
    const cJSON *content = cJSON_IsObject(delta)
        ? cJSON_GetObjectItemCaseSensitive(delta, "content")
        : NULL;
    if (cJSON_IsString(content) && content->valuestring != NULL) {
        return bit_strdup(content->valuestring);
    }
    return NULL;
}

/* Process one complete SSE line (no trailing '\n'). */
static void stream_process_line(stream_ctx *ctx, const char *line, size_t len)
{
    if (ctx->done) {
        return;
    }
    /* only "data: ..." lines matter; ignore comments, event:, blank lines */
    if (len < 5 || strncmp(line, "data:", 5) != 0) {
        return;
    }
    const char *payload = line + 5;
    const char *end = line + len;
    while (payload < end && (*payload == ' ' || *payload == '\t')) {
        payload++;
    }
    if ((size_t)(end - payload) >= 6 && strncmp(payload, "[DONE]", 6) == 0) {
        ctx->done = true;
        return;
    }
    if (payload >= end) {
        return;
    }
    cJSON *chunk = cJSON_Parse(payload);
    if (chunk == NULL) {
        return; /* ignore unparsable keep-alives */
    }
    cJSON_Delete(ctx->last);
    ctx->last = chunk;

    char *content = chunk_content(chunk);
    if (content != NULL && content[0] != '\0') {
        membuf_append(&ctx->text, content, strlen(content));
        if (ctx->on_delta != NULL) {
            ctx->on_delta(content, NULL, ctx->userdata);
        }
    }
    free(content);
}

static size_t write_stream_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    stream_ctx *ctx = (stream_ctx *)userdata;
    size_t total = size * nmemb;

    /* determine HTTP status once headers are available */
    if (ctx->status == 0 && ctx->curl != NULL) {
        long code = 0;
        if (curl_easy_getinfo(ctx->curl, CURLINFO_RESPONSE_CODE, &code) == CURLE_OK && code != 0) {
            ctx->status = code;
        }
    }

    if (ctx->status >= 400) {
        /* collect the error body instead of streaming */
        if (!membuf_append(&ctx->errbody, ptr, total)) {
            return 0;
        }
        return total;
    }

    if (!membuf_append(&ctx->line, ptr, total)) {
        return 0;
    }

    /* emit every complete line (split ONLY on '\n'; the remainder stays
     * buffered so multi-byte UTF-8 sequences split across TCP chunks are
     * never decoded partially) */
    size_t start = 0;
    for (size_t i = 0; i < ctx->line.size; i++) {
        if (ctx->line.data[i] == '\n') {
            size_t len = i - start;
            if (len > 0 && ctx->line.data[len + start - 1] == '\r') {
                len--;
            }
            ctx->line.data[len + start] = '\0'; /* safe: we own the byte at i, and len<=i-start */
            stream_process_line(ctx, ctx->line.data + start, len);
            start = i + 1;
        }
    }
    if (start > 0) {
        memmove(ctx->line.data, ctx->line.data + start, ctx->line.size - start);
        ctx->line.size -= start;
        ctx->line.data[ctx->line.size] = '\0';
    }
    return total;
}

int bit_chat_completions_stream(bit_client *c, const char *body_json,
                                void (*on_delta)(const char *chunk, const cJSON *final_chunk, void *ud),
                                void *userdata, char *full_text_out, size_t out_size)
{
    if (c == NULL) {
        return -1;
    }
    c->last_error[0] = '\0';

    cJSON *body = (body_json != NULL) ? cJSON_Parse(body_json) : NULL;
    if (body == NULL) {
        set_error(c, "failed to parse body_json");
        return -1;
    }
    if (cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(body, "stream"))) {
        cJSON_ReplaceItemInObjectCaseSensitive(body, "stream", cJSON_CreateBool(true));
    } else {
        cJSON_AddBoolToObject(body, "stream", true);
    }
    char *payload = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (payload == NULL) {
        set_error(c, "failed to serialize request body");
        return -1;
    }

    char *url = build_url(c, "/v1/chat/completions");
    if (url == NULL) {
        free(payload);
        return -1;
    }

    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        free(payload);
        free(url);
        set_error(c, "failed to initialize curl");
        return -1;
    }

    stream_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.c = c;
    ctx.curl = curl;
    ctx.on_delta = on_delta;
    ctx.userdata = userdata;
    membuf_init(&ctx.line);
    membuf_init(&ctx.text);
    membuf_init(&ctx.errbody);

    struct curl_slist *headers = build_headers(c, true);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, c->timeout_ms);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(payload));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_stream_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode rc = curl_easy_perform(curl);
    int result = 0;

    if (ctx.status >= 400) {
        const char *raw = (ctx.errbody.data != NULL) ? ctx.errbody.data : "";
        cJSON *err = cJSON_Parse(raw);
        char *message = bit_error_message(err, (int)ctx.status);
        set_error(c, "chat completions stream failed (HTTP %d): %s", (int)ctx.status, message);
        free(message);
        cJSON_Delete(err);
        result = -1;
    } else if (rc != CURLE_OK) {
        set_error(c, "chat completions stream failed: %s", curl_easy_strerror(rc));
        result = -1;
    }

    /* final callback with the last parsed chunk (may carry usage) */
    if (result == 0 && on_delta != NULL && ctx.last != NULL) {
        on_delta(NULL, ctx.last, userdata);
    }

    if (full_text_out != NULL && out_size > 0) {
        size_t n = (ctx.text.data != NULL) ? ctx.text.size : 0;
        if (n > out_size - 1) {
            n = out_size - 1;
        }
        memcpy(full_text_out, (ctx.text.data != NULL) ? ctx.text.data : "", n);
        full_text_out[n] = '\0';
    }

    cJSON_Delete(ctx.last);
    membuf_free(&ctx.line);
    membuf_free(&ctx.text);
    membuf_free(&ctx.errbody);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(payload);
    free(url);
    return result;
}
