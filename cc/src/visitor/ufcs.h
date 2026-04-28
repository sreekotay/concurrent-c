#ifndef CC_VISITOR_UFCS_H
#define CC_VISITOR_UFCS_H

#include <stddef.h>
#include <string.h>

#include "ast/ast.h"
#include "comptime/symbols.h"

enum {
    CC_UFCS_REWRITE_OK = 0,
    CC_UFCS_REWRITE_NO_MATCH = 1,
    CC_UFCS_REWRITE_UNRESOLVED = 2,
    CC_UFCS_REWRITE_ERROR = -1,
};

typedef enum {
    CC_UFCS_CHANNEL_KIND_NONE = 0,
    CC_UFCS_CHANNEL_KIND_TX,
    CC_UFCS_CHANNEL_KIND_RX,
    CC_UFCS_CHANNEL_KIND_RAW,
} CCUfcsChannelKind;

// Rewrite a single source line in-place into the out buffer (UTF-8 C text).
// Returns CC_UFCS_REWRITE_OK on success; CC_UFCS_REWRITE_NO_MATCH when no UFCS
// patterns are present; CC_UFCS_REWRITE_UNRESOLVED when a UFCS call has no
// explicit owner; and CC_UFCS_REWRITE_ERROR on malformed input.
int cc_ufcs_rewrite_line(const char* in, char* out, size_t out_cap);

// Rewrite UFCS with await context flag. When is_await=1, channel ops (send/recv)
// emit task-returning variants (cc_chan_*_task) for use in @async functions.
int cc_ufcs_rewrite_line_await(const char* in, char* out, size_t out_cap, int is_await);

// Extended rewrite with type info. recv_type_is_ptr=1 when the receiver's resolved
// type is a pointer (from TCC), enabling correct dispatch for ptr.free() vs handle.free().
int cc_ufcs_rewrite_line_ex(const char* in, char* out, size_t out_cap, int is_await, int recv_type_is_ptr);

// Full UFCS rewrite with receiver type from TCC. If recv_type is non-NULL,
// generates TypeName_method(&recv, ...) for struct types.
int cc_ufcs_rewrite_line_full(const char* in, char* out, size_t out_cap, 
                              int is_await, int recv_type_is_ptr, const char* recv_type);

/* Provide the active compile-time symbol table for UFCS registry lookups. */
void cc_ufcs_set_symbols(CCSymbolTable* symbols);
void cc_ufcs_set_source_context(const char* source_text, size_t source_offset);

/* Compose the callee name used by the global `cc_type_register("*", ...)`
 * default UFCS hook: `CCFoo.bar()` -> `cc_foo_bar(...)`, and bare CamelCase
 * user types like `RedisConn.retain()` -> `redis_conn_retain(...)`.
 *
 * This is a text-only mirror of cc_ufcs_generic_cc_prefix_lower_c for parser
 * survival paths that cannot invoke comptime hooks yet. Authoritative UFCS
 * lowering still goes through the registered hook machinery. */
static inline int cc_ufcs_compose_default_callee(char* out,
                                                 size_t out_sz,
                                                 const char* type_name,
                                                 const char* method_name) {
    const char* t = type_name;
    size_t tlen;
    size_t wi = 0;
    size_t body_start = 0;
    int has_cc_prefix = 0;
    if (!out || out_sz == 0 || !type_name || !method_name || !method_name[0]) return 0;
    out[0] = '\0';
    while (*t == ' ' || *t == '\t') t++;
    if (strncmp(t, "struct ", 7) == 0) t += 7;
    else if (strncmp(t, "union ", 6) == 0) t += 6;
    tlen = strlen(t);
    while (tlen > 0 && (t[tlen - 1] == '*' || t[tlen - 1] == ' ' || t[tlen - 1] == '\t')) tlen--;
    if (tlen == 0) return 0;
    has_cc_prefix = (tlen > 2 && t[0] == 'C' && t[1] == 'C' && t[2] >= 'A' && t[2] <= 'Z');
    if (!((t[0] >= 'A' && t[0] <= 'Z') || (t[0] >= 'a' && t[0] <= 'z') || t[0] == '_')) return 0;
    if (has_cc_prefix) {
        if (wi + 3 >= out_sz) return 0;
        out[wi++] = 'c';
        out[wi++] = 'c';
        out[wi++] = '_';
        body_start = 2;
    }
    for (size_t ri = body_start; ri < tlen; ++ri) {
        char c = t[ri];
        int is_upper = (c >= 'A' && c <= 'Z');
        if (is_upper && ri > body_start) {
            if (wi + 1 >= out_sz) return 0;
            out[wi++] = '_';
        }
        if (wi + 1 >= out_sz) return 0;
        out[wi++] = is_upper ? (char)(c + ('a' - 'A')) : c;
    }
    if (wi + 1 + strlen(method_name) + 1 > out_sz) return 0;
    out[wi++] = '_';
    strcpy(out + wi, method_name);
    return 1;
}

/* Resolve builtin channel UFCS lowering from receiver type + method + mode. */
static inline const char* cc_ufcs_channel_callee(const char* recv_type_name,
                                                 const char* method,
                                                 int is_await,
                                                 CCUfcsChannelKind* out_kind,
                                                 int* out_recv_by_value) {
    int is_tx = 0;
    int is_rx = 0;
    int is_raw = 0;

    if (out_kind) *out_kind = CC_UFCS_CHANNEL_KIND_NONE;
    if (out_recv_by_value) *out_recv_by_value = 0;
    if (!recv_type_name || !method) return NULL;

    is_tx = (strncmp(recv_type_name, "CCChanTx_", 9) == 0 ||
             strcmp(recv_type_name, "CCChanTx") == 0 ||
             strcmp(recv_type_name, "CCChanTx*") == 0);
    is_rx = (strncmp(recv_type_name, "CCChanRx_", 9) == 0 ||
             strcmp(recv_type_name, "CCChanRx") == 0 ||
             strcmp(recv_type_name, "CCChanRx*") == 0);
    is_raw = (strcmp(recv_type_name, "CCChan") == 0 ||
              strcmp(recv_type_name, "CCChan*") == 0);

    if (is_tx) {
        if (out_kind) *out_kind = CC_UFCS_CHANNEL_KIND_TX;
        if (out_recv_by_value) *out_recv_by_value = 1;
        if (strcmp(method, "send") == 0) return is_await ? "cc_channel_send_task" : "cc_channel_send";
        if (strcmp(method, "try_send") == 0) return "cc_channel_try_send";
        if (strcmp(method, "send_take") == 0) return "cc_channel_send_take";
        if (strcmp(method, "send_task") == 0) return "cc_channel_send_task";
        if (strcmp(method, "send_task_hybrid") == 0) return "cc_channel_send_task_hybrid";
        if (strcmp(method, "close") == 0) return "cc_channel_close";
        if (strcmp(method, "free") == 0) return "cc_channel_free";
        return NULL;
    }

    if (is_rx) {
        if (out_kind) *out_kind = CC_UFCS_CHANNEL_KIND_RX;
        if (out_recv_by_value) *out_recv_by_value = 1;
        if (strcmp(method, "recv") == 0) return is_await ? "cc_channel_recv_task" : "cc_channel_recv";
        if (strcmp(method, "try_recv") == 0) return "cc_channel_try_recv";
        if (strcmp(method, "close") == 0) return "cc_channel_close";
        if (strcmp(method, "free") == 0) return "cc_channel_free";
        return NULL;
    }

    if (is_raw) {
        if (out_kind) *out_kind = CC_UFCS_CHANNEL_KIND_RAW;
        if (out_recv_by_value) *out_recv_by_value = 1;
        if (strcmp(method, "recv") == 0) return is_await ? "cc_channel_recv_task" : "cc_channel_recv";
        if (strcmp(method, "try_recv") == 0) return "cc_channel_try_recv";
        if (strcmp(method, "close") == 0) return "cc_channel_close";
        if (strcmp(method, "free") == 0) return "cc_channel_free";
    }

    return NULL;
}

#endif // CC_VISITOR_UFCS_H

