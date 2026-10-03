/*
 * curl/curl.h — minimal libcurl replacement for OFFLINE unit testing only.
 *
 * This file shadows the real <curl/curl.h> on the include path when compiling
 * the offline harness. It lets bitsdk.c compile/link on a host without libcurl
 * so the pure-logic parts can be unit-tested with no network.
 * Test infrastructure only; never used by `make all` / `make smoke`.
 */
#ifndef BITSDK_OFFLINE_STUB_CURL_H
#define BITSDK_OFFLINE_STUB_CURL_H

#include <stddef.h>

typedef void CURL;
typedef int CURLcode;
typedef int CURLoption;
typedef int CURLINFO;

struct curl_slist {
    char *data;
    struct curl_slist *next;
};

#define CURL_GLOBAL_DEFAULT 0L
#define CURLE_OK 0

#define CURLOPT_URL              10001
#define CURLOPT_CUSTOMREQUEST    10002
#define CURLOPT_HTTPHEADER       10003
#define CURLOPT_TIMEOUT_MS       10004
#define CURLOPT_WRITEFUNCTION    10005
#define CURLOPT_WRITEDATA        10006
#define CURLOPT_NOSIGNAL         10007
#define CURLOPT_ACCEPT_ENCODING  10008
#define CURLOPT_POSTFIELDS       10009
#define CURLOPT_POSTFIELDSIZE    10010
#define CURLOPT_POST             10011

#define CURLINFO_RESPONSE_CODE 20001

#define LIBCURL_VERSION_NUM 0x072000

CURLcode curl_global_init(long flags);
CURL *curl_easy_init(void);
CURLcode curl_easy_setopt(CURL *handle, CURLoption option, ...);
CURLcode curl_easy_perform(CURL *handle);
void curl_easy_cleanup(CURL *handle);
CURLcode curl_easy_getinfo(CURL *handle, CURLINFO info, ...);
const char *curl_easy_strerror(CURLcode code);
struct curl_slist *curl_slist_append(struct curl_slist *list, const char *string);
void curl_slist_free_all(struct curl_slist *list);

/* test-only accessors (not part of real libcurl) */
const char *bit_test_captured_url(void);
void bit_test_reset_capture(void);

#endif
