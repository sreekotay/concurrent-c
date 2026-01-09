#include "build.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Very small existence check for build.cc.
static bool file_exists(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    fclose(f);
    return true;
}

static int append_binding(CCConstBinding* out_bindings, size_t* out_count, size_t max, const char* name, long long value) {
    if (*out_count >= max) return ENOSPC;
    out_bindings[*out_count].name = name;
    out_bindings[*out_count].value = value;
    (*out_count)++;
    return 0;
}

static int append_target_consts(const CCBuildTarget* target, CCConstBinding* out_bindings, size_t* out_count, size_t max) {
    if (!target) return 0;
    int err = 0;
    err = append_binding(out_bindings, out_count, max, "TARGET_PTR_WIDTH", target->ptr_width);
    if (err) return err;
    err = append_binding(out_bindings, out_count, max, "TARGET_IS_LITTLE_ENDIAN", (target->endian && strcmp(target->endian, "little") == 0) ? 1 : 0);
    if (err) return err;
    // Note: os/arch/abi strings would need string storage; omit for now.
    return 0;
}

// Extremely small parser: lines starting with "CC_CONST <NAME> <VALUE>"
// (whitespace-separated) are captured. VALUE parsed as long long (base auto).
static int parse_int(const char* s, long long* out) {
    if (!s || !out) return EINVAL;
    char* end = NULL;
    long long v = strtoll(s, &end, 0);
    if (end == s) return EINVAL;
    *out = v;
    return 0;
}

// Supports simple expressions: integer literal or known target const names.
static int eval_expr(const char* token, const CCBuildInputs* inputs, long long* out) {
    if (!token || !out) return EINVAL;
    if (inputs && inputs->target) {
        const CCBuildTarget* t = inputs->target;
        if (strcmp(token, "TARGET_PTR_WIDTH") == 0) {
            *out = t->ptr_width;
            return 0;
        }
        if (strcmp(token, "TARGET_IS_LITTLE_ENDIAN") == 0) {
            *out = (t->endian && strcmp(t->endian, "little") == 0) ? 1 : 0;
            return 0;
        }
    }
    return parse_int(token, out);
}

// Extremely small parser: lines starting with "CC_CONST <NAME> <EXPR>"
// EXPR can be an integer literal or a single target const symbol.
static int parse_build_file(const char* path, const CCBuildInputs* inputs, CCConstBinding* out_bindings, size_t* out_count, size_t max) {
    FILE* f = fopen(path, "r");
    if (!f) return errno ? errno : -1;
    char line[512];
    int err = 0;
    size_t lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        // Trim leading spaces
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "CC_CONST", 8) != 0) continue;
        p += 8;
        while (*p == ' ' || *p == '\t') p++;
        char name_buf[128];
        char expr_buf[256];
        if (sscanf(p, "%127s %255s", name_buf, expr_buf) == 2) {
            long long value = 0;
            err = eval_expr(expr_buf, inputs, &value);
            if (err != 0) {
                fprintf(stderr, "%s:%zu: invalid const expression: %s\n", path, lineno, expr_buf);
                break;
            }
            char* stored = strdup(name_buf);
            if (!stored) { err = ENOMEM; break; }
            err = append_binding(out_bindings, out_count, max, stored, value);
            if (err) { free(stored); break; }
        } else {
            fprintf(stderr, "%s:%zu: malformed CC_CONST line\n", path, lineno);
            err = EINVAL;
            break;
        }
    }
    fclose(f);
    return err;
}

static int parse_build_options(const char* path, CCBuildOptionDecl* out_opts, size_t* out_count, size_t max) {
    if (!out_opts || !out_count) return EINVAL;
    FILE* f = fopen(path, "r");
    if (!f) return errno ? errno : -1;
    char line[1024];
    int err = 0;
    size_t lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "CC_OPTION", 9) != 0) continue;
        p += 9;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) continue;

        // name is first token; help is remainder of line.
        char name_buf[128];
        int nread = 0;
        if (sscanf(p, "%127s%n", name_buf, &nread) != 1) {
            fprintf(stderr, "%s:%zu: malformed CC_OPTION line\n", path, lineno);
            err = EINVAL;
            break;
        }
        p += nread;
        while (*p == ' ' || *p == '\t') p++;

        // Trim trailing newline/CR.
        char* end = p + strlen(p);
        while (end > p && (end[-1] == '\n' || end[-1] == '\r')) end--;
        *end = '\0';

        if (*out_count >= max) {
            err = ENOSPC;
            break;
        }
        char* stored_name = strdup(name_buf);
        if (!stored_name) { err = ENOMEM; break; }
        char* stored_help = strdup(*p ? p : "");
        if (!stored_help) { free(stored_name); err = ENOMEM; break; }
        out_opts[*out_count].name = stored_name;
        out_opts[*out_count].help = stored_help;
        (*out_count)++;
    }
    fclose(f);
    if (err == ENOSPC) {
        fprintf(stderr, "cc: too many CC_OPTION lines in build.cc (max %zu)\n", max);
    }
    return err;
}

int cc_build_load_consts(const char* build_path, const CCBuildInputs* inputs, CCConstBinding* out_bindings, size_t* out_count) {
    if (!out_bindings || !out_count) {
        return EINVAL;
    }
    *out_count = 0;
    if (!build_path) {
        return 0;
    }
    if (!file_exists(build_path)) {
        return 0; // No build.cc → no consts.
    }

    const size_t max = 128; // matches caller limit.

    const CCBuildTarget* target = inputs ? inputs->target : NULL;
    int err = append_target_consts(target, out_bindings, out_count, max);
    if (err) return err;

    err = parse_build_file(build_path, inputs, out_bindings, out_count, max);
    if (err == ENOSPC) {
        fprintf(stderr, "cc: too many consts in build.cc (max %zu)\n", max);
        return err;
    } else if (err != 0) {
        return err;
    }

    return 0;
}

int cc_build_list_options(const char* build_path, CCBuildOptionDecl* out_opts, size_t* out_count, size_t max) {
    if (!out_opts || !out_count) return EINVAL;
    *out_count = 0;
    if (!build_path) return 0;
    if (!file_exists(build_path)) return 0;
    return parse_build_options(build_path, out_opts, out_count, max);
}

void cc_build_free_options(CCBuildOptionDecl* opts, size_t count) {
    if (!opts) return;
    for (size_t i = 0; i < count; ++i) {
        free((void*)opts[i].name);
        free((void*)opts[i].help);
        opts[i].name = NULL;
        opts[i].help = NULL;
    }
}

