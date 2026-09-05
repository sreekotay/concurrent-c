/* Tiny libuv FFI for the Track B Rust twin — keeps Rust off uv_timer_t layout. */
#include <uv.h>
#include <stdlib.h>

typedef void (*tb_rust_timer_cb)(void *data);

typedef struct {
    uv_timer_t handle;
    tb_rust_timer_cb cb;
    void *data;
} TbRustTimer;

static void tb_rust_on_timer(uv_timer_t *h) {
    TbRustTimer *t = (TbRustTimer *)h->data;
    if (t && t->cb) t->cb(t->data);
}

static void tb_rust_on_close(uv_handle_t *h) {
    free(h->data);
}

uv_loop_t *tb_uv_default_loop(void) { return uv_default_loop(); }

TbRustTimer *tb_uv_timer_new(uv_loop_t *loop, tb_rust_timer_cb cb, void *data) {
    TbRustTimer *t = (TbRustTimer *)calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->cb = cb;
    t->data = data;
    if (uv_timer_init(loop, &t->handle) != 0) {
        free(t);
        return NULL;
    }
    t->handle.data = t;
    return t;
}

int tb_uv_timer_start(TbRustTimer *t, uint64_t timeout, uint64_t repeat) {
    if (!t) return -1;
    return uv_timer_start(&t->handle, tb_rust_on_timer, timeout, repeat);
}

int tb_uv_timer_stop(TbRustTimer *t) {
    if (!t) return -1;
    return uv_timer_stop(&t->handle);
}

void tb_uv_timer_close(TbRustTimer *t) {
    if (!t) return;
    if (!uv_is_closing((uv_handle_t *)&t->handle))
        uv_close((uv_handle_t *)&t->handle, tb_rust_on_close);
}

int tb_uv_run_once(uv_loop_t *loop) { return uv_run(loop, UV_RUN_ONCE); }
