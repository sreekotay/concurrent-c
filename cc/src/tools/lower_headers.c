/*
 * lower_headers.c - Tool to lower all .cch headers to .h files
 *
 * Usage: lower_headers <input_dir> <output_dir>
 *
 * Recursively finds all .cch files in input_dir, transforms CC syntax
 * (T!>(E) -> CCResult_T_E, T? -> CCOptional_T), and writes to output_dir
 * preserving directory structure.
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

/* Process a single .cch file */
static int process_file(const char* cch_path, const char* h_path) {
    /* Ensure output directory exists */
    char* h_dir = strdup(h_path);
    if (!h_dir) return ENOMEM;
    char* last_slash = strrchr(h_dir, '/');
    if (last_slash) {
        *last_slash = '\0';
        mkpath(h_dir, 0755);
    }
    free(h_dir);
    
    /* Check if output is newer than input (skip if so) */
    struct stat in_stat, out_stat;
    if (stat(cch_path, &in_stat) == 0 && stat(h_path, &out_stat) == 0) {
        if (out_stat.st_mtime >= in_stat.st_mtime) {
            /* Output is up to date */
            return 0;
        }
    }
    
    printf("  %s -> %s\n", cch_path, h_path);
    return cc_lower_header(cch_path, h_path);
}

/* Recursively process directory */
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
        if (entry->d_name[0] == '.') continue;  /* Skip hidden and . / .. */
        
        char entry_path[4096];
        snprintf(entry_path, sizeof(entry_path), "%s/%s", path, entry->d_name);
        
        struct stat st;
        if (stat(entry_path, &st) < 0) continue;
        
        if (S_ISDIR(st.st_mode)) {
            /* Recurse into subdirectory */
            char new_subdir[4096];
            if (subdir && subdir[0]) {
                snprintf(new_subdir, sizeof(new_subdir), "%s/%s", subdir, entry->d_name);
            } else {
                snprintf(new_subdir, sizeof(new_subdir), "%s", entry->d_name);
            }
            err = process_dir(input_dir, output_dir, new_subdir);
            if (err) break;
        } else if (S_ISREG(st.st_mode) && is_cch_file(entry->d_name)) {
            /* Process .cch file */
            char* h_path = cch_to_h_path(input_dir, output_dir, entry_path);
            if (!h_path) {
                err = ENOMEM;
                break;
            }
            err = process_file(entry_path, h_path);
            free(h_path);
            if (err) break;
        }
    }
    
    closedir(dir);
    return err;
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <input_dir> <output_dir>\n", argv[0]);
        fprintf(stderr, "\nLowers .cch headers to .h files:\n");
        fprintf(stderr, "  - Rewrites T!>(E) -> CCResult_T_E + guarded CC_DECL_RESULT_SPEC\n");
        fprintf(stderr, "  - Rewrites T? -> CCOptional_T + guarded CC_DECL_OPTIONAL\n");
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
    
    printf("Done.\n");
    return 0;
}
