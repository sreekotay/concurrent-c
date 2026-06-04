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

int main(void) {
    char out_path[256];
    char log_path[256];
    char cmd[1024];
    char* lowered;
    const char* handle_poll;
    const char* owner_hot;

    if (mkdir("tmp", 0777) != 0 && errno != EEXIST) {
        perror("mkdir tmp");
        return 2;
    }

    snprintf(out_path, sizeof(out_path), "tmp/redis_phase2_lowering_%ld.c", (long)getpid());
    snprintf(log_path, sizeof(log_path), "tmp/redis_phase2_lowering_%ld.log", (long)getpid());
    snprintf(cmd, sizeof(cmd),
             "./cc/bin/ccc --emit-c-only real_projects/redis/redis_idiomatic.ccs -o %s > %s 2>&1",
             out_path, log_path);
    if (system(cmd) != 0) {
        fprintf(stderr, "emit-c-only failed; see %s\n", log_path);
        return 2;
    }

    lowered = read_file(out_path);
    if (!lowered) {
        perror("read lowered C");
        return 2;
    }

    if (!strstr(lowered, "cc_channel_raw_try_send_into(")) {
        fprintf(stderr, "Redis reply path did not lower to try_send_into\n");
        free(lowered);
        return 1;
    }
    if (strstr(lowered, "cc_channel_raw_send_into(")) {
        fprintf(stderr, "Redis reply path still contains blocking send_into\n");
        free(lowered);
        return 1;
    }

    handle_poll = strstr(lowered, "static CCFutureStatus __cc_async_handle_client_");
    if (!handle_poll || span_has(handle_poll, "static void __cc_async_handle_client_", "cc_run_blocking_task")) {
        fprintf(stderr, "handle_client hot coroutine contains blocking-thread dispatch\n");
        free(lowered);
        return 1;
    }

    owner_hot = strstr(lowered, "cc_io_avail(cc_channel_recv(__f->__p_req_rx");
    if (!owner_hot || span_has(owner_hot, "redis_conn_release_request", "cc_run_blocking_task")) {
        fprintf(stderr, "owner request/reply loop contains blocking-thread dispatch\n");
        free(lowered);
        return 1;
    }

    free(lowered);
    remove(out_path);
    remove(log_path);
    puts("ok");
    return 0;
}
