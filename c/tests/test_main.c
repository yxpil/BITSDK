/*
 * test_main.c — OFFLINE unit/integration harness for the C BITSDK core logic.
 *
 * Unlike c/test/test_smoke.c (which needs the fake BIT HTTP server), this
 * harness links against stub_curl.c so it needs NO network and NO libcurl.
 * It exercises:
 *   - client lifecycle & NULL safety
 *   - bit_error_message() extraction rules
 *   - input validation on the JSON-RPC / chat-completions entry points
 *   - PATH-INJECTION behavior: tool_id / session_id are interpolated into the
 *     request URL (the stub captures it) — "../" traversal is passed through.
 *
 * Build (from c/):
 *   gcc -Iinclude -Isrc -Isrc/cjson -Itests \
 *       tests/test_main.c tests/stub_curl.c src/bitsdk.c src/cjson/cJSON.c \
 *       -o build/test_unit
 *   ./build/test_unit
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bitsdk.h"

/* test-only transport-stub accessors (see stubinclude/curl/curl.h) */
extern const char *bit_test_captured_url(void);
extern void bit_test_reset_capture(void);

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (cond) {                                                           \
            g_passed++;                                                       \
            printf("[PASS] %s\n", #cond);                                     \
        } else {                                                              \
            g_failed++;                                                       \
            printf("[FAIL] %s: ", #cond);                                     \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

int main(void)
{
    /* ---- lifecycle & NULL safety ---- */
    CHECK(bit_client_new(NULL, "k") == NULL, "NULL base_url must return NULL");
    bit_client_free(NULL); /* must not crash */
    CHECK(bit_last_error(NULL) != NULL, "last_error(NULL) returns empty string, not NULL");
    CHECK(strcmp(bit_last_error(NULL), "") == 0, "last_error(NULL) is empty");

    bit_client *c = bit_client_new("http://127.0.0.1:7777/", "client-key");
    CHECK(c != NULL, "client created");
    CHECK(strcmp(bit_last_error(c), "") == 0, "fresh client has no error");
    bit_set_access_password(c, "pw");
    bit_set_timeout_ms(c, 5000); /* positive: accepted */
    bit_set_timeout_ms(c, -1);    /* non-positive: ignored */

    /* ---- bit_error_message extraction rules ---- */
    {
        cJSON *r = cJSON_Parse("{\"error\":{\"message\":\"boom\"}}");
        char *m = bit_error_message(r, 500);
        CHECK(strcmp(m, "boom") == 0, "nested error.message extracted, got %s", m);
        free(m);
        cJSON_Delete(r);
    }
    {
        cJSON *r = cJSON_Parse("{\"error\":\"plain string err\"}");
        char *m = bit_error_message(r, 400);
        CHECK(strcmp(m, "plain string err") == 0, "string error extracted, got %s", m);
        free(m);
        cJSON_Delete(r);
    }
    {
        cJSON *r = cJSON_Parse("{\"ok\":true}");
        char *m = bit_error_message(r, 500);
        CHECK(strcmp(m, "HTTP 500") == 0, "fallback 'HTTP 500', got %s", m);
        free(m);
        cJSON_Delete(r);
    }
    {
        char *m = bit_error_message(NULL, 404);
        CHECK(strcmp(m, "HTTP 404") == 0, "NULL response fallback 'HTTP 404', got %s", m);
        free(m);
    }

    /* ---- input validation (no network reached) ---- */
    {
        cJSON *r = bit_mcp(c, "this is not json");
        CHECK(r == NULL, "bit_mcp rejects malformed JSON-RPC body");
        CHECK(strlen(bit_last_error(c)) > 0, "bit_mcp sets last_error on bad input");
    }
    {
        cJSON *r = bit_chat_completions(c, "{oops");
        CHECK(r == NULL, "bit_chat_completions rejects malformed body");
    }
    {
        cJSON *r = bit_remove_tool(c, NULL);
        CHECK(r == NULL, "bit_remove_tool rejects NULL tool_id");
    }

    /* ---- PATH INJECTION: tool_id interpolated raw into the URL ---- */
    {
        bit_test_reset_capture();
        cJSON *r = bit_remove_tool(c, "../../etc/passwd");
        const char *url = bit_test_captured_url();
        CHECK(strstr(url, "/api/tools/../../etc/passwd") != NULL,
              "captured URL contains raw traversal tool_id, got: %s", url);
        if (r) cJSON_Delete(r);
    }
    {
        bit_test_reset_capture();
        cJSON *r = bit_debug_session(c, "../admin/delete");
        const char *url = bit_test_captured_url();
        CHECK(strstr(url, "/api/debug/sessions/../admin/delete") != NULL,
              "captured URL contains raw traversal session_id, got: %s", url);
        if (r) cJSON_Delete(r);
    }

    bit_client_free(c);

    printf("\npassed=%d failed=%d\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
