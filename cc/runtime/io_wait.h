#ifndef CC_RUNTIME_IO_WAIT_H
#define CC_RUNTIME_IO_WAIT_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef struct cc__io_owned_watcher cc__io_owned_watcher;
typedef struct cc__wait_select_group cc__wait_select_group;

typedef struct cc__io_wait_select_handle {
    int kind;
    void* ptr;
} cc__io_wait_select_handle;

int cc__io_wait_ready(int fd, short events);
int cc__io_wait_fd(int fd, short events);
int cc__io_wait_fd_deadline(int fd, short events, const struct timespec* abs_deadline);
void cc__io_wait_forget_fd(int fd);
cc__io_owned_watcher* cc__io_watcher_create(int fd);
void cc__io_watcher_destroy(cc__io_owned_watcher* watcher);
/* Phase 1 of a two-phase close: mark closing and wake every fiber parked on
 * the watcher (they return ECANCELED). Frees nothing; the fd stays open.
 * The last waiter out (or the closer, if none) calls destroy + close(fd). */
void cc__io_watcher_cancel_waiters(cc__io_owned_watcher* watcher);
int cc__io_watcher_closing(const cc__io_owned_watcher* watcher);

/* Signal sinks (CCSignal backing). Delivery runs on the I/O waiter thread:
 * kqueue EVFILT_SIGNAL, or a self-pipe drained by the poll thread. */
typedef struct cc__io_signal_sink cc__io_signal_sink;
cc__io_signal_sink* cc__io_signal_sink_create(void);
int  cc__io_signal_sink_add(cc__io_signal_sink* k, int signo);   /* 0 or errno */
void cc__io_signal_sink_close(cc__io_signal_sink* k);            /* wakes waiter (ECANCELED) */
int  cc__io_signal_sink_take(cc__io_signal_sink* k);             /* signo or 0 */
int  cc__io_signal_sink_wait(cc__io_signal_sink* k, const struct timespec* abs_deadline, int* out_signo);
int cc__io_watcher_wait(cc__io_owned_watcher* watcher, short events);
int cc__io_watcher_wait_deadline(cc__io_owned_watcher* watcher,
                                 short events,
                                 const struct timespec* abs_deadline);
int cc__io_wait_select_publish(cc__io_owned_watcher* watcher,
                               short events,
                               uint64_t wait_ticket,
                               cc__wait_select_group* group,
                               size_t select_index,
                               cc__io_wait_select_handle* out_handle);
void cc__io_wait_select_finish(cc__io_wait_select_handle* handle);

#endif /* CC_RUNTIME_IO_WAIT_H */
