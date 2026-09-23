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
    char* line = NULL;
    size_t line_cap = 0;
    int err = 0;
    size_t lineno = 0;
    while (getline(&line, &line_cap, f) >= 0) {
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
    free(line);
    fclose(f);
    return err;
}

static int parse_build_options(const char* path, CCBuildOptionDecl* out_opts, size_t* out_count, size_t max) {
    if (!out_opts || !out_count) return EINVAL;
    FILE* f = fopen(path, "r");
    if (!f) return errno ? errno : -1;
    char* line = NULL;
    size_t line_cap = 0;
    int err = 0;
    size_t lineno = 0;
    while (getline(&line, &line_cap, f) >= 0) {
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
    free(line);
    fclose(f);
    if (err == ENOSPC) {
        fprintf(stderr, "%s: too many CC_OPTION lines (the limit is %zu)\n", path, max);
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
        fprintf(stderr, "%s: too many CC_CONST lines (the limit is %zu, target consts included)\n", build_path, max);
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

static CCBuildTargetKind parse_target_kind(const char* s) {
    if (!s) return 0;
    if (strcmp(s, "exe") == 0) return CC_BUILD_TARGET_EXE;
    if (strcmp(s, "obj") == 0) return CC_BUILD_TARGET_OBJ;
    return 0;
}

static int find_target_index(CCBuildTargetDecl* targets, size_t count, const char* name) {
    if (!targets || !name) return -1;
    for (size_t i = 0; i < count; ++i) {
        if (targets[i].name && strcmp(targets[i].name, name) == 0) return (int)i;
    }
    return -1;
}

static char* dup_trim_eol(const char* s) {
    if (!s) return strdup("");
    const char* end = s + strlen(s);
    while (end > s && (end[-1] == '\n' || end[-1] == '\r')) end--;
    size_t n = (size_t)(end - s);
    char* out = (char*)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

/* Append to a heap string list, doubling its capacity. The capacity is not
 * stored: it is the next power of two at or above the count. */
static int append_str_list(const char*** io_list, size_t* io_count, const char* s) {
    if (!io_list || !io_count || !s) return EINVAL;
    size_t n = *io_count;
    if (n == 0 || (n & (n - 1)) == 0) {
        size_t cap = n ? n * 2 : 4;
        const char** new_list = (const char**)realloc((void*)(*io_list), cap * sizeof(char*));
        if (!new_list) return ENOMEM;
        *io_list = new_list;
    }
    (*io_list)[n] = s;
    (*io_count)++;
    return 0;
}

static int append_flags_str(const char** io_dst, const char* more) {
    if (!io_dst || !more) return EINVAL;
    // Trim leading ws.
    while (*more == ' ' || *more == '\t') more++;
    if (!*more) return 0;

    if (!*io_dst) {
        char* d = dup_trim_eol(more);
        if (!d) return ENOMEM;
        // Trim trailing ws.
        size_t n = strlen(d);
        while (n > 0 && (d[n - 1] == ' ' || d[n - 1] == '\t')) { d[n - 1] = '\0'; n--; }
        *io_dst = d;
        return 0;
    }
    // concat: "<old> <more>"
    const char* old = *io_dst;
    char* more2 = dup_trim_eol(more);
    if (!more2) return ENOMEM;
    size_t n2 = strlen(more2);
    while (n2 > 0 && (more2[n2 - 1] == ' ' || more2[n2 - 1] == '\t')) { more2[n2 - 1] = '\0'; n2--; }
    if (!more2[0]) { free(more2); return 0; }

    size_t n1 = strlen(old);
    char* out = (char*)malloc(n1 + 1 + n2 + 1);
    if (!out) { free(more2); return ENOMEM; }
    memcpy(out, old, n1);
    out[n1] = ' ';
    memcpy(out + n1 + 1, more2, n2);
    out[n1 + 1 + n2] = '\0';
    free((void*)old);
    free(more2);
    *io_dst = out;
    return 0;
}

/* The next whitespace-separated token at *io_p, heap-copied whole, or NULL
 * in *out at the end of the line. Returns ENOMEM only. */
static int next_token(char** io_p, char** out) {
    char* p = *io_p;
    *out = NULL;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == '\n' || *p == '\r') { *io_p = p; return 0; }
    char* start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
    size_t len = (size_t)(p - start);
    char* tok = (char*)malloc(len + 1);
    if (!tok) return ENOMEM;
    memcpy(tok, start, len);
    tok[len] = '\0';
    *io_p = p;
    *out = tok;
    return 0;
}

/* Append every remaining token on the line to a string list. */
static int append_tokens(char** io_p, const char*** io_list, size_t* io_count) {
    for (;;) {
        char* tok = NULL;
        int err = next_token(io_p, &tok);
        if (err) return err;
        if (!tok) return 0;
        err = append_str_list(io_list, io_count, tok);
        if (err) { free(tok); return err; }
    }
}

static void free_str_list(const char** list, size_t count) {
    for (size_t j = 0; j < count; ++j) free((void*)list[j]);
    free((void*)list);
}

static void free_target_fields(CCBuildTargetDecl* t) {
    free((void*)t->name);
    free_str_list(t->srcs, t->src_count);
    free_str_list(t->deps, t->dep_count);
    free((void*)t->out_name);
    free((void*)t->target_triple);
    free((void*)t->sysroot);
    free((void*)t->install_dest);
    free_str_list(t->include_dirs, t->include_dir_count);
    free_str_list(t->defines, t->define_count);
    free_str_list(t->libs, t->lib_count);
    free((void*)t->cflags);
    free((void*)t->ldflags);
    memset(t, 0, sizeof(*t));
}

/* A directive keyword at p followed by a blank. */
static int is_directive(const char* p, const char* kw) {
    size_t n = strlen(kw);
    return strncmp(p, kw, n) == 0 && (p[n] == ' ' || p[n] == '\t');
}

/* Every table here grows: a build file may declare any number of targets,
 * each with any number of sources, deps, include dirs, defines and libs, on
 * lines of any length. Nothing is truncated or dropped; a malformed line is
 * an error naming the file and the line. */
static int parse_build_targets(const char* path,
                              CCBuildTargetDecl** out_targets,
                              size_t* out_count,
                              char** out_default_name) {
    if (!out_targets || !out_count) return EINVAL;
    *out_targets = NULL;
    *out_count = 0;
    if (out_default_name) *out_default_name = NULL;
    FILE* f = fopen(path, "r");
    if (!f) return errno ? errno : -1;
    char* line = NULL;
    size_t line_cap = 0;
    CCBuildTargetDecl* targets = NULL;
    size_t count = 0;
    size_t cap = 0;
    int err = 0;
    size_t lineno = 0;
    // Pass 1: parse CC_DEFAULT + CC_TARGET entries.
    while (getline(&line, &line_cap, f) >= 0) {
        lineno++;
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (is_directive(p, "CC_DEFAULT")) {
            p += 10;
            char* name = NULL;
            err = next_token(&p, &name);
            if (err) break;
            if (!name) {
                fprintf(stderr, "%s:%zu: malformed CC_DEFAULT line\n", path, lineno);
                err = EINVAL;
                break;
            }
            if (out_default_name) {
                free(*out_default_name);
                *out_default_name = name;
            } else {
                free(name);
            }
            continue;
        }
        if (!is_directive(p, "CC_TARGET")) continue;
        p += 9;

        CCBuildTargetDecl t;
        memset(&t, 0, sizeof(t));
        char* kind_tok = NULL;
        err = next_token(&p, (char**)&t.name);
        if (!err && t.name) err = next_token(&p, &kind_tok);
        if (err) { free_target_fields(&t); break; }
        if (!t.name || !kind_tok) {
            fprintf(stderr, "%s:%zu: malformed CC_TARGET line\n", path, lineno);
            free_target_fields(&t);
            free(kind_tok);
            err = EINVAL;
            break;
        }
        t.kind = parse_target_kind(kind_tok);
        if (!t.kind) {
            fprintf(stderr, "%s:%zu: unknown target kind: %s\n", path, lineno, kind_tok);
            free_target_fields(&t);
            free(kind_tok);
            err = EINVAL;
            break;
        }
        free(kind_tok);
        if (find_target_index(targets, count, t.name) >= 0) {
            fprintf(stderr, "%s:%zu: duplicate CC_TARGET %s\n", path, lineno, t.name);
            free_target_fields(&t);
            err = EINVAL;
            break;
        }
        err = append_tokens(&p, &t.srcs, &t.src_count);
        if (err) { free_target_fields(&t); break; }
        if (t.src_count == 0) {
            fprintf(stderr, "%s:%zu: CC_TARGET must list at least 1 source\n", path, lineno);
            free_target_fields(&t);
            err = EINVAL;
            break;
        }
        if (count == cap) {
            size_t ncap = cap ? cap * 2 : 16;
            CCBuildTargetDecl* grown = (CCBuildTargetDecl*)realloc(targets, ncap * sizeof(*grown));
            if (!grown) { free_target_fields(&t); err = ENOMEM; break; }
            targets = grown;
            cap = ncap;
        }
        targets[count++] = t;
    }
    // Pass 2: attach per-target properties.
    if (err == 0) {
        rewind(f);
        lineno = 0;
        while (getline(&line, &line_cap, f) >= 0) {
            lineno++;
            char* p = line;
            while (*p == ' ' || *p == '\t') p++;

            const int is_deps = is_directive(p, "CC_TARGET_DEPS");
            const int is_out = is_directive(p, "CC_TARGET_OUT");
            const int is_tgt = is_directive(p, "CC_TARGET_TARGET");
            const int is_sys = is_directive(p, "CC_TARGET_SYSROOT");
            const int is_install = is_directive(p, "CC_INSTALL");
            const int is_inc = is_directive(p, "CC_TARGET_INCLUDE");
            const int is_cflags = is_directive(p, "CC_TARGET_CFLAGS");
            const int is_ldflags = is_directive(p, "CC_TARGET_LDFLAGS");
            const int is_def = is_directive(p, "CC_TARGET_DEFINE");
            const int is_libs = is_directive(p, "CC_TARGET_LIBS");

            if (!is_deps && !is_out && !is_tgt && !is_sys && !is_install && !is_inc && !is_cflags && !is_ldflags && !is_def && !is_libs) continue;

            const char* directive = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            int dlen = (int)(p - directive);

            char* name = NULL;
            err = next_token(&p, &name);
            if (err) break;
            if (!name) {
                fprintf(stderr, "%s:%zu: malformed %.*s line\n", path, lineno, dlen, directive);
                err = EINVAL;
                break;
            }
            int idx = find_target_index(targets, count, name);
            if (idx < 0) {
                fprintf(stderr, "%s:%zu: unknown target for property: %s\n", path, lineno, name);
                free(name);
                err = EINVAL;
                break;
            }
            free(name);
            CCBuildTargetDecl* t = &targets[(size_t)idx];

            if (is_deps) {
                err = append_tokens(&p, &t->deps, &t->dep_count);
            } else if (is_out || is_tgt || is_sys || is_install) {
                // Single string value after target name.
                char* tok = NULL;
                err = next_token(&p, &tok);
                if (err) break;
                if (!tok) {
                    fprintf(stderr, "%s:%zu: %.*s %s needs a value\n", path, lineno, dlen, directive, t->name);
                    err = EINVAL;
                    break;
                }
                const char** slot = is_out ? &t->out_name
                                  : is_tgt ? &t->target_triple
                                  : is_sys ? &t->sysroot
                                           : &t->install_dest;
                free((void*)*slot);
                *slot = tok;
            } else if (is_inc) {
                err = append_tokens(&p, &t->include_dirs, &t->include_dir_count);
            } else if (is_cflags) {
                err = append_flags_str(&t->cflags, p);
            } else if (is_ldflags) {
                err = append_flags_str(&t->ldflags, p);
            } else if (is_def) {
                err = append_tokens(&p, &t->defines, &t->define_count);
            } else {
                err = append_tokens(&p, &t->libs, &t->lib_count);
            }
            if (err) break;
        }
    }

    if (!err && ferror(f)) {
        fprintf(stderr, "%s: read error\n", path);
        err = EIO;
    }
    free(line);
    fclose(f);
    if (err == ENOMEM) {
        fprintf(stderr, "%s: out of memory reading build targets\n", path);
    }
    if (err != 0) {
        if (out_default_name && *out_default_name) { free(*out_default_name); *out_default_name = NULL; }
        for (size_t i = 0; i < count; ++i) free_target_fields(&targets[i]);
        free(targets);
        return err;
    }
    *out_targets = targets;
    *out_count = count;
    return 0;
}

int cc_build_list_targets(const char* build_path, CCBuildTargetDecl** out_targets, size_t* out_count, char** out_default_name) {
    if (!out_targets || !out_count) return EINVAL;
    *out_targets = NULL;
    *out_count = 0;
    if (out_default_name) *out_default_name = NULL;
    if (!build_path) return 0;
    if (!file_exists(build_path)) return 0;
    return parse_build_targets(build_path, out_targets, out_count, out_default_name);
}

void cc_build_free_targets(CCBuildTargetDecl* targets, size_t count, char* default_name) {
    free(default_name);
    if (!targets) return;
    for (size_t i = 0; i < count; ++i) free_target_fields(&targets[i]);
    free(targets);
}
