/* Concurrent-C Curl_thrdq — same C ABI as curl lib/thrdqueue.h.
 * CURLcode is int so this TU does not include curl_setup.h.
 */
#ifndef CC_CURL_THRDQUEUE_H
#define CC_CURL_THRDQUEUE_H

#include <stdbool.h>
#include <stdint.h>

struct curl_thrdq;
struct Curl_easy;

typedef enum {
    CURL_THRDQ_EV_ITEM_DONE
} Curl_thrdq_event;

typedef void (*Curl_thrdq_ev_cb)(const struct curl_thrdq *tqueue,
                                 Curl_thrdq_event ev, void *user_data);
typedef void (*Curl_thrdq_item_process_cb)(void *item);
typedef void (*Curl_thrdq_item_free_cb)(void *item);
typedef bool (*Curl_thrdq_item_match_cb)(void *item, void *match_data);

#ifndef CC_CURLE_OK
#define CC_CURLE_OK 0
#define CC_CURLE_FAILED_INIT 2
#define CC_CURLE_OUT_OF_MEMORY 27
#define CC_CURLE_OPERATION_TIMEDOUT 28
#define CC_CURLE_BAD_FUNCTION_ARGUMENT 43
#define CC_CURLE_SEND_ERROR 55
#define CC_CURLE_RECV_ERROR 56
#define CC_CURLE_AGAIN 81
#endif

int Curl_thrdq_create(struct curl_thrdq **ptqueue, const char *name,
                      uint32_t max_len, uint32_t min_threads,
                      uint32_t max_threads, uint32_t idle_time_ms,
                      Curl_thrdq_item_free_cb fn_free,
                      Curl_thrdq_item_process_cb fn_process,
                      Curl_thrdq_ev_cb fn_event, void *user_data);

void Curl_thrdq_destroy(struct curl_thrdq *tqueue, bool join);

int Curl_thrdq_send(struct curl_thrdq *tqueue, void *item,
                    const char *description, int64_t timeout_ms);

int Curl_thrdq_recv(struct curl_thrdq *tqueue, void **pitem);

void Curl_thrdq_clear(struct curl_thrdq *tqueue,
                      Curl_thrdq_item_match_cb fn_match, void *match_data);

int Curl_thrdq_await_done(struct curl_thrdq *tqueue, uint32_t timeout_ms);

int Curl_thrdq_set_props(struct curl_thrdq *tqueue, uint32_t max_len,
                         uint32_t min_threads, uint32_t max_threads,
                         uint32_t idle_time_ms);

void Curl_thrdq_trace(struct curl_thrdq *tqueue, struct Curl_easy *data);

#endif /* CC_CURL_THRDQUEUE_H */
