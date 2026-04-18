/*
 * CCSelect runtime (v1): compose-once, reuse-many multi-source select.
 *
 * Implementation strategy (v1):
 *   Generalizes cc_chan_wait_recv_or_socket to N channels + 0..1 readable
 *   sockets using one shared CCSocketSignal anchored to the readable socket
 *   (if any). All registered channels have their recv_signal wired to this
 *   shared signal, so any send/close into a watched channel bumps the signal;
 *   the signal's socket arm catches FD readability.
 *
 *   Handlers are invoked inline; their bool return (via CCClosure{0,1}.fn
 *   returning void*) becomes cc_select_wait's Ok value.
 *
 * Deferred for v2:
 *   - send / writable cases
 *   - deadlines / cancellation (beyond what nursery does transparently)
 *   - pure-channel selects (no socket) — requires a socket-independent signal
 *   - multiple readable sockets per select
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "../include/ccc/cc_select.cch"
#include "../include/ccc/cc_channel.cch"
#include "../include/ccc/std/net.cch"

void cc_select_init(CCSelect* sel) {
    if (!sel) return;
    memset(sel, 0, sizeof(*sel));
}

void cc_select_dispose(CCSelect* sel) {
    if (!sel) return;
    if (sel->_wired) {
        /* Detach channel recv_signals so later channel use doesn't bump our
         * (about-to-be-freed) signal. */
        for (uint8_t i = 0; i < sel->count; ++i) {
            CCSelectCase* c = &sel->cases[i];
            if (c->kind == CC_SEL_RECV || c->kind == CC_SEL_CLOSE) {
                cc_chan_set_recv_signal((CCChan*)c->source, NULL);
            }
        }
        sel->_wired = 0;
    }
    for (uint8_t i = 0; i < sel->count; ++i) {
        CCSelectCase* c = &sel->cases[i];
        if (c->h.drop) c->h.drop(c->h.env);
    }
    if (sel->_sig) {
        cc_socket_signal_free(sel->_sig);
        free(sel->_sig);
        sel->_sig = NULL;
    }
    sel->_anchor_sock = NULL;
    sel->count = 0;
}

static int cc__select_append_case(CCSelect* sel, CCSelectCase c) {
    if (sel->count >= CC_SELECT_MAX_CASES) return ENOSPC;
    sel->cases[sel->count++] = c;
    return 0;
}

int cc_select_add_recv_raw(CCSelect* sel, CCChan* ch, void** out_ptr_slot, CCClosure0 handler) {
    if (!sel || !ch) return EINVAL;
    CCSelectCase c = {0};
    c.kind = CC_SEL_RECV;
    c.source = ch;
    c.out_ptr_slot = out_ptr_slot;
    c.h = handler;
    return cc__select_append_case(sel, c);
}

int cc_select_add_close_raw(CCSelect* sel, CCChan* ch, CCIoError* out_err_slot, CCClosure0 handler) {
    if (!sel || !ch) return EINVAL;
    CCSelectCase c = {0};
    c.kind = CC_SEL_CLOSE;
    c.source = ch;
    c.out_err_slot = out_err_slot;
    c.h = handler;
    return cc__select_append_case(sel, c);
}

int cc_select_add_readable(CCSelect* sel, CCSocket* sock, CCClosure0 handler) {
    if (!sel || !sock) return EINVAL;
    /* v1 limitation: at most one readable-socket case (it anchors the signal). */
    if (sel->_anchor_sock) return EINVAL;
    CCSelectCase c = {0};
    c.kind = CC_SEL_READABLE;
    c.source = sock;
    c.h = handler;
    int rc = cc__select_append_case(sel, c);
    if (rc == 0) sel->_anchor_sock = sock;
    return rc;
}

static int cc__select_lazy_wire(CCSelect* sel) {
    if (sel->_wired) return 0;
    if (!sel->_anchor_sock) {
        /* v1 needs a socket to anchor the CCSocketSignal. */
        return EINVAL;
    }
    sel->_sig = (CCSocketSignal*)malloc(sizeof(CCSocketSignal));
    if (!sel->_sig) return ENOMEM;
    cc_socket_signal_init(sel->_sig, sel->_anchor_sock);

    /* Route each watched channel's recv-signal to our shared signal so any
     * send into (or close of) the channel bumps the signal epoch. Channels
     * that already had a recv_signal set lose their prior wiring; callers
     * should own the channels exclusively for the lifetime of this select. */
    for (uint8_t i = 0; i < sel->count; ++i) {
        CCSelectCase* c = &sel->cases[i];
        if (c->kind == CC_SEL_RECV || c->kind == CC_SEL_CLOSE) {
            cc_chan_set_recv_signal((CCChan*)c->source, sel->_sig);
        }
    }
    sel->_wired = 1;
    return 0;
}

/* Poll every registered channel-recv/channel-close case exactly once. On a
 * ready case, dispatches to the handler and returns its bool result via
 * *out_fired = true; the returned Result carries Ok(bool) or Err. If no case
 * was ready, *out_fired = false and the Result is Ok(false) (sentinel). */
static CCResult_bool_CCIoError cc__select_try_channel_cases(CCSelect* sel,
                                                            bool* out_fired) {
    *out_fired = false;
    for (uint8_t i = 0; i < sel->count; ++i) {
        CCSelectCase* c = &sel->cases[i];
        if (c->kind != CC_SEL_RECV && c->kind != CC_SEL_CLOSE) continue;
        void* out_tmp = NULL;
        int rc = cc_chan_try_recv((CCChan*)c->source, &out_tmp, sizeof(void*));
        if (rc == 0) {
            /* Data ready — only fires if we have a recv case for this channel;
             * otherwise we got data we don't know how to dispatch. Prefer the
             * matching recv case (there may be a sibling close case on the
             * same channel). */
            CCSelectCase* recv_case = NULL;
            for (uint8_t j = 0; j < sel->count; ++j) {
                if (sel->cases[j].kind == CC_SEL_RECV &&
                    sel->cases[j].source == c->source) {
                    recv_case = &sel->cases[j];
                    break;
                }
            }
            if (!recv_case) {
                /* No recv case for this channel; we've consumed a value we
                 * can't dispatch. This is a caller error — a channel must
                 * have a recv case if we try_recv from it. */
                return CCRes_err(bool, CCIoError, cc_io_from_errno(EINVAL));
            }
            if (recv_case->out_ptr_slot) *recv_case->out_ptr_slot = out_tmp;
            void* ret = recv_case->h.fn ? recv_case->h.fn(recv_case->h.env) : NULL;
            *out_fired = true;
            return CCRes_ok(bool, CCIoError, ret != NULL);
        }
        if (rc == EPIPE) {
            /* Channel closed — fire close handler if registered; else synthesize
             * a recv-with-null behavior (fall through as Ok(false)). */
            CCSelectCase* close_case = NULL;
            CCIoError io_err = cc_io_error(CC_IO_CONNECTION_CLOSED);
            if (cc__chan_has_close_error((CCChan*)c->source, /*is_recv=*/true)) {
                io_err = cc__chan_get_close_error((CCChan*)c->source, /*is_recv=*/true);
            }
            for (uint8_t j = 0; j < sel->count; ++j) {
                if (sel->cases[j].kind == CC_SEL_CLOSE &&
                    sel->cases[j].source == c->source) {
                    close_case = &sel->cases[j];
                    break;
                }
            }
            if (close_case && close_case->h.fn) {
                if (close_case->out_err_slot) *close_case->out_err_slot = io_err;
                void* ret = close_case->h.fn(close_case->h.env);
                *out_fired = true;
                return CCRes_ok(bool, CCIoError, ret != NULL);
            }
            /* No close case: propagate as Err so the caller observes the close. */
            *out_fired = true;
            return CCRes_err(bool, CCIoError, io_err);
        }
        if (rc != EAGAIN) {
            *out_fired = true;
            return CCRes_err(bool, CCIoError, cc_io_from_errno(rc));
        }
    }
    return CCRes_ok(bool, CCIoError, false);
}

CCResult_bool_CCIoError cc_select_wait(CCSelect* sel) {
    if (!sel || sel->count == 0) {
        return CCRes_err(bool, CCIoError, cc_io_from_errno(EINVAL));
    }
    int wire_rc = cc__select_lazy_wire(sel);
    if (wire_rc != 0) {
        return CCRes_err(bool, CCIoError, cc_io_from_errno(wire_rc));
    }

    /* Find the readable case (v1: at most one). */
    CCSelectCase* readable_case = NULL;
    for (uint8_t i = 0; i < sel->count; ++i) {
        if (sel->cases[i].kind == CC_SEL_READABLE) {
            readable_case = &sel->cases[i];
            break;
        }
    }

    for (;;) {
        bool fired = false;
        CCResult_bool_CCIoError r = cc__select_try_channel_cases(sel, &fired);
        if (fired || !r.ok) return r;

        /* Snapshot the shared signal epoch before the second try. */
        uint64_t epoch = cc_socket_signal_snapshot(sel->_sig);
        r = cc__select_try_channel_cases(sel, &fired);
        if (fired || !r.ok) return r;

        /* Park on the signal. The signal's socket-fd arm catches socket
         * readiness; the signal epoch arm catches channel activity. */
        CCResult_bool_CCIoError wait_res = cc_socket_signal_wait_since(sel->_sig, epoch);
        if (!wait_res.ok) {
            return CCRes_err(bool, CCIoError, wait_res.u.error);
        }
        if (wait_res.u.value && readable_case) {
            /* Socket became readable — but also re-poll channels first: a
             * channel may have landed data between the last try and the wake
             * (the signal arm doesn't tell us which source fired). */
            CCResult_bool_CCIoError r2 = cc__select_try_channel_cases(sel, &fired);
            if (fired || !r2.ok) return r2;
            void* ret = readable_case->h.fn ? readable_case->h.fn(readable_case->h.env) : NULL;
            return CCRes_ok(bool, CCIoError, ret != NULL);
        }
        /* Signal-only wake (no socket readiness) — loop and re-poll channels. */
    }
}
