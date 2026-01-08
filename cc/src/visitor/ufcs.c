#include "ufcs.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// Simple identifier check (ASCII-only for now).
static int is_ident_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

// Map a receiver+method to a desugared function call prefix.
// Returns number of bytes written to out (not including args), or -1 on failure.
static int emit_desugared_call(char* out, size_t cap, const char* recv, const char* method, bool has_args) {
    if (!out || cap == 0 || !recv || !method) return -1;

    // Special cases for stdlib convenience.
    if (strcmp(method, "as_slice") == 0) {
        return snprintf(out, cap, "string_as_slice(&%s)", recv);
    }
    if (strcmp(method, "append") == 0) {
        return snprintf(out, cap, "string_append(&%s, ", recv);
    }
    if (strcmp(recv, "std_out") == 0 && strcmp(method, "write") == 0) {
        return snprintf(out, cap, "cc_std_out_write(");
    }
    // Generic UFCS: method(&recv,
    if (has_args) {
        return snprintf(out, cap, "%s(&%s, ", method, recv);
    }
    return snprintf(out, cap, "%s(&%s)", method, recv);
}

// Rewrite one line, handling nested method calls.
int cc_ufcs_rewrite_line(const char* in, char* out, size_t out_cap) {
    if (!in || !out || out_cap == 0) return -1;
    const char* p = in;
    char* o = out;
    size_t cap = out_cap;

    while (*p && cap > 1) {
        const char* dot = strchr(p, '.');
        if (!dot) break;
        // Identify receiver
        const char* r_end = dot - 1;
        while (r_end >= p && isspace((unsigned char)*r_end)) r_end--;
        if (r_end < p || !is_ident_char(*r_end)) {
            size_t chunk = (size_t)((dot + 1) - p);
            if (chunk >= cap) chunk = cap - 1;
            memcpy(o, p, chunk); o += chunk; cap -= chunk;
            p = dot + 1;
            continue;
        }
        const char* r_start = r_end;
        while (r_start > p && is_ident_char(*(r_start - 1))) r_start--;
        size_t recv_len = (size_t)(r_end - r_start + 1);

        // Identify method
        const char* m_start = dot + 1;
        while (*m_start && isspace((unsigned char)*m_start)) m_start++;
        if (!is_ident_char(*m_start)) {
            size_t chunk = (size_t)((dot + 1) - p);
            if (chunk >= cap) chunk = cap - 1;
            memcpy(o, p, chunk); o += chunk; cap -= chunk;
            p = dot + 1;
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
        int n = emit_desugared_call(o, cap, recv, method, has_args);
        if (n < 0 || (size_t)n >= cap) return -1;
        o += n; cap -= (size_t)n;
        size_t copy_len = args_out_len;
        if (copy_len >= cap) copy_len = cap - 1;
        memcpy(o, rewritten_args, copy_len); o += copy_len; cap -= copy_len;
        if (cap > 1 && has_args) { *o++ = ')'; cap--; }

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

