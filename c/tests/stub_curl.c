/*
 * stub_curl.c — see stubinclude/curl/curl.h. Offline test transport:
 *  - records the URL passed via CURLOPT_URL (for path-injection assertions)
 *  - performs nothing (no network)
 */
#include <curl/curl.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static char g_captured[1024] = "";

const char *bit_test_captured_url(void) { return g_captured; }
void bit_test_reset_capture(void) { g_captured[0] = '\0'; }

CURLcode curl_global_init(long flags) { (void)flags; return CURLE_OK; }

CURL *curl_easy_init(void) {
    static char dummy[1];
    return (CURL *)dummy;
}

CURLcode curl_easy_setopt(CURL *handle, CURLoption option, ...) {
    (void)handle;
    va_list ap;
    va_start(ap, option);
    if (option == CURLOPT_URL) {
        const char *u = va_arg(ap, const char *);
        if (u != NULL) {
            snprintf(g_captured, sizeof(g_captured), "%s", u);
        }
    }
    va_end(ap);
    return CURLE_OK;
}

CURLcode curl_easy_perform(CURL *handle) {
    (void)handle;
    /* No network, no response body: bitsdk.c treats this as CURLE_OK with an
     * empty body (returns an empty object). Enough for offline URL/path checks. */
    return CURLE_OK;
}

void curl_easy_cleanup(CURL *handle) { (void)handle; }

CURLcode curl_easy_getinfo(CURL *handle, CURLINFO info, ...) {
    (void)handle;
    va_list ap;
    va_start(ap, info);
    if (info == CURLINFO_RESPONSE_CODE) {
        long *p = va_arg(ap, long *);
        if (p != NULL) {
            *p = 0; /* no HTTP response in offline stub */
        }
    }
    va_end(ap);
    return CURLE_OK;
}

const char *curl_easy_strerror(CURLcode code) {
    (void)code;
    return "offline-stub: no transport";
}

struct curl_slist *curl_slist_append(struct curl_slist *list, const char *string) {
    (void)list;
    (void)string;
    return (struct curl_slist *)0x1; /* non-NULL sentinel */
}

void curl_slist_free_all(struct curl_slist *list) { (void)list; }
