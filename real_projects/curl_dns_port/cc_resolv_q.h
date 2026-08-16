/* C ABI for the Concurrent-C DNS resolve queue used by asyn_thrdd.c.
 * Drop-in replacement for the Curl_thrdq surface that asyn-thrdd needs.
 *
 * Returns curl CURLcode values as int so the CC TU need not parse curl.h.
 * Function types are pointer typedefs (ccc parser-friendly).
 */
#ifndef CC_CURL_RESOLV_Q_H
#define CC_CURL_RESOLV_Q_H

#include <stdbool.h>
#include <stdint.h>

typedef struct CcResolvQ CcResolvQ;

typedef void (*CcResolvQItemFree)(void *item);
typedef void (*CcResolvQItemProcess)(void *item);
typedef void (*CcResolvQEvent)(void *user_data);
typedef bool (*CcResolvQItemMatch)(void *item, void *match_data);

/* Numeric CURLcode values (curl/curl.h) - keep in sync with curl pin. */
#ifndef CC_CURLE_OK
#define CC_CURLE_OK 0
#define CC_CURLE_FAILED_INIT 2
#define CC_CURLE_OUT_OF_MEMORY 27
#define CC_CURLE_OPERATION_TIMEDOUT 28
#define CC_CURLE_SEND_ERROR 55
#define CC_CURLE_RECV_ERROR 56
#define CC_CURLE_AGAIN 81
#endif

int CcResolvQ_create(CcResolvQ **out, const char *name, uint32_t max_len,
                     uint32_t min_threads, uint32_t max_threads,
                     uint32_t idle_time_ms, CcResolvQItemFree fn_free,
                     CcResolvQItemProcess fn_process, CcResolvQEvent fn_event,
                     void *user_data);

void CcResolvQ_destroy(CcResolvQ *q, bool join);

int CcResolvQ_send(CcResolvQ *q, void *item, const char *description,
                   int64_t timeout_ms);

int CcResolvQ_recv(CcResolvQ *q, void **pitem);

void CcResolvQ_clear(CcResolvQ *q, CcResolvQItemMatch fn_match,
                     void *match_data);

int CcResolvQ_set_props(CcResolvQ *q, uint32_t max_len, uint32_t min_threads,
                        uint32_t max_threads, uint32_t idle_time_ms);

#endif /* CC_CURL_RESOLV_Q_H */
