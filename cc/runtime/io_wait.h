#ifndef CC_RUNTIME_IO_WAIT_H
#define CC_RUNTIME_IO_WAIT_H

typedef struct cc__io_owned_watcher cc__io_owned_watcher;

int cc__io_wait_ready(int fd, short events);
int cc__io_wait_fd(int fd, short events);
void cc__io_wait_forget_fd(int fd);
cc__io_owned_watcher* cc__io_watcher_create(int fd);
void cc__io_watcher_destroy(cc__io_owned_watcher* watcher);
int cc__io_watcher_wait(cc__io_owned_watcher* watcher, short events);

#endif /* CC_RUNTIME_IO_WAIT_H */
