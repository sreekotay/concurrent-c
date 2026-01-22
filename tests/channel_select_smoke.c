#define CC_ENABLE_SHORT_NAMES
#include <ccc/std/prelude.cch>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    CCChan* ch;
    int value;
    unsigned int delay_ms;
} SenderArgs;

static void* sender_thread(void* arg) {
    SenderArgs* a = (SenderArgs*)arg;
    cc_sleep_ms(a->delay_ms);
    int v = a->value;
    cc_chan_send(a->ch, &v, sizeof(int));
    return NULL;
}

int main(void) {
    CCExec* ex = cc_exec_init(2, 16);
    if (!ex) return 1;

    // Ready case
    CCChan* ch = cc_chan_create(1);
    int recv_val = 0;
    size_t ready_idx = (size_t)-1;
    CCAsyncHandle h;
    CCChanMatchCase cases[] = { CC_CHAN_MATCH_RECV_CASE(ch, &recv_val, int) };
    CCDeadline d = cc_deadline_after_ms(500);

    int rc = cc_chan_match_select_async(ex, cases, 1, &ready_idx, &h, &d);
    if (rc != 0) return 2;

    SenderArgs args = { .ch = ch, .value = 42, .delay_ms = 50 };
    pthread_t t;
    pthread_create(&t, NULL, sender_thread, &args);

    int wait_err = cc_async_wait(&h);
    pthread_join(t, NULL);
    if (wait_err != 0 || ready_idx != 0 || recv_val != 42) {
        fprintf(stderr, "ready case failed wait_err=%d ready_idx=%zu recv=%d\n", wait_err, ready_idx, recv_val);
        return 3;
    }
    cc_async_handle_free(&h);

    // Timeout case
    size_t ready_idx2 = (size_t)-1;
    CCAsyncHandle h2;
    CCDeadline short_d = cc_deadline_after_ms(50);
    rc = cc_chan_match_select_async(ex, cases, 1, &ready_idx2, &h2, &short_d);
    if (rc != 0) return 4;
    int wait_err2 = cc_async_wait(&h2);
    cc_async_handle_free(&h2);
    cc_chan_free(ch);

    if (wait_err2 != ETIMEDOUT) {
        fprintf(stderr, "timeout case failed wait_err2=%d\n", wait_err2);
        return 5;
    }

    // Closed channel should surface EPIPE and set ready index
    CCChan* ch_closed = cc_chan_create(1);
    CCChanMatchCase cases_closed[] = { CC_CHAN_MATCH_RECV_CASE(ch_closed, &recv_val, int) };
    size_t ready_idx3 = (size_t)-1;
    CCAsyncHandle h3;
    cc_chan_close(ch_closed);
    rc = cc_chan_match_select_async(ex, cases_closed, 1, &ready_idx3, &h3, NULL);
    if (rc != 0) { fprintf(stderr, "closed case submit rc=%d\n", rc); return 6; }
    int wait_err3 = cc_async_wait(&h3);
    cc_async_handle_free(&h3);
    cc_chan_free(ch_closed);
    if (wait_err3 != EPIPE || ready_idx3 != 0) {
        fprintf(stderr, "closed case wait_err3=%d ready_idx3=%zu\n", wait_err3, ready_idx3);
        return 7;
    }

    // Future-based helper ready case
    CCChan* ch2 = cc_chan_create(1);
    int recv_val_fut = 0;
    size_t ready_idx_fut = (size_t)-1;
    CCChanMatchCase cases_fut[] = { CC_CHAN_MATCH_RECV_CASE(ch2, &recv_val_fut, int) };
    CCFuture fut;
    CCDeadline fut_d = cc_deadline_after_ms(500);
    rc = cc_chan_match_select_future(ex, cases_fut, 1, &ready_idx_fut, &fut, &fut_d);
    if (rc != 0) { fprintf(stderr, "future submit rc=%d\n", rc); return 8; }
    SenderArgs args2 = { .ch = ch2, .value = 99, .delay_ms = 20 };
    pthread_t t2;
    pthread_create(&t2, NULL, sender_thread, &args2);
    CCFutureStatus fut_st = cc_future_wait(&fut, NULL);
    pthread_join(t2, NULL);
    cc_future_free(&fut);
    if (fut_st != CC_FUTURE_READY || ready_idx_fut != 0 || recv_val_fut != 99) {
        fprintf(stderr, "future wait fut_st=%d ready_idx_fut=%zu recv_val_fut=%d\n", (int)fut_st, ready_idx_fut, recv_val_fut);
        return 9;
    }
    cc_chan_free(ch2);

    cc_exec_shutdown(ex);
    cc_exec_free(ex);

    printf("channel select smoke ok\\n");
    return 0;
}

