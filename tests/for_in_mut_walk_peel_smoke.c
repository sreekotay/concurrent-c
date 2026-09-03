/*
 * Mut-walk peel: frozen slice / clean grower snapshot .len and the data
 * pointer; the store is not behind a goto. A grower the body shrinks
 * keeps "for-in write".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static int count_substr(const char* s, const char* needle) {
    int count = 0;
    size_t n = strlen(needle);
    const char* p = s;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += n;
    }
    return count;
}

static char* emit_c(const char* src, const char* tag) {
    char out_path[256];
    char log_path[256];
    char cmd[1024];
    char* lowered;

    snprintf(out_path, sizeof(out_path), "tmp/for_in_mut_walk_peel_%s_%ld.c",
             tag, (long)getpid());
    snprintf(log_path, sizeof(log_path), "tmp/for_in_mut_walk_peel_%s_%ld.log",
             tag, (long)getpid());
    snprintf(cmd, sizeof(cmd),
             "./cc/bin/ccc --emit-c-only %s -o %s > %s 2>&1", src, out_path,
             log_path);
    if (system(cmd) != 0) {
        fprintf(stderr, "emit-c-only failed for %s; see %s\n", src, log_path);
        return NULL;
    }
    lowered = read_file(out_path);
    remove(out_path);
    remove(log_path);
    if (!lowered)
        fprintf(stderr, "read lowered C failed for %s\n", src);
    return lowered;
}

static int check_peel(const char* lowered, const char* tag, int want_write) {
    int writes;
    if (!strstr(lowered, "__cc_fn_") || !strstr(lowered, "__cc_fp_")) {
        fprintf(stderr, "%s: missing snapshot peel (__cc_fn_ / __cc_fp_)\n",
                tag);
        return 0;
    }
    if (!strstr(lowered, " = ((__cc_fp_")) {
        fprintf(stderr, "%s: missing peel store\n", tag);
        return 0;
    }
    writes = count_substr(lowered,
                          "CC_ERROR(CC_ERR_INVALID_ARG, \"for-in write\")");
    if (writes != want_write) {
        fprintf(stderr, "%s: expected %d write-bound raises, got %d\n", tag,
                want_write, writes);
        return 0;
    }
    return 1;
}

int main(void) {
    char* slice = emit_c("tests/for_in_mut_slice_smoke.ccs", "slice");
    char* extent = emit_c("tests/for_in_mut_extent_smoke.ccs", "extent");
    char* shrink = emit_c("tests/for_in_mut_vec_shrink_smoke.ccs", "shrink");
    int ok = 1;

    if (!slice || !extent || !shrink) {
        free(slice);
        free(extent);
        free(shrink);
        return 2;
    }
    if (!check_peel(slice, "slice", 0)) ok = 0;
    if (!check_peel(extent, "extent", 0)) ok = 0;
    if (count_substr(shrink,
                     "CC_ERROR(CC_ERR_INVALID_ARG, \"for-in write\")") !=
        1) {
        fprintf(stderr, "shrink: expected 1 write-bound raise, got %d\n",
                count_substr(shrink,
                             "CC_ERROR(CC_ERR_INVALID_ARG, \"for-in write\")"));
        ok = 0;
    }
    if (!strstr(shrink, "__cc_wv")) {
        fprintf(stderr, "shrink: lost the live write temp\n");
        ok = 0;
    }
    if (strstr(shrink, " = ((__cc_fp_")) {
        fprintf(stderr, "shrink: shrinking grower still used a peel store\n");
        ok = 0;
    }

    free(slice);
    free(extent);
    free(shrink);
    if (!ok) return 1;
    puts("ok");
    return 0;
}
