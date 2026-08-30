#include <ccc/cc_turnstile.cch>
#include <errno.h>

CCResult_bool_CCIoError cc_turnstile_enter(CCTurnstile* t, int i) {
    int tok = 0;
    int rc;
    (void)i;
    if (!cc_turnstile_ready(t))
        return cc_err_CCResult_bool_CCIoError(cc_io_error(CC_IO_INVALID_ARGUMENT));
    rc = cc_channel_raw_recv(t->depth_ch, &tok, sizeof(tok));
    if (rc != 0) {
        /* Closed depth channel is not Ok(false): that would spawn unbounded. */
        if (rc == EPIPE)
            return cc_err_CCResult_bool_CCIoError(
                cc_io_error(CC_IO_CONNECTION_CLOSED));
        return cc_chan_result_with(t->depth_ch, rc, /*is_recv=*/true);
    }
    return cc_ok_CCResult_bool_CCIoError(true);
}

CCResult_bool_CCIoError cc_turnstile_leave(CCTurnstile* t) {
    int tok = 1;
    int rc;
    if (!t || !t->depth_ch)
        return cc_err_CCResult_bool_CCIoError(cc_io_error(CC_IO_INVALID_ARGUMENT));
    rc = cc_channel_raw_send(t->depth_ch, &tok, sizeof(tok));
    if (rc == EPIPE)
        return cc_err_CCResult_bool_CCIoError(cc_io_error(CC_IO_CONNECTION_CLOSED));
    return cc_chan_result_with(t->depth_ch, rc, /*is_recv=*/false);
}
