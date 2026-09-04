/*
 * example.c — minimal BITSDK usage example (C).
 *
 * Build (from the c/ directory):   make example
 * Run:                             ./build/example [base_url] [client_key] [password]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bitsdk.h"

static void print_json(const char *label, const cJSON *value)
{
    char *text = cJSON_Print(value);
    printf("== %s ==\n%s\n", label, text ? text : "(null)");
    free(text);
}

int main(int argc, char **argv)
{
    const char *url = (argc > 1) ? argv[1] : "http://127.0.0.1:9803";
    const char *key = (argc > 2) ? argv[2] : "bit_test_key_123456";
    const char *pwd = (argc > 3) ? argv[3] : "test-pwd-1";

    bit_client *client = bit_client_new(url, key);
    if (client == NULL) {
        fprintf(stderr, "failed to create client\n");
        return 1;
    }
    bit_set_access_password(client, pwd);
    bit_set_timeout_ms(client, 30000);
    /* bit_set_key_in_query(client, true); — send the key as ?key= instead */

    /* health (no auth required) */
    cJSON *health = bit_health(client);
    if (health == NULL) {
        fprintf(stderr, "health failed: %s\n", bit_last_error(client));
        bit_client_free(client);
        return 1;
    }
    print_json("health", health);
    cJSON_Delete(health);

    /* register → invoke → remove a remote tool */
    cJSON *reg = bit_register_tool(client, "example_tool", "demo tool",
                                   "{\"type\":\"object\",\"properties\":{}}",
                                   "http://127.0.0.1:9000/hook");
    if (reg != NULL) {
        print_json("register", reg);
        const cJSON *tool = cJSON_GetObjectItemCaseSensitive(reg, "tool");
        const cJSON *id = cJSON_IsObject(tool)
            ? cJSON_GetObjectItemCaseSensitive(tool, "id")
            : NULL;
        if (cJSON_IsString(id)) {
            cJSON *inv = bit_invoke_tool(client, id->valuestring, "{\"hello\":\"world\"}");
            print_json("invoke", inv);
            cJSON_Delete(inv);
            cJSON *rem = bit_remove_tool(client, id->valuestring);
            cJSON_Delete(rem);
        }
        cJSON_Delete(reg);
    } else {
        fprintf(stderr, "register failed: %s\n", bit_last_error(client));
    }

    /* chat */
    cJSON *chat = bit_chat(client, "hello from the C SDK", NULL);
    if (chat != NULL) {
        const cJSON *reply = cJSON_GetObjectItemCaseSensitive(chat, "reply");
        if (cJSON_IsString(reply)) {
            printf("== chat reply ==\n%s\n", reply->valuestring);
        }
        cJSON_Delete(chat);
    }

    /* OpenAI-compatible streaming: collect the full text, print deltas live */
    const char *body =
        "{\"model\":\"ignored\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
    char text[1024] = { 0 };
    int rc = bit_chat_completions_stream(
        client, body,
        /* on_delta */ NULL, /* userdata */ NULL,
        text, sizeof(text));
    if (rc == 0) {
        printf("== stream assembled ==\n%s\n", text);
    } else {
        fprintf(stderr, "stream failed: %s\n", bit_last_error(client));
    }

    /* HTTP errors are returned as parsed bodies — inspect the status yourself */
    int status = 0;
    cJSON *bad = bit_request(client, "GET", "/api/debug/sessions/nope", NULL, &status);
    if (bad != NULL && status >= 400) {
        char *message = bit_error_message(bad, status);
        printf("== expected error (HTTP %d) ==\n%s\n", status, message);
        free(message);
        cJSON_Delete(bad);
    }

    bit_client_free(client);
    return 0;
}
