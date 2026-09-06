/*
 * CCSignal — fiber-side signal delivery. Thin owner over io_wait's signal
 * sink; the routing (EVFILT_SIGNAL / self-pipe) lives in io_wait.c.
 */
#include <ccc/std/signal.cch>

#include <errno.h>
#include <stddef.h>

#include "fiber_internal.h"
#include "io_wait.h"

CCResult_CCSignal_CCIoError cc_signal_watch_list(const int* sigs, size_t n) {
    CCSignal s = {.impl = NULL};
    if (!sigs || n == 0) {
        return cc_err_CCResult_CCSignal_CCIoError(cc_io_from_errno(EINVAL));
    }
    for (size_t i = 0; i < n; i++) {
        if (sigs[i] < 1 || sigs[i] > 64) {
            return cc_err_CCResult_CCSignal_CCIoError(cc_io_from_errno(EINVAL));
        }
    }
    cc__io_signal_sink* k = cc__io_signal_sink_create();
    if (!k) {
        return cc_err_CCResult_CCSignal_CCIoError(cc_io_from_errno(ENOMEM));
    }
    for (size_t i = 0; i < n; i++) {
        int err = cc__io_signal_sink_add(k, sigs[i]);
        if (err != 0) {
            cc__io_signal_sink_close(k);
            return cc_err_CCResult_CCSignal_CCIoError(cc_io_from_errno(err));
        }
    }
    s.impl = k;
    return cc_ok_CCResult_CCSignal_CCIoError(s);
}

CCResult_CCSignal_CCIoError cc_signal_watch(int sig) {
    return cc_signal_watch_list(&sig, 1);
}

CCResult_CCSignal_CCIoError cc_signal_watch2(int sig_a, int sig_b) {
    int sigs[2] = {sig_a, sig_b};
    return cc_signal_watch_list(sigs, 2);
}

CCResult_int_CCIoError cc_signal_wait(CCSignal* s) {
    if (!s || !s->impl) {
        return cc_err_CCResult_int_CCIoError(cc_io_from_errno(EINVAL));
    }
    int signo = 0;
    int err = cc__io_signal_sink_wait((cc__io_signal_sink*)s->impl, NULL, &signo);
    if (err != 0) {
        return cc_err_CCResult_int_CCIoError(cc_io_from_errno(err));
    }
    return cc_ok_CCResult_int_CCIoError(signo);
}

int cc_signal_poll(CCSignal* s) {
    if (!s || !s->impl) return 0;
    return cc__io_signal_sink_take((cc__io_signal_sink*)s->impl);
}

void cc_signal_close(CCSignal* s) {
    if (!s || !s->impl) return;
    cc__io_signal_sink* k = (cc__io_signal_sink*)s->impl;
    s->impl = NULL;
    cc__io_signal_sink_close(k);
}
