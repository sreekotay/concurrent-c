/*
 * lower_headers.c - Tool to lower all .cch headers to .h files
 *
 * Usage:
 *   lower_headers <input_dir> <output_dir>
 *   lower_headers <input_dir> <output_dir> --rewrite-includes <src_dir> <dst_dir>
 *   lower_headers --ordered <output_dir> <file.cch> [file.cch ...]
 *
 * Mode 1 (headers): Recursively finds all .cch files in input_dir, transforms
 * CC syntax (T!>(E) -> CCResult_T_E), and writes .h files to output_dir
 * preserving directory structure. Face-tree *.h that are already host C
 * (ccc/vendor/ffc.h, cc_closure_helper.h, …) are copied through as real
 * files (symlinks followed) so out/include is a complete one-I host-C root.
 *
 * Mode 2 (--rewrite-includes): Additionally copies .c and .h files from
 * src_dir to dst_dir, rewriting any #include referencing .cch to .h so that
 * plain C compilation units resolve to the lowered headers.
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "header/lower_header.h"
#include "util/io.h"

/* Create directory and all parents */
static int mkpath(const char* path, mode_t mode) {
    char* p = strdup(path);
    if (!p) return -1;
    
    char* sep = p;
    while ((sep = strchr(sep + 1, '/'))) {
        *sep = '\0';
        if (mkdir(p, mode) < 0 && errno != EEXIST) {
            free(p);
            return -1;
        }
        *sep = '/';
    }
    if (mkdir(p, mode) < 0 && errno != EEXIST) {
        free(p);
        return -1;
    }
    free(p);
    return 0;
}

/* Check if path ends with .cch */
static int is_cch_file(const char* name) {
    size_t len = strlen(name);
    return len > 4 && strcmp(name + len - 4, ".cch") == 0;
}

static int is_h_file(const char* name) {
    size_t len = strlen(name);
    return len > 2 && strcmp(name + len - 2, ".h") == 0;
}

static int is_c_or_h_file(const char* name) {
    size_t len = strlen(name);
    if (len > 2 && strcmp(name + len - 2, ".c") == 0) return 1;
    return is_h_file(name);
}

static const char* path_basename(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* Flat .h next to output_dir for an explicit --ordered input list. */
static char* ordered_h_path(const char* output_dir, const char* cch_path) {
    const char* base = path_basename(cch_path);
    size_t dlen = strlen(output_dir);
    size_t blen = strlen(base);
    char* out = malloc(dlen + 1 + blen + 1);
    size_t n;
    if (!out) return NULL;
    sprintf(out, "%s/%s", output_dir, base);
    n = strlen(out);
    if (n > 4 && strcmp(out + n - 4, ".cch") == 0)
        strcpy(out + n - 4, ".h");
    return out;
}

/* Convert .cch path to .h path */
static char* cch_to_h_path(const char* input_dir, const char* output_dir, const char* cch_path) {
    /* Get relative path from input_dir */
    const char* rel = cch_path;
    size_t in_len = strlen(input_dir);
    if (strncmp(cch_path, input_dir, in_len) == 0) {
        rel = cch_path + in_len;
        if (*rel == '/') rel++;
    }
    
    /* Build output path with .h extension */
    size_t rel_len = strlen(rel);
    size_t out_dir_len = strlen(output_dir);
    char* out = malloc(out_dir_len + 1 + rel_len + 1);
    if (!out) return NULL;
    
    sprintf(out, "%s/%s", output_dir, rel);
    
    /* Change .cch to .h */
    size_t out_len = strlen(out);
    if (out_len > 4) {
        strcpy(out + out_len - 4, ".h");
    }
    
    return out;
}

/* Same relative path under output_dir (face-tree *.h passthrough). */
static char* rel_out_path(const char* input_dir, const char* output_dir,
                          const char* src_path) {
    const char* rel = src_path;
    size_t in_len = strlen(input_dir);
    if (strncmp(src_path, input_dir, in_len) == 0) {
        rel = src_path + in_len;
        if (*rel == '/') rel++;
    }
    size_t rel_len = strlen(rel);
    size_t out_dir_len = strlen(output_dir);
    char* out = malloc(out_dir_len + 1 + rel_len + 1);
    if (!out) return NULL;
    sprintf(out, "%s/%s", output_dir, rel);
    return out;
}

static int copy_passthrough_h(const char* src_path, const char* dst_path) {
    FILE* in;
    char* h_dir = strdup(dst_path);
    if (!h_dir) return ENOMEM;
    char* last_slash = strrchr(h_dir, '/');
    if (last_slash) {
        *last_slash = '\0';
        mkpath(h_dir, 0755);
    }
    free(h_dir);

    in = fopen(src_path, "rb");
    if (!in) return errno ? errno : -1;
    if (fseek(in, 0, SEEK_END) != 0) {
        int e = errno;
        fclose(in);
        return e ? e : -1;
    }
    long len = ftell(in);
    if (len < 0) {
        int e = errno;
        fclose(in);
        return e ? e : -1;
    }
    if (fseek(in, 0, SEEK_SET) != 0) {
        int e = errno;
        fclose(in);
        return e ? e : -1;
    }
    char* buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(in);
        return ENOMEM;
    }
    size_t n = fread(buf, 1, (size_t)len, in);
    fclose(in);
    int err = cc_write_file_if_changed(dst_path, buf, n);
    free(buf);
    if (err) return err;
    printf("  passthrough: %s -> %s\n", src_path, dst_path);
    return 0;
}

/* Process a single .cch file: full header lowering */
static int process_file(const char* cch_path, const char* h_path) {
    char* h_dir = strdup(h_path);
    if (!h_dir) return ENOMEM;
    char* last_slash = strrchr(h_dir, '/');
    if (last_slash) {
        *last_slash = '\0';
        mkpath(h_dir, 0755);
    }
    free(h_dir);
    
    printf("  %s -> %s\n", cch_path, h_path);
    return cc_lower_header(cch_path, h_path);
}

/* Recursively process directory for .cch header lowering */
static int process_dir(const char* input_dir, const char* output_dir, const char* subdir) {
    char path[4096];
    if (subdir && subdir[0]) {
        snprintf(path, sizeof(path), "%s/%s", input_dir, subdir);
    } else {
        snprintf(path, sizeof(path), "%s", input_dir);
    }
    
    DIR* dir = opendir(path);
    if (!dir) {
        fprintf(stderr, "Cannot open directory: %s\n", path);
        return errno;
    }
    
    int err = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        
        char entry_path[4096];
        snprintf(entry_path, sizeof(entry_path), "%s/%s", path, entry->d_name);
        
        struct stat st;
        if (stat(entry_path, &st) < 0) continue;
        
        if (S_ISDIR(st.st_mode)) {
            char new_subdir[4096];
            if (subdir && subdir[0]) {
                snprintf(new_subdir, sizeof(new_subdir), "%s/%s", subdir, entry->d_name);
            } else {
                snprintf(new_subdir, sizeof(new_subdir), "%s", entry->d_name);
            }
            err = process_dir(input_dir, output_dir, new_subdir);
            if (err) break;
        } else if (S_ISREG(st.st_mode) && is_cch_file(entry->d_name)) {
            char* h_path = cch_to_h_path(input_dir, output_dir, entry_path);
            if (!h_path) {
                err = ENOMEM;
                break;
            }
            err = process_file(entry_path, h_path);
            free(h_path);
            if (err) break;
        } else if (S_ISREG(st.st_mode) && is_h_file(entry->d_name)) {
            char* h_path = rel_out_path(input_dir, output_dir, entry_path);
            if (!h_path) {
                err = ENOMEM;
                break;
            }
            err = copy_passthrough_h(entry_path, h_path);
            free(h_path);
            if (err) break;
        }
    }
    
    closedir(dir);
    return err;
}

/* ---------------------------------------------------------------------------
 * --rewrite-includes: copy .c/.h source files, rewriting .cch includes to .h
 * -------------------------------------------------------------------------*/

static char* rewrite_cch_includes(const char* src, size_t n, int* changed) {
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t last_emit = 0;
    size_t i = 0;
    *changed = 0;

    while (i < n) {
        if (i + 5 <= n && strncmp(src + i, ".cch>", 5) == 0) {
            out_cap = out_cap ? out_cap : n + 64;
            if (!out) { out = malloc(out_cap); if (!out) return NULL; }
            size_t chunk = i - last_emit;
            if (out_len + chunk + 4 > out_cap) {
                out_cap = (out_len + chunk + 4) * 2;
                out = realloc(out, out_cap);
                if (!out) return NULL;
            }
            memcpy(out + out_len, src + last_emit, chunk); out_len += chunk;
            memcpy(out + out_len, ".h>", 3); out_len += 3;
            i += 5; last_emit = i;
            *changed = 1;
            continue;
        }
        if (i + 5 <= n && strncmp(src + i, ".cch\"", 5) == 0) {
            out_cap = out_cap ? out_cap : n + 64;
            if (!out) { out = malloc(out_cap); if (!out) return NULL; }
            size_t chunk = i - last_emit;
            if (out_len + chunk + 4 > out_cap) {
                out_cap = (out_len + chunk + 4) * 2;
                out = realloc(out, out_cap);
                if (!out) return NULL;
            }
            memcpy(out + out_len, src + last_emit, chunk); out_len += chunk;
            memcpy(out + out_len, ".h\"", 3); out_len += 3;
            i += 5; last_emit = i;
            *changed = 1;
            continue;
        }
        i++;
    }

    if (!*changed) return NULL;
    if (last_emit < n) {
        size_t tail = n - last_emit;
        if (out_len + tail + 1 > out_cap) {
            out_cap = out_len + tail + 1;
            out = realloc(out, out_cap);
            if (!out) return NULL;
        }
        memcpy(out + out_len, src + last_emit, tail); out_len += tail;
    }
    out[out_len] = '\0';
    return out;
}

static int rewrite_source_file(const char* src_path, const char* dst_path) {
    FILE* in = fopen(src_path, "rb");
    if (!in) return errno ? errno : -1;
    fseek(in, 0, SEEK_END);
    long len = ftell(in);
    fseek(in, 0, SEEK_SET);
    if (len <= 0) { fclose(in); return 0; }

    char* buf = malloc(len + 1);
    if (!buf) { fclose(in); return ENOMEM; }
    size_t nread = fread(buf, 1, len, in);
    fclose(in);
    buf[nread] = '\0';

    int changed = 0;
    char* rewritten = rewrite_cch_includes(buf, nread, &changed);
    const char* to_write = rewritten ? rewritten : buf;
    size_t to_write_len = rewritten ? strlen(rewritten) : nread;

    int werr = cc_write_file_if_changed(dst_path, to_write, to_write_len);
    if (werr) { free(buf); free(rewritten); return werr; }

    if (changed) printf("  rewrite: %s -> %s\n", src_path, dst_path);

    free(buf);
    free(rewritten);
    return 0;
}

static int rewrite_source_dir(const char* src_dir, const char* dst_dir) {
    DIR* dir = opendir(src_dir);
    if (!dir) {
        fprintf(stderr, "Cannot open source directory: %s\n", src_dir);
        return errno;
    }

    mkpath(dst_dir, 0755);

    int err = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char src_path[4096], dst_path[4096];
        snprintf(src_path, sizeof(src_path), "%s/%s", src_dir, entry->d_name);

        struct stat st;
        if (stat(src_path, &st) < 0 || !S_ISREG(st.st_mode)) continue;
        if (!is_c_or_h_file(entry->d_name)) continue;

        snprintf(dst_path, sizeof(dst_path), "%s/%s", dst_dir, entry->d_name);
        err = rewrite_source_file(src_path, dst_path);
        if (err) break;
    }

    closedir(dir);
    return err;
}

/* ---------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------*/

int main(int argc, char* argv[]) {
    if (argc >= 4 && strcmp(argv[1], "--ordered") == 0) {
        const char* output_dir = argv[2];
        int i;
        if (mkpath(output_dir, 0755) != 0) {
            fprintf(stderr, "Cannot create output directory: %s\n", output_dir);
            return 1;
        }
        printf("Lowering headers (ordered): -> %s\n", output_dir);
        for (i = 3; i < argc; i++) {
            char* h_path = ordered_h_path(output_dir, argv[i]);
            int err;
            if (!h_path) {
                fprintf(stderr, "OOM\n");
                return 1;
            }
            if (!is_cch_file(argv[i])) {
                fprintf(stderr, "Not a .cch file: %s\n", argv[i]);
                free(h_path);
                return 1;
            }
            err = process_file(argv[i], h_path);
            free(h_path);
            if (err) {
                fprintf(stderr, "Error lowering %s: %d\n", argv[i], err);
                return 1;
            }
        }
        printf("Done.\n");
        return 0;
    }

    if (argc != 3 && argc != 6) {
        fprintf(stderr, "Usage: %s <input_dir> <output_dir>\n", argv[0]);
        fprintf(stderr, "       %s <input_dir> <output_dir> --rewrite-includes <src_dir> <dst_dir>\n", argv[0]);
        fprintf(stderr, "       %s --ordered <output_dir> <file.cch> [file.cch ...]\n", argv[0]);
        fprintf(stderr, "\nLowers .cch headers to .h files:\n");
        fprintf(stderr, "  - Rewrites T!>(E) -> CCResult_T_E + guarded CC_DECL_RESULT_SPEC\n");
        fprintf(stderr, "  - Copies face-tree *.h through (follows symlinks)\n");
        fprintf(stderr, "\n--rewrite-includes: copy .c/.h files, rewriting .cch includes to .h\n");
        fprintf(stderr, "--ordered: lower the listed files in this process, in argv order\n");
        return 1;
    }
    
    const char* input_dir = argv[1];
    const char* output_dir = argv[2];
    
    printf("Lowering headers: %s -> %s\n", input_dir, output_dir);
    
    int err = process_dir(input_dir, output_dir, "");
    if (err) {
        fprintf(stderr, "Error lowering headers: %d\n", err);
        return 1;
    }

    if (argc == 6 && strcmp(argv[3], "--rewrite-includes") == 0) {
        const char* src_dir = argv[4];
        const char* dst_dir = argv[5];
        printf("Rewriting includes: %s -> %s\n", src_dir, dst_dir);
        err = rewrite_source_dir(src_dir, dst_dir);
        if (err) {
            fprintf(stderr, "Error rewriting source includes: %d\n", err);
            return 1;
        }
    }
    
    printf("Done.\n");
    return 0;
}
