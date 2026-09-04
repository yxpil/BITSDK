/*
 * test_smoke.c — smoke test for the C BITSDK against the fake BIT server
 * (test/fake_bit_server.js on http://127.0.0.1:9803).
 *
 * Env: BIT_URL (default http://127.0.0.1:9803), BIT_KEY (default
 * bit_test_key_123456), BIT_PWD (default test-pwd-1).
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "bitsdk.h"

static const char *URL = "http://127.0.0.1:9803";
static const char *KEY = "bit_test_key_123456";
static const char *PWD = "test-pwd-1";

static int g_passed = 0;
static int g_failed = 0;
static const char *g_name = "";
static char g_msg[512];
static jmp_buf g_jmp;

#define FAIL(...)                                                              \
    do {                                                                       \
        snprintf(g_msg, sizeof(g_msg), __VA_ARGS__);                           \
        longjmp(g_jmp, 1);                                                     \
    } while (0)

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            FAIL(__VA_ARGS__);                                                 \
        }                                                                      \
    } while (0)

/* obj["a"]["b"] path lookup with "[N]" array indexing; NULL when missing */
static const cJSON *get(const cJSON *obj, const char *path)
{
    const cJSON *cur = obj;
    const char *p = path;

    while (cur != NULL && p != NULL && *p != '\0') {
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        char key[128];
        if (len >= sizeof(key)) {
            return NULL;
        }
        memcpy(key, p, len);
        key[len] = '\0';

        /* split "name[idx1][idx2]..." into the object key and index chain */
        char *bracket = strchr(key, '[');
        if (bracket != NULL) {
            *bracket = '\0';
        }
        cur = cJSON_GetObjectItemCaseSensitive(cur, key);
        if (cur == NULL) {
            return NULL;
        }

        if (bracket != NULL) {
            char *idx = bracket + 1;
            while (idx != NULL) {
                cur = cJSON_GetArrayItem(cur, (int)strtol(idx, NULL, 10));
                if (cur == NULL) {
                    return NULL;
                }
                idx = strchr(idx, ']');
                if (idx == NULL) {
                    break;
                }
                idx++; /* past ']' */
                if (*idx == '[') {
                    idx++; /* past '[', next index follows */
                } else {
                    break;
                }
            }
        }

        p = dot ? dot + 1 : NULL;
    }
    return cur;
}

static const char *get_str(const cJSON *obj, const char *path)
{
    const cJSON *item = get(obj, path);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

/* ---- streaming collector ---- */
typedef struct {
    char text[2048];
    int chunks;
    cJSON *usage_copy; /* duplicated inside the callback (final chunk is only valid during the call) */
} collector;

static void on_delta(const char *chunk, const cJSON *final_chunk, void *ud)
{
    collector *c = (collector *)ud;
    if (chunk != NULL) {
        c->chunks++;
        strncat(c->text, chunk, sizeof(c->text) - strlen(c->text) - 1);
    } else if (final_chunk != NULL) {
        const cJSON *usage = cJSON_GetObjectItemCaseSensitive(final_chunk, "usage");
        if (usage != NULL) {
            c->usage_copy = cJSON_Duplicate(usage, 1);
        }
    }
}

/* ---- tests ---- */

static void t_health(bit_client *c)
{
    cJSON *res = bit_health(c);
    CHECK(res != NULL, "transport failed: %s", bit_last_error(c));
    CHECK(cJSON_IsTrue(get(res, "ok")), "health.ok should be true");
    CHECK(get_str(res, "version") != NULL, "health.version missing");
    cJSON_Delete(res);
}

static void t_register_invoke_remove(bit_client *c)
{
    char name[64];
    snprintf(name, sizeof(name), "c_echo_%lx_%d",
             (unsigned long)time(NULL), (int)getpid());

    cJSON *reg = bit_register_tool(c, name, "smoke test tool",
                                   "{\"type\":\"object\",\"properties\":{}}",
                                   "http://127.0.0.1:9000/hook");
    CHECK(reg != NULL, "register transport failed: %s", bit_last_error(c));
    const char *id = get_str(reg, "tool.id");
    CHECK(id != NULL, "no tool id in %s", cJSON_PrintUnformatted(reg));

    cJSON *inv = bit_invoke_tool(c, id, "{\"ping\":\"pong\"}");
    CHECK(inv != NULL, "invoke transport failed: %s", bit_last_error(c));
    const char *via = get_str(inv, "result.via");
    CHECK(via != NULL && strcmp(via, name) == 0, "result.via should be %s, got %s", name,
          via ? via : "null");

    cJSON *rem = bit_remove_tool(c, id);
    CHECK(rem != NULL, "remove transport failed: %s", bit_last_error(c));
    const char *removed = get_str(rem, "removed");
    CHECK(removed != NULL && strcmp(removed, id) == 0, "removed should be %s, got %s", id,
          removed ? removed : "null");

    cJSON_Delete(reg);
    cJSON_Delete(inv);
    cJSON_Delete(rem);
}

static void t_chat(bit_client *c)
{
    char message[64];
    snprintf(message, sizeof(message), "c hello %lx_%d",
             (unsigned long)time(NULL), (int)getpid());
    cJSON *res = bit_chat(c, message, NULL);
    CHECK(res != NULL, "transport failed: %s", bit_last_error(c));
    const char *reply = get_str(res, "reply");
    CHECK(reply != NULL, "no reply in %s", cJSON_PrintUnformatted(res));
    char prefix[128];
    snprintf(prefix, sizeof(prefix), "fake reply to: %s", message);
    CHECK(strncmp(reply, prefix, strlen(prefix)) == 0, "unexpected reply: %s", reply);
    cJSON_Delete(res);
}

static void t_debug_state(bit_client *c)
{
    cJSON *res = bit_debug_state(c);
    CHECK(res != NULL, "transport failed: %s", bit_last_error(c));
    CHECK(get_str(res, "ai.active.model") != NULL, "ai.active.model missing");
    cJSON_Delete(res);
}

static void t_debug_sessions(bit_client *c)
{
    cJSON *res = bit_debug_sessions(c);
    CHECK(res != NULL, "transport failed: %s", bit_last_error(c));
    const cJSON *sessions = get(res, "sessions");
    CHECK(cJSON_IsArray(sessions) && cJSON_GetArraySize(sessions) >= 1, "expected >=1 session");
    cJSON_Delete(res);
}

static void t_debug_session(bit_client *c)
{
    cJSON *res = bit_debug_session(c, "default");
    CHECK(res != NULL, "transport failed: %s", bit_last_error(c));
    const cJSON *messages = get(res, "messages");
    CHECK(cJSON_IsArray(messages) && cJSON_GetArraySize(messages) >= 1,
          "default session has no messages");
    cJSON_Delete(res);
}

static void t_mcp_initialize(bit_client *c)
{
    const char *payload =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
        "{\"protocolVersion\":\"2025-03-26\",\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"c-smoke\",\"version\":\"1.0\"}}}";
    cJSON *res = bit_mcp(c, payload);
    CHECK(res != NULL, "transport failed: %s", bit_last_error(c));
    const char *name = get_str(res, "result.serverInfo.name");
    CHECK(name != NULL && strcmp(name, "fake-bit") == 0, "serverInfo.name should be fake-bit, got %s",
          name ? name : "null");
    cJSON_Delete(res);
}

static void t_models(bit_client *c)
{
    cJSON *res = bit_models(c);
    CHECK(res != NULL, "transport failed: %s", bit_last_error(c));
    const cJSON *data = get(res, "data");
    CHECK(cJSON_IsArray(data) && cJSON_GetArraySize(data) == 2, "expected 2 models");
    cJSON_Delete(res);
}

static void t_chat_completions(bit_client *c)
{
    const char *body =
        "{\"model\":\"fake-model\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
    cJSON *res = bit_chat_completions(c, body);
    CHECK(res != NULL, "transport failed: %s", bit_last_error(c));
    const char *content = get_str(res, "choices[0].message.content");
    CHECK(content != NULL && strcmp(content, "你好，世界!") == 0,
          "content should be 你好，世界!, got %s", content ? content : "null");
    cJSON_Delete(res);
}

static void t_chat_completions_stream(bit_client *c)
{
    const char *body =
        "{\"model\":\"fake-model\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
    char text[2048];
    memset(text, 0, sizeof(text));
    collector col;
    memset(&col, 0, sizeof(col));
    int rc = bit_chat_completions_stream(c, body, on_delta, &col, text, sizeof(text));
    CHECK(rc == 0, "stream failed: %s", bit_last_error(c));
    CHECK(strcmp(text, "你好，世界!") == 0, "assembled text should be 你好，世界!, got %s", text);
    CHECK(col.chunks == 3, "expected 3 content chunks, got %d", col.chunks);
    CHECK(col.usage_copy != NULL, "final chunk should carry usage");
    cJSON_Delete(col.usage_copy);
}

static void t_wrong_key_401(bit_client *c)
{
    (void)c;
    bit_client *bad = bit_client_new(URL, "definitely-wrong-key");
    CHECK(bad != NULL, "client alloc failed");
    bit_set_access_password(bad, PWD);
    cJSON *body = cJSON_Parse("{\"message\":\"should fail\"}");
    int status = 0;
    cJSON *res = bit_request(bad, "POST", "/api/chat", body, &status);
    CHECK(res != NULL, "expected parsed error body, got transport failure: %s", bit_last_error(bad));
    CHECK(status == 401, "expected status 401, got %d", status);
    char *message = bit_error_message(res, status);
    CHECK(message != NULL && strstr(message, "API Key") != NULL,
          "expected key error message, got: %s", message);
    free(message);
    cJSON_Delete(res);
    cJSON_Delete(body);
    bit_client_free(bad);
}

static void t_missing_password_401(bit_client *c)
{
    (void)c;
    bit_client *nopwd = bit_client_new(URL, KEY); /* no access password set */
    CHECK(nopwd != NULL, "client alloc failed");
    cJSON *body = cJSON_Parse("{\"message\":\"should fail\"}");
    int status = 0;
    cJSON *res = bit_request(nopwd, "POST", "/api/chat", body, &status);
    CHECK(res != NULL, "expected parsed error body, got transport failure: %s", bit_last_error(nopwd));
    CHECK(status == 401, "expected status 401, got %d", status);
    char *message = bit_error_message(res, status);
    CHECK(message != NULL && strstr(message, "访问密码") != NULL,
          "expected password error text, got: %s", message);
    free(message);
    cJSON_Delete(res);
    cJSON_Delete(body);
    bit_client_free(nopwd);
}

static void t_key_in_query(bit_client *c)
{
    (void)c;
    bit_client *q = bit_client_new(URL, KEY);
    CHECK(q != NULL, "client alloc failed");
    bit_set_access_password(q, PWD);
    bit_set_key_in_query(q, true);

    cJSON *res = bit_chat(q, "query key works", NULL);
    CHECK(res != NULL, "transport failed: %s", bit_last_error(q));
    const char *reply = get_str(res, "reply");
    CHECK(reply != NULL && strncmp(reply, "fake reply to: ", 15) == 0, "unexpected reply: %s",
          reply ? reply : "null");
    cJSON_Delete(res);

    cJSON *state = bit_debug_state(q);
    CHECK(state != NULL, "debug_state via ?key= failed: %s", bit_last_error(q));
    CHECK(get(state, "ai") != NULL, "debugState.ai missing");
    cJSON_Delete(state);
    bit_client_free(q);
}

/* ---- harness ---- */

static bit_client *g_client = NULL;

static void run(const char *name, void (*fn)(bit_client *))
{
    g_name = name;
    g_msg[0] = '\0';
    if (setjmp(g_jmp) == 0) {
        fn(g_client);
        g_passed++;
        printf("[PASS] %s\n", name);
    } else {
        g_failed++;
        printf("[FAIL] %s: %s\n", name, g_msg[0] ? g_msg : "assertion failed");
    }
    fflush(stdout);
}

static const char *env_or(const char *name, const char *fallback)
{
    const char *value = getenv(name);
    return (value != NULL && value[0] != '\0') ? value : fallback;
}

int main(void)
{
    URL = env_or("BIT_URL", URL);
    KEY = env_or("BIT_KEY", KEY);
    PWD = env_or("BIT_PWD", PWD);

    g_client = bit_client_new(URL, KEY);
    if (g_client == NULL) {
        fprintf(stderr, "failed to create client\n");
        return 1;
    }
    bit_set_access_password(g_client, PWD);
    bit_set_timeout_ms(g_client, 30000);

    run("health", t_health);
    run("register_invoke_remove", t_register_invoke_remove);
    run("chat", t_chat);
    run("debug_state", t_debug_state);
    run("debug_sessions", t_debug_sessions);
    run("debug_session", t_debug_session);
    run("mcp_initialize", t_mcp_initialize);
    run("models", t_models);
    run("chat_completions", t_chat_completions);
    run("chat_completions_stream", t_chat_completions_stream);
    run("wrong_key_401", t_wrong_key_401);
    run("missing_password_401", t_missing_password_401);
    run("key_in_query", t_key_in_query);

    bit_client_free(g_client);

    printf("\npassed=%d failed=%d\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
