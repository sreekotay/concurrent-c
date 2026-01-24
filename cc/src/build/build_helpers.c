#include <ccc/cc_build_helpers.cch>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void cc_build_trim_space(char* s) {
    if (!s) return;
    char* end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    char* start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
}

int cc_build_join_paths(char* out, size_t cap, const char* base, const char* rel) {
    if (!out || cap == 0 || !base || !rel) return -1;
    size_t len = 0;
    len = snprintf(out, cap, "%s/%s", base, rel);
    return (len >= cap) ? -1 : 0;
}

int cc_build_make_stem(char* out, size_t cap, const char* rel_path) {
    if (!out || cap == 0 || !rel_path) return -1;
    const char* slash = strrchr(rel_path, '/');
    const char* dot = strrchr(rel_path, '.');
    size_t copy_len = strlen(rel_path);
    if (dot && (!slash || dot > slash)) copy_len = (size_t)(dot - rel_path);
    size_t written = 0;
    for (size_t i = 0; i < copy_len && written + 1 < cap; ++i) {
        char c = rel_path[i];
        if (c == '/' || c == '\\') {
            c = '_';
        } else if (c == '.' && (!slash || (const char*)&rel_path[i] > slash)) {
            c = '_';
        }
        out[written++] = c;
    }
    if (written == 0 && cap > 1) {
        out[written++] = '_';
    }
    out[written] = '\0';
    return 0;
}

int cc_build_read_kv_pair(const char* path, const char* key, char* out, size_t cap) {
    if (!path || !key || !out || cap == 0) return -1;
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    int result = -1;
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == '\0') continue;
        char* eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char* value = eq + 1;
        cc_build_trim_space(p);
        cc_build_trim_space(value);
        if (strcmp(p, key) == 0) {
            strncpy(out, value, cap);
            out[cap - 1] = '\0';
            result = 0;
            break;
        }
    }
    fclose(f);
    return result;
}

int cc_build_expand_flag(char* out, size_t cap, const char* base, const char* extra) {
    if (!out || cap == 0) return -1;
    out[0] = '\0';
    if (base && *base) {
        strncpy(out, base, cap);
        out[cap - 1] = '\0';
    }
    if (extra && *extra) {
        char tmp[256];
        strncpy(tmp, extra, sizeof(tmp));
        tmp[sizeof(tmp) - 1] = '\0';
        cc_build_trim_space(tmp);
        if (*tmp) {
            if (out[0]) {
                size_t len = strlen(out);
                if (len + 1 < cap) {
                    out[len] = ' ';
                    out[len + 1] = '\0';
                }
            }
            strncat(out, tmp, cap - strlen(out) - 1);
        }
    }
    return 0;
}
