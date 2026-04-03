#ifndef CC_RUNTIME_IO_WAIT_H
#define CC_RUNTIME_IO_WAIT_H

#include <stddef.h>

typedef struct cc__io_owned_watcher cc__io_owned_watcher;
typedef struct cc__wait_select_group cc__wait_select_group;

typedef struct cc__io_wait_select_handle {
    int kind;
    void* ptr;
} cc__io_wait_select_handle;

int cc__io_wait_ready(int fd, short events);
int cc__io_wait_fd(int fd, short events);
void cc__io_wait_forget_fd(int fd);
cc__io_owned_watcher* cc__io_watcher_create(int fd);
void cc__io_watcher_destroy(cc__io_owned_watcher* watcher);
int cc__io_watcher_wait(cc__io_owned_watcher* watcher, short events);
int cc__io_wait_select_publish(cc__io_owned_watcher* watcher,
                               short events,
                               cc__wait_select_group* group,
                               size_t select_index,
                               cc__io_wait_select_handle* out_handle);
void cc__io_wait_select_finish(cc__io_wait_select_handle* handle);

#endif /* CC_RUNTIME_IO_WAIT_H */
