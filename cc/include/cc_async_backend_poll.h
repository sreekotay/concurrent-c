#ifndef CC_ASYNC_BACKEND_POLL_H
#define CC_ASYNC_BACKEND_POLL_H

// Register the poll-based async backend (nonblocking FDs + poll/ppoll).
// Returns 0 on success. Backend uses the shared async runtime executor fallback for none.
int cc_async_backend_poll_register(void);

#endif // CC_ASYNC_BACKEND_POLL_H
