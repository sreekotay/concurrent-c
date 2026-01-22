#include "ufcs.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// Simple identifier check (ASCII-only for now).
static int is_ident_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

// Thread-local context: set to 1 when rewriting UFCS inside `await`.
// Channel ops should emit task-returning variants when set.
static _Thread_local int g_ufcs_await_context = 0;

// Thread-local context: set to 1 when receiver's resolved type is a pointer.
// Used for free() dispatch: ptr.free() -> cc_chan_free(ptr) vs handle.free() -> chan_free(handle).
static _Thread_local int g_ufcs_recv_type_is_ptr = 0;

// Map a receiver+method to a desugared function call prefix.
// Returns number of bytes written to out (not including args), or -1 on failure.
static const char* skip_ws(const char* s) {
    while (s && *s && isspace((unsigned char)*s)) s++;
    return s;
}

static void trim_ws_in_place(char* s) {
    if (!s) return;
    size_t n = strlen(s);
    size_t a = 0;
    while (a < n && isspace((unsigned char)s[a])) a++;
    size_t b = n;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    if (a > 0) memmove(s, s + a, b - a);
    s[b - a] = '\0';
}

static int is_ident_only(const char* s) {
    if (!s || !*s) return 0;
    if (!(isalpha((unsigned char)s[0]) || s[0] == '_')) return 0;
    for (const char* p = s; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '_')) return 0;
    }
    return 1;
}

static int is_addr_of_ident(const char* s) {
    if (!s) return 0;
    s = skip_ws(s);
    if (*s != '&') return 0;
    s++;
    s = skip_ws(s);
    return is_ident_only(s);
}

static int emit_desugared_call(char* out,
                               size_t cap,
                               const char* recv,
                               const char* method,
                               bool recv_is_ptr,
                               const char* args_rewritten,
                               bool has_args) {
    if (!out || cap == 0 || !recv || !method) return -1;

    /* Channel ergonomic sugar:
       Prefer the surface chan_* helpers and do NOT auto-take address-of for handles.
       Also avoids collisions with libc symbols like close/free.
       In await context, emit task-returning variants (cc_chan_*_task). */
    if (strcmp(method, "send") == 0) {
        if (g_ufcs_await_context) {
            /* Emit cc_chan_send_task(recv.raw, &val, sizeof(val)) */
            if (!has_args || !args_rewritten) {
                return snprintf(out, cap, "cc_chan_send_task((%s%s).raw, NULL, 0)",
                               recv_is_ptr ? "*" : "", recv);
            }
            return snprintf(out, cap,
                           "cc_chan_send_task((%s%s).raw, &(%s), sizeof(%s))",
                           recv_is_ptr ? "*" : "", recv, args_rewritten, args_rewritten);
        }
        if (!has_args || !args_rewritten) return snprintf(out, cap, "chan_send(%s%s)", recv_is_ptr ? "*":"", recv);
        return snprintf(out, cap, "chan_send(%s%s, %s)", recv_is_ptr ? "*":"", recv, args_rewritten);
    }
    if (strcmp(method, "recv") == 0) {
        if (g_ufcs_await_context) {
            /* Emit cc_chan_recv_task(recv.raw, ptr, sizeof(*ptr)) */
            if (!has_args || !args_rewritten) {
                return snprintf(out, cap, "cc_chan_recv_task((%s%s).raw, NULL, 0)",
                               recv_is_ptr ? "*" : "", recv);
            }
            return snprintf(out, cap,
                           "cc_chan_recv_task((%s%s).raw, %s, sizeof(*(%s)))",
                           recv_is_ptr ? "*" : "", recv, args_rewritten, args_rewritten);
        }
        if (!has_args || !args_rewritten) return snprintf(out, cap, "chan_recv(%s%s)", recv_is_ptr ? "*":"", recv);
        return snprintf(out, cap, "chan_recv(%s%s, %s)", recv_is_ptr ? "*":"", recv, args_rewritten);
    }
    if (strcmp(method, "send_take") == 0) {
        if (!has_args || !args_rewritten) return snprintf(out, cap, "chan_send_take(%s%s)", recv_is_ptr ? "*":"", recv);
        return snprintf(out, cap, "chan_send_take(%s%s, %s)", recv_is_ptr ? "*":"", recv, args_rewritten);
    }
    if (strcmp(method, "try_send") == 0) {
        if (!has_args || !args_rewritten) return snprintf(out, cap, "chan_try_send(%s%s)", recv_is_ptr ? "*":"", recv);
        return snprintf(out, cap, "chan_try_send(%s%s, %s)", recv_is_ptr ? "*":"", recv, args_rewritten);
    }
    if (strcmp(method, "try_recv") == 0) {
        if (!has_args || !args_rewritten) return snprintf(out, cap, "chan_try_recv(%s%s)", recv_is_ptr ? "*":"", recv);
        return snprintf(out, cap, "chan_try_recv(%s%s, %s)", recv_is_ptr ? "*":"", recv, args_rewritten);
    }
    if (strcmp(method, "close") == 0) {
        return snprintf(out, cap, "chan_close(%s%s)", recv_is_ptr ? "*":"", recv);
    }
    if (strcmp(method, "free") == 0) {
        /* If receiver's resolved type is a pointer (e.g., CCChan*), call cc_chan_free directly.
           Otherwise use chan_free macro which expects a handle with .raw field. */
        if (g_ufcs_recv_type_is_ptr) {
            return snprintf(out, cap, "cc_chan_free(%s%s)", recv_is_ptr ? "*":"", recv);
        }
        return snprintf(out, cap, "chan_free(%s%s)", recv_is_ptr ? "*":"", recv);
    }

    // Special cases for stdlib convenience.
    if (strcmp(method, "as_slice") == 0) {
        return recv_is_ptr ? snprintf(out, cap, "string_as_slice(%s)", recv)
                           : snprintf(out, cap, "string_as_slice(&%s)", recv);
    }
    if (strcmp(method, "append") == 0) {
        return recv_is_ptr ? snprintf(out, cap, "string_append(%s, ", recv)
                           : snprintf(out, cap, "string_append(&%s, ", recv);
    }
    
    /* Slice UFCS methods: s.len(), s.trim(), s.at(i), etc.
       These dispatch to CCSlice_* functions. */
    if (strcmp(method, "len") == 0) {
        return recv_is_ptr ? snprintf(out, cap, "CCSlice_len(%s)", recv)
                           : snprintf(out, cap, "CCSlice_len(&%s)", recv);
    }
    if (strcmp(method, "trim") == 0) {
        return recv_is_ptr ? snprintf(out, cap, "CCSlice_trim(%s)", recv)
                           : snprintf(out, cap, "CCSlice_trim(&%s)", recv);
    }
    if (strcmp(method, "trim_left") == 0) {
        return recv_is_ptr ? snprintf(out, cap, "CCSlice_trim_left(%s)", recv)
                           : snprintf(out, cap, "CCSlice_trim_left(&%s)", recv);
    }
    if (strcmp(method, "trim_right") == 0) {
        return recv_is_ptr ? snprintf(out, cap, "CCSlice_trim_right(%s)", recv)
                           : snprintf(out, cap, "CCSlice_trim_right(&%s)", recv);
    }
    if (strcmp(method, "is_empty") == 0) {
        return recv_is_ptr ? snprintf(out, cap, "CCSlice_is_empty(%s)", recv)
                           : snprintf(out, cap, "CCSlice_is_empty(&%s)", recv);
    }
    if (strcmp(method, "at") == 0) {
        if (!has_args || !args_rewritten) {
            return recv_is_ptr ? snprintf(out, cap, "CCSlice_at(%s, 0)", recv)
                               : snprintf(out, cap, "CCSlice_at(&%s, 0)", recv);
        }
        return recv_is_ptr ? snprintf(out, cap, "CCSlice_at(%s, %s)", recv, args_rewritten)
                           : snprintf(out, cap, "CCSlice_at(&%s, %s)", recv, args_rewritten);
    }
    if (strcmp(method, "sub") == 0) {
        if (!has_args || !args_rewritten) {
            return recv_is_ptr ? snprintf(out, cap, "CCSlice_sub(%s, 0, 0)", recv)
                               : snprintf(out, cap, "CCSlice_sub(&%s, 0, 0)", recv);
        }
        return recv_is_ptr ? snprintf(out, cap, "CCSlice_sub(%s, %s)", recv, args_rewritten)
                           : snprintf(out, cap, "CCSlice_sub(&%s, %s)", recv, args_rewritten);
    }
    if (strcmp(method, "starts_with") == 0) {
        if (!has_args || !args_rewritten) {
            return recv_is_ptr ? snprintf(out, cap, "CCSlice_starts_with(%s, (CCSlice){0})", recv)
                               : snprintf(out, cap, "CCSlice_starts_with(&%s, (CCSlice){0})", recv);
        }
        return recv_is_ptr ? snprintf(out, cap, "CCSlice_starts_with(%s, %s)", recv, args_rewritten)
                           : snprintf(out, cap, "CCSlice_starts_with(&%s, %s)", recv, args_rewritten);
    }
    if (strcmp(method, "ends_with") == 0) {
        if (!has_args || !args_rewritten) {
            return recv_is_ptr ? snprintf(out, cap, "CCSlice_ends_with(%s, (CCSlice){0})", recv)
                               : snprintf(out, cap, "CCSlice_ends_with(&%s, (CCSlice){0})", recv);
        }
        return recv_is_ptr ? snprintf(out, cap, "CCSlice_ends_with(%s, %s)", recv, args_rewritten)
                           : snprintf(out, cap, "CCSlice_ends_with(&%s, %s)", recv, args_rewritten);
    }
    if (strcmp(method, "eq") == 0) {
        if (!has_args || !args_rewritten) {
            return recv_is_ptr ? snprintf(out, cap, "CCSlice_eq(%s, (CCSlice){0})", recv)
                               : snprintf(out, cap, "CCSlice_eq(&%s, (CCSlice){0})", recv);
        }
        return recv_is_ptr ? snprintf(out, cap, "CCSlice_eq(%s, %s)", recv, args_rewritten)
                           : snprintf(out, cap, "CCSlice_eq(&%s, %s)", recv, args_rewritten);
    }
    if (strcmp(recv, "std_out") == 0 && strcmp(method, "write") == 0) {
        /* Overload selection lives in the compiler:
           - String: cc_std_out_write_string(&s) (or pass-through if already &s)
           - "literal"/char[:]: cc_std_out_write(cc_slice_from_buffer("lit", sizeof("lit")-1))
           - Anything else: cc_std_out_write(expr) (expecting CCSlice) */
        if (!has_args || !args_rewritten) return snprintf(out, cap, "cc_std_out_write(");

        char tmp[512];
        strncpy(tmp, args_rewritten, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        trim_ws_in_place(tmp);

        if (tmp[0] == '"') {
            /* Wrap string literal as CCSlice. */
            return snprintf(out, cap, "cc_std_out_write(cc_slice_from_buffer(%s, sizeof(%s) - 1))", tmp, tmp);
        }
        if (is_ident_only(tmp)) {
            return snprintf(out, cap, "cc_std_out_write_string(&%s)", tmp);
        }
        if (is_addr_of_ident(tmp)) {
            return snprintf(out, cap, "cc_std_out_write_string(%s)", tmp);
        }
        return snprintf(out, cap, "cc_std_out_write(");
    }
    if (strcmp(recv, "std_err") == 0 && strcmp(method, "write") == 0) {
        if (!has_args || !args_rewritten) return snprintf(out, cap, "cc_std_err_write(");
        char tmp[512];
        strncpy(tmp, args_rewritten, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        trim_ws_in_place(tmp);
        if (tmp[0] == '"') {
            return snprintf(out, cap, "cc_std_err_write(cc_slice_from_buffer(%s, sizeof(%s) - 1))", tmp, tmp);
        }
        if (is_ident_only(tmp)) {
            return snprintf(out, cap, "cc_std_err_write_string(&%s)", tmp);
        }
        if (is_addr_of_ident(tmp)) {
            return snprintf(out, cap, "cc_std_err_write_string(%s)", tmp);
        }
        return snprintf(out, cap, "cc_std_err_write(");
    }
    // Generic UFCS: method(&recv,
    if (has_args) {
        return recv_is_ptr ? snprintf(out, cap, "%s(%s, ", method, recv)
                           : snprintf(out, cap, "%s(&%s, ", method, recv);
    }
    return recv_is_ptr ? snprintf(out, cap, "%s(%s)", method, recv)
                       : snprintf(out, cap, "%s(&%s)", method, recv);
}

// Rewrite one line, handling nested method calls.
int cc_ufcs_rewrite_line(const char* in, char* out, size_t out_cap) {
    if (!in || !out || out_cap == 0) return -1;
    const char* p = in;
    char* o = out;
    size_t cap = out_cap;

    while (*p && cap > 1) {
        const char* dot = strstr(p, ".");
        const char* arrow = strstr(p, "->");
        const char* sep = NULL;
        bool recv_is_ptr = false;
        if (arrow && (!dot || arrow < dot)) { sep = arrow; recv_is_ptr = true; }
        else { sep = dot; recv_is_ptr = false; }
        if (!sep) break;
        // Identify receiver
        const char* r_end = sep - 1;
        while (r_end >= p && isspace((unsigned char)*r_end)) r_end--;
        if (r_end < p || !is_ident_char(*r_end)) {
            size_t chunk = (size_t)((sep + (recv_is_ptr ? 2 : 1)) - p);
            if (chunk >= cap) chunk = cap - 1;
            memcpy(o, p, chunk); o += chunk; cap -= chunk;
            p = sep + (recv_is_ptr ? 2 : 1);
            continue;
        }
        const char* r_start = r_end;
        while (r_start > p && is_ident_char(*(r_start - 1))) r_start--;
        size_t recv_len = (size_t)(r_end - r_start + 1);

        // Identify method
        const char* m_start = sep + (recv_is_ptr ? 2 : 1);
        while (*m_start && isspace((unsigned char)*m_start)) m_start++;
        if (!is_ident_char(*m_start)) {
            size_t chunk = (size_t)((sep + (recv_is_ptr ? 2 : 1)) - p);
            if (chunk >= cap) chunk = cap - 1;
            memcpy(o, p, chunk); o += chunk; cap -= chunk;
            p = sep + (recv_is_ptr ? 2 : 1);
            continue;
        }
        const char* m_end = m_start;
        while (is_ident_char(*m_end)) m_end++;
        size_t method_len = (size_t)(m_end - m_start);

        // Next non-space after method must be '('
        const char* paren = m_end;
        while (*paren && isspace((unsigned char)*paren)) paren++;
        if (*paren != '(') {
            size_t chunk = (size_t)((dot + 1) - p);
            if (chunk >= cap) chunk = cap - 1;
            memcpy(o, p, chunk); o += chunk; cap -= chunk;
            p = dot + 1;
            continue;
        }

        // Extract args (balanced parentheses)
        int depth = 1;
        const char* args_start = paren + 1;
        const char* args_end = args_start;
        while (*args_end && depth > 0) {
            if (*args_end == '(') depth++;
            else if (*args_end == ')') depth--;
            args_end++;
        }
        if (depth != 0) {
            size_t chunk = (size_t)((dot + 1) - p);
            if (chunk >= cap) chunk = cap - 1;
            memcpy(o, p, chunk); o += chunk; cap -= chunk;
            p = dot + 1;
            continue;
        }
        args_end--; // points to ')'

        // Emit prefix before receiver
        size_t prefix_len = (size_t)(r_start - p);
        if (prefix_len >= cap) prefix_len = cap - 1;
        memcpy(o, p, prefix_len); o += prefix_len; cap -= prefix_len;

        // Build receiver/method strings
        char recv[64] = {0};
        char method[64] = {0};
        if (recv_len >= sizeof(recv) || method_len >= sizeof(method)) {
            // Too long; fall back to copying original segment.
            size_t seg_len = (size_t)(args_end - p + 1);
            if (seg_len >= cap) seg_len = cap - 1;
            memcpy(o, p, seg_len); o += seg_len; cap -= seg_len;
            p = args_end + 1;
            continue;
        }
        memcpy(recv, r_start, recv_len); recv[recv_len] = '\0';
        memcpy(method, m_start, method_len); method[method_len] = '\0';

        // Rewrite arguments recursively to catch nested UFCS.
        char inner[512] = {0};
        size_t arg_len = (size_t)(args_end - args_start);
        if (arg_len >= sizeof(inner)) arg_len = sizeof(inner) - 1;
        memcpy(inner, args_start, arg_len); inner[arg_len] = '\0';

        char rewritten_args[512] = {0};
        if (cc_ufcs_rewrite_line(inner, rewritten_args, sizeof(rewritten_args)) != 0) {
            strncpy(rewritten_args, inner, sizeof(rewritten_args) - 1);
        }

        size_t args_out_len = strnlen(rewritten_args, sizeof(rewritten_args));
        bool has_args = args_out_len > 0;

        // Emit desugared call
        int n = emit_desugared_call(o, cap, recv, method, recv_is_ptr, rewritten_args, has_args);
        if (n < 0 || (size_t)n >= cap) return -1;
        o += n; cap -= (size_t)n;
        /* If we emitted a full call for std_out/std_err.write overload, we don't
           append args/closing paren. Detect that by checking for a trailing ')'. */
        if (o > out && *(o - 1) == ')') {
            /* already complete */
        } else {
            size_t copy_len = args_out_len;
            if (copy_len >= cap) copy_len = cap - 1;
            memcpy(o, rewritten_args, copy_len); o += copy_len; cap -= copy_len;
            if (cap > 1 && has_args) { *o++ = ')'; cap--; }
        }

        p = args_end + 1;
    }

    // Copy any remaining tail
    while (*p && cap > 1) { *o++ = *p++; cap--; }
    *o = '\0';
    return 0;
}

int cc_ufcs_rewrite(CCASTRoot* root) {
    (void)root;
    // Rewriter now handled per-line in visitor; AST rewrite will come with TCC.
    return 0;
}

// Rewrite UFCS with await context: channel ops emit task-returning variants.
int cc_ufcs_rewrite_line_await(const char* in, char* out, size_t out_cap, int is_await) {
    g_ufcs_await_context = is_await;
    int rc = cc_ufcs_rewrite_line(in, out, out_cap);
    g_ufcs_await_context = 0;
    return rc;
}

// Extended rewrite with type info from TCC.
int cc_ufcs_rewrite_line_ex(const char* in, char* out, size_t out_cap, int is_await, int recv_type_is_ptr) {
    g_ufcs_await_context = is_await;
    g_ufcs_recv_type_is_ptr = recv_type_is_ptr;
    int rc = cc_ufcs_rewrite_line(in, out, out_cap);
    g_ufcs_await_context = 0;
    g_ufcs_recv_type_is_ptr = 0;
    return rc;
}