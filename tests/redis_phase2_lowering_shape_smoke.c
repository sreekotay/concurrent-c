/*
 * Lowering-shape oracle for the two Redis concurrency models:
 *
 *   redis_owner.ccs     — channel reply path lowers to typed try_send_into;
 *                         owner_loop / handle_client stay fiber-hot (no
 *                         cc_run_blocking_task).
 *   redis_idiomatic.ccs — exclusive shard locks (cc_exclusive_acquire*);
 *                         no channel try_send_into; handle_client fiber-hot.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char* read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    long n;
    char* buf;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    n = ftell(f);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = (char*)malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[n] = 0;
    fclose(f);
    return buf;
}

static int span_has(const char* start, const char* end_marker, const char* needle) {
    const char* end = strstr(start, end_marker);
    const char* hit;
    if (!start || !end || end < start) return 0;
    hit = strstr(start, needle);
    return hit && hit < end;
}

static char* emit_c(const char* src, const char* tag) {
    char out_path[256];
    char log_path[256];
    char cmd[1024];
    char* lowered;

    snprintf(out_path, sizeof(out_path), "tmp/redis_phase2_%s_%ld.c", tag, (long)getpid());
    snprintf(log_path, sizeof(log_path), "tmp/redis_phase2_%s_%ld.log", tag, (long)getpid());
    snprintf(cmd, sizeof(cmd),
             "./cc/bin/ccc --no-cache --emit-c-only %s -o %s > %s 2>&1",
             src, out_path, log_path);
    if (system(cmd) != 0) {
        fprintf(stderr, "emit-c-only failed for %s; see %s\n", src, log_path);
        return NULL;
    }
    lowered = read_file(out_path);
    remove(out_path);
    remove(log_path);
    if (!lowered) {
        fprintf(stderr, "read lowered C failed for %s\n", src);
        return NULL;
    }
    return lowered;
}

static int check_owner(const char* lowered) {
    const char* handle_poll;
    const char* owner_hot;

    if (!strstr(lowered, "cc_channel_try_send_into(")) {
        fprintf(stderr, "owner: reply path did not lower to typed try_send_into\n");
        return 1;
    }
    if (strstr(lowered, "cc_channel_raw_try_send_into(")) {
        fprintf(stderr, "owner: reply path still contains raw try_send_into plumbing\n");
        return 1;
    }
    if (strstr(lowered, "cc_channel_raw_send_into(")) {
        fprintf(stderr, "owner: reply path still contains blocking send_into\n");
        return 1;
    }

    handle_poll = strstr(lowered, "static CCFutureStatus __cc_async_handle_client_");
    if (!handle_poll ||
        span_has(handle_poll, "static void __cc_async_handle_client_",
                 "cc_run_blocking_task")) {
        fprintf(stderr, "owner: handle_client hot coroutine contains blocking-thread dispatch\n");
        return 1;
    }

    owner_hot = strstr(lowered, "cc_io_avail(cc_channel_recv(__f->__p_req_rx");
    if (!owner_hot ||
        span_has(owner_hot, "static void __cc_async_owner_loop_",
                 "cc_run_blocking_task")) {
        fprintf(stderr, "owner: owner_loop request/reply path contains blocking-thread dispatch\n");
        return 1;
    }
    return 0;
}

static int check_idiomatic(const char* lowered) {
    const char* handle_poll;

    if (!strstr(lowered, "cc_exclusive_acquire(") &&
        !strstr(lowered, "cc_exclusive_acquire_sorted(") &&
        !strstr(lowered, "cc_exclusive_acquire_range(")) {
        fprintf(stderr, "idiomatic: shard path did not lower to cc_exclusive_acquire*\n");
        return 1;
    }
    if (strstr(lowered, "cc_channel_try_send_into(")) {
        fprintf(stderr, "idiomatic: unexpected channel try_send_into (exclusive path)\n");
        return 1;
    }
    if (strstr(lowered, "cc_channel_raw_try_send_into(") ||
        strstr(lowered, "cc_channel_raw_send_into(")) {
        fprintf(stderr, "idiomatic: unexpected raw channel send_into plumbing\n");
        return 1;
    }

    handle_poll = strstr(lowered, "static CCFutureStatus __cc_async_handle_client_");
    if (!handle_poll ||
        span_has(handle_poll, "static void __cc_async_handle_client_",
                 "cc_run_blocking_task")) {
        fprintf(stderr, "idiomatic: handle_client hot coroutine contains blocking-thread dispatch\n");
        return 1;
    }
    return 0;
}

int main(void) {
    char* owner = NULL;
    char* idio = NULL;
    int rc = 0;

    if (mkdir("tmp", 0777) != 0 && errno != EEXIST) {
        perror("mkdir tmp");
        return 2;
    }

    owner = emit_c("real_projects/redis/redis_owner.ccs", "owner");
    if (!owner) return 2;
    if (check_owner(owner) != 0) {
        free(owner);
        return 1;
    }
    free(owner);

    idio = emit_c("real_projects/redis/redis_idiomatic.ccs", "idiomatic");
    if (!idio) return 2;
    if (check_idiomatic(idio) != 0) {
        free(idio);
        return 1;
    }
    free(idio);

    puts("ok");
    return rc;
}
