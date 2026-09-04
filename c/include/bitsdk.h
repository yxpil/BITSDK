/*
 * bitsdk.h — C client for the BIT HTTP API.
 *
 * This is a thin libcurl + cJSON based SDK. The binding contract is
 * docs/API.md (see ../docs/API.md). All public symbols are prefixed bit_.
 *
 * Authentication model (docs/API.md):
 *   - Client Key: sent as "Authorization: Bearer <key>", or as "?key=<key>"
 *     when bit_set_key_in_query() is enabled (for clients that cannot set
 *     headers).
 *   - Access password: sent as "X-Access-Password: <password>" when set with
 *     bit_set_access_password(). Required on /api paths (except /api/health),
 *     exempt on /v1 paths and /mcp.
 *
 * Error conventions:
 *   - Transport failures (DNS, connect, timeout, invalid JSON) return NULL
 *     with bit_last_error() set to an English description.
 *   - HTTP error responses are NOT NULL returns: the wrapper returns the
 *     parsed body and the caller inspects the HTTP status (pass a
 *     status_out / use bit_request directly) and the cJSON "error" field.
 *     bit_error_message() extracts a human-readable message.
 */
#ifndef BITSDK_H
#define BITSDK_H

#include <stdbool.h>
#include <stddef.h>

#include "cjson/cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque client handle. */
typedef struct bit_client bit_client;

/* ---------- lifecycle ---------- */

/* Creates a client. base_url like "http://127.0.0.1:7777" (trailing slashes
 * are tolerated); client_key may be NULL when the server has no key
 * configured. Returns NULL on allocation failure. */
bit_client *bit_client_new(const char *base_url, const char *client_key);

/* Frees a client and its internal buffers. NULL is tolerated. */
void bit_client_free(bit_client *c);

/* ---------- options ---------- */

/* Sets the access password sent as "X-Access-Password" (NULL clears it). */
void bit_set_access_password(bit_client *c, const char *password);

/* Per-request timeout in milliseconds (default 30000). */
void bit_set_timeout_ms(bit_client *c, long timeout_ms);

/* When true, the Client Key is sent as "?key=<key>" instead of the
 * Authorization header (for clients that cannot set headers). */
void bit_set_key_in_query(bit_client *c, bool key_in_query);

/* English description of the last transport failure ("" when none).
 * The pointer stays valid until the next request on this client. */
const char *bit_last_error(bit_client *c);

/* ---------- core transport ---------- */

/* Performs one HTTP request against path (e.g. "/api/tools"). Adds auth
 * (Authorization: Bearer or ?key=), X-Access-Password when set, and
 * Content-Type: application/json when a body is given.
 *
 * Returns the parsed response body as cJSON (caller frees with cJSON_Delete)
 * or NULL on transport failure (see bit_last_error()). HTTP error responses
 * (4xx/5xx) are returned normally — inspect *status_out and the body's
 * "error" field. *status_out (when non-NULL) receives the HTTP status code
 * (0 on transport failure). */
cJSON *bit_request(bit_client *c, const char *method, const char *path,
                   const cJSON *body_or_null, int *status_out);

/* Convenience: extracts an error message from a response body.
 * {"error":{"message":"..."}} -> inner message; {"error":"..."} -> string;
 * otherwise "HTTP <status>". Returns a malloc'd NUL-terminated string the
 * caller frees (never NULL). */
char *bit_error_message(const cJSON *response_or_null, int status);

/* ---------- convenience wrappers ----------
 * Each returns the parsed JSON response (caller frees) or NULL on transport
 * failure. HTTP errors (409, 404, 400, 401, ...) still return the parsed
 * body — check the status via bit_request or inspect the "error" field. */

/* GET /api/health */
cJSON *bit_health(bit_client *c);

/* GET /api/tools */
cJSON *bit_list_tools(bit_client *c);

/* POST /api/tools — registers a remote tool. parameters_json may be NULL
 * (defaults to {"type":"object","properties":{}}). */
cJSON *bit_register_tool(bit_client *c, const char *name, const char *description,
                         const char *parameters_json, const char *url);

/* DELETE /api/tools/{id} */
cJSON *bit_remove_tool(bit_client *c, const char *tool_id);

/* POST /api/tools/{id}/invoke — params_json may be NULL (treated as {}). */
cJSON *bit_invoke_tool(bit_client *c, const char *tool_id, const char *params_json);

/* POST /api/chat — session_id_or_null selects/creates a session (NULL uses
 * the currently active session). */
cJSON *bit_chat(bit_client *c, const char *message, const char *session_id_or_null);

/* GET /api/audit */
cJSON *bit_audit(bit_client *c);

/* GET /api/debug/state */
cJSON *bit_debug_state(bit_client *c);

/* GET /api/debug/sessions */
cJSON *bit_debug_sessions(bit_client *c);

/* GET /api/debug/sessions/{id} */
cJSON *bit_debug_session(bit_client *c, const char *session_id);

/* GET /api/debug/mcp */
cJSON *bit_debug_mcp(bit_client *c);

/* POST /mcp — raw JSON-RPC 2.0 payload (BIT acts as MCP server). */
cJSON *bit_mcp(bit_client *c, const char *jsonrpc_body_json);

/* GET /v1/models */
cJSON *bit_models(bit_client *c);

/* POST /v1/chat/completions — non-streaming OpenAI-compatible call. */
cJSON *bit_chat_completions(bit_client *c, const char *body_json);

/* ---------- streaming (SSE) ---------- */

/* POST /v1/chat/completions with "stream":true, reading the text/event-stream
 * response. Bytes are accumulated and lines split ONLY on '\n' (safe for
 * multi-byte UTF-8 split across TCP chunks).
 *
 * For every content chunk the callback is called as
 * on_delta(chunk, NULL, ud); when the stream ends it is called once as
 * on_delta(NULL, final_chunk, ud) with the last parsed chunk (may carry
 * "usage"). Both arguments may be NULL if the stream carried no chunks.
 * final_chunk is owned by the SDK and only valid for the duration of the
 * callback — duplicate it with cJSON_Duplicate() to keep it.
 *
 * The fully assembled text is copied into full_text_out (truncated to
 * out_size-1 bytes, always NUL-terminated; may be NULL to discard).
 *
 * Returns 0 on success, -1 on failure (bit_last_error() describes it). */
int bit_chat_completions_stream(bit_client *c, const char *body_json,
                                void (*on_delta)(const char *chunk, const cJSON *final_chunk, void *ud),
                                void *userdata, char *full_text_out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* BITSDK_H */
