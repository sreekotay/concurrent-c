#include "ufcs.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccc/std/prelude.cch>
#include <ccc/std/string.cch>

#include "comptime/symbols.h"
#include "preprocess/type_registry.h"

// Simple identifier check (ASCII-only for now).
static int is_ident_start(char c) {
    return isalpha((unsigned char)c) || c == '_';
}

static int is_ident_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

// Thread-local context: set to 1 when rewriting UFCS inside `await`.
// Channel ops should emit task-returning variants when set.
static _Thread_local int g_ufcs_await_context = 0;

// Thread-local context: set to 1 when receiver's resolved type is a pointer.
// Used only by explicit type-driven channel dispatch.
static _Thread_local int g_ufcs_recv_type_is_ptr = 0;

// Thread-local context: receiver type name from TCC (e.g., "Point", "Vec_int").
// When set, UFCS generates TypeName_method(&recv, ...) for struct types.
static _Thread_local const char* g_ufcs_recv_type = NULL;
static _Thread_local CCSymbolTable* g_ufcs_symbols = NULL;

void cc_ufcs_set_symbols(CCSymbolTable* symbols) {
    g_ufcs_symbols = symbols;
}

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

static int cc__skip_balanced_suffix(const char** pp, char open, char close) {
    const char* p = *pp;
    int depth = 1;
    if (!p || *p != open) return 0;
    p++;
    while (*p && depth > 0) {
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            while (*p) {
                if (*p == '\\' && p[1]) { p += 2; continue; }
                if (*p == q) { p++; break; }
                p++;
            }
            continue;
        }
        if (*p == open) depth++;
        else if (*p == close) depth--;
        p++;
    }
    if (depth != 0) return 0;
    *pp = p;
    return 1;
}

static int cc__is_addressable_lvalue_expr(const char* s) {
    const char* p = skip_ws(s);
    if (!p || !is_ident_start(*p)) return 0;
    while (is_ident_char(*p)) p++;
    for (;;) {
        p = skip_ws(p);
        if (*p == '.') {
            p = skip_ws(p + 1);
            if (!is_ident_start(*p)) return 0;
            while (is_ident_char(*p)) p++;
            continue;
        }
        if (p[0] == '-' && p[1] == '>') {
            p = skip_ws(p + 2);
            if (!is_ident_start(*p)) return 0;
            while (is_ident_char(*p)) p++;
            continue;
        }
        if (*p == '[') {
            if (!cc__skip_balanced_suffix(&p, '[', ']')) return 0;
            continue;
        }
        break;
    }
    p = skip_ws(p);
    return *p == '\0';
}

static const char* cc__result_err_type_suffix(const char* type_name) {
    if (!type_name || strncmp(type_name, "CCResult_", 9) != 0) return NULL;
    const char* last_us = strrchr(type_name, '_');
    if (!last_us || !last_us[1]) return NULL;
    return last_us + 1;
}

static int cc__is_string_recv_type(const char* type_name) {
    return type_name &&
           (strcmp(type_name, "CCString") == 0 ||
            strcmp(type_name, "CCString*") == 0 ||
            strcmp(type_name, "__CCVecGeneric") == 0 ||
            strcmp(type_name, "__CCVecGeneric*") == 0 ||
            strcmp(type_name, "Vec_char") == 0 ||
            strcmp(type_name, "Vec_char*") == 0);
}

static int cc__is_slice_recv_type(const char* type_name) {
    return type_name &&
           (strcmp(type_name, "CCSlice") == 0 ||
            strcmp(type_name, "CCSlice*") == 0 ||
            strcmp(type_name, "CCSliceUnique") == 0 ||
            strcmp(type_name, "CCSliceUnique*") == 0);
}

static int cc__is_family_recv_type(const char* type_name) {
    return type_name &&
           (strncmp(type_name, "Vec_", 4) == 0 ||
            strncmp(type_name, "Map_", 4) == 0 ||
            strncmp(type_name, "CCResult_", 9) == 0 ||
            strncmp(type_name, "CCOptional_", 11) == 0);
}

static int cc__is_parser_vec_recv_type(const char* type_name) {
    return type_name &&
           (strcmp(type_name, "__CCVecGeneric") == 0 ||
            strcmp(type_name, "__CCVecGeneric*") == 0);
}

static int cc__is_parser_map_recv_type(const char* type_name) {
    return type_name &&
           (strcmp(type_name, "__CCMapGeneric") == 0 ||
            strcmp(type_name, "__CCMapGeneric*") == 0);
}

static int cc__is_untyped_channel_recv_type(const char* type_name) {
    return type_name &&
           (strcmp(type_name, "CCChan") == 0 ||
            strcmp(type_name, "CCChan*") == 0);
}

static int cc__ufcs_rewrite_line_simple(const char* in, char* out, size_t out_cap);
typedef CCSlice (*CCUfcsCompiledCallable)(CCSlice method, CCSliceArray argv, CCArena *arena);

#define CC_UFCS_VALUE_TAG "__cc_ufcs_value__:"

typedef struct {
    const char* recv_type_name;
    const char* typed_chan_type;
    int recv_is_simple;
    int recv_is_addressable;
} CCUFCSDispatchCtx;

static int cc__recv_pass_direct(const CCUFCSDispatchCtx* ctx, bool recv_is_ptr) {
    return recv_is_ptr || !ctx || !ctx->recv_is_addressable;
}

static int cc__emit_registered_callable(char* out,
                                        size_t cap,
                                        const char* recv,
                                        const char* method,
                                        int recv_is_addressable,
                                        bool recv_is_ptr,
                                        const char* recv_type_name,
                                        const char* args_rewritten,
                                        bool has_args) {
    const void* fn_ptr = NULL;
    CCSlice argv_items[5];
    CCSliceArray argv = {0};
    CCArena arena = cc_heap_arena(1024);
    CCUfcsCompiledCallable fn;
    CCSlice method_slice;
    CCSlice lowered;
    char lowered_name[256];
    size_t lowered_len = 0;
    int receiver_by_value = 0;
    if (!g_ufcs_symbols || !recv_type_name) return -1;
    /* Real comptime UFCS path for callable registrations collected from named
       handlers and non-capturing lambdas. */
    if (cc_symbols_lookup_ufcs_callable(g_ufcs_symbols, recv_type_name, &fn_ptr) != 0 || !fn_ptr) {
        cc_heap_arena_free(&arena);
        return -1;
    }
    fn = (CCUfcsCompiledCallable)fn_ptr;
    argv_items[argv.len++] = cc_slice_from_buffer((void*)recv_type_name, strlen(recv_type_name));
    argv_items[argv.len++] = cc_slice_from_buffer((void*)recv, strlen(recv));
    if (has_args && args_rewritten && args_rewritten[0]) {
        argv_items[argv.len++] = cc_slice_from_buffer((void*)args_rewritten, strlen(args_rewritten));
    } else {
        argv_items[argv.len++] = cc_slice_empty();
    }
    argv_items[argv.len++] = cc_slice_from_buffer((void*)(g_ufcs_await_context ? "await" : "sync"),
                                                  g_ufcs_await_context ? sizeof("await") - 1 : sizeof("sync") - 1);
    argv_items[argv.len++] = cc_slice_from_buffer((void*)(recv_is_ptr ? "ptr" : "value"),
                                                  recv_is_ptr ? sizeof("ptr") - 1 : sizeof("value") - 1);
    argv.items = argv_items;
    method_slice = cc_slice_from_buffer((void*)method, strlen(method));
    lowered = fn(method_slice, argv, &arena);
    if (!lowered.ptr || lowered.len == 0) {
        cc_heap_arena_free(&arena);
        return -1;
    }
    if (lowered.len > sizeof(CC_UFCS_VALUE_TAG) - 1 &&
        memcmp(lowered.ptr, CC_UFCS_VALUE_TAG, sizeof(CC_UFCS_VALUE_TAG) - 1) == 0) {
        receiver_by_value = 1;
        lowered.ptr = (char*)lowered.ptr + (sizeof(CC_UFCS_VALUE_TAG) - 1);
        lowered.len -= (sizeof(CC_UFCS_VALUE_TAG) - 1);
    }
    lowered_len = lowered.len < sizeof(lowered_name) - 1 ? lowered.len : sizeof(lowered_name) - 1;
    memcpy(lowered_name, lowered.ptr, lowered_len);
    lowered_name[lowered_len] = '\0';
    cc_heap_arena_free(&arena);
    if (getenv("CC_DEBUG_COMPTIME_UFCS")) {
        fprintf(stderr, "CC_DEBUG_COMPTIME_UFCS: receiver type %s lowered %s -> %s\n",
                recv_type_name, method, lowered_name);
    }
    if (has_args) {
        if (receiver_by_value) {
            return snprintf(out, cap, "%s(%s, ", lowered_name, recv);
        }
        if (recv_is_ptr || !recv_is_addressable)
            return snprintf(out, cap, "%s(%s, ", lowered_name, recv);
        return snprintf(out, cap, "%s(&%s, ", lowered_name, recv);
    }
    if (receiver_by_value) return snprintf(out, cap, "%s(%s)", lowered_name, recv);
    if (recv_is_ptr || !recv_is_addressable)
        return snprintf(out, cap, "%s(%s)", lowered_name, recv);
    return snprintf(out, cap, "%s(&%s)", lowered_name, recv);
}

static void cc__resolve_dispatch_ctx(CCUFCSDispatchCtx* ctx, const char* recv) {
    CCTypeRegistry* reg = cc_type_registry_get_global();
    const char* reg_type_name = NULL;
    int reg_recv_is_ptr = 0;
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->recv_is_simple = is_ident_only(recv) || is_addr_of_ident(recv);
    ctx->recv_is_addressable = ctx->recv_is_simple || cc__is_addressable_lvalue_expr(recv);
    ctx->recv_type_name = g_ufcs_recv_type;
    if (reg) {
        reg_type_name = cc_type_registry_resolve_receiver_expr(reg, recv, &reg_recv_is_ptr);
        if (!ctx->recv_type_name ||
            ((cc__is_parser_vec_recv_type(ctx->recv_type_name) ||
              cc__is_parser_map_recv_type(ctx->recv_type_name) ||
              strcmp(ctx->recv_type_name, "CCChanTx") == 0 ||
              strcmp(ctx->recv_type_name, "CCChanRx") == 0) &&
             reg_type_name && reg_type_name[0])) {
            ctx->recv_type_name = reg_type_name;
        }
    }
    if (reg_recv_is_ptr) g_ufcs_recv_type_is_ptr = 1;
    if (ctx->recv_type_name &&
        (strncmp(ctx->recv_type_name, "CCChanTx_", 9) == 0 ||
         strncmp(ctx->recv_type_name, "CCChanRx_", 9) == 0)) {
        ctx->typed_chan_type = ctx->recv_type_name;
    } else if (reg_type_name &&
               (strncmp(reg_type_name, "CCChanTx_", 9) == 0 ||
                strncmp(reg_type_name, "CCChanRx_", 9) == 0)) {
        ctx->typed_chan_type = reg_type_name;
    }
}

static int cc__emit_type_driven_dispatch(char* out,
                                         size_t cap,
                                         const char* recv,
                                         const char* method,
                                         bool recv_is_ptr,
                                         const char* args_rewritten,
                                         bool has_args,
                                         const CCUFCSDispatchCtx* ctx) {
    if (!ctx) return -1;
    if (ctx->recv_type_name) {
        int callable_len = cc__emit_registered_callable(out, cap, recv, method, ctx->recv_is_addressable,
                                                        recv_is_ptr, ctx->recv_type_name, args_rewritten, has_args);
        if (callable_len >= 0) return callable_len;
    }
    if (g_ufcs_symbols && ctx->recv_type_name) {
        const char* registered_prefix = NULL;
        if (cc_symbols_lookup_ufcs_prefix(g_ufcs_symbols, ctx->recv_type_name, &registered_prefix) == 0 &&
            registered_prefix && registered_prefix[0]) {
            if (getenv("CC_DEBUG_COMPTIME_UFCS")) {
                fprintf(stderr, "CC_DEBUG_COMPTIME_UFCS: receiver type %s uses prefix %s\n",
                        ctx->recv_type_name, registered_prefix);
            }
            if (has_args) {
                if (cc__recv_pass_direct(ctx, recv_is_ptr))
                    return snprintf(out, cap, "%s%s(%s, ", registered_prefix, method, recv);
                return snprintf(out, cap, "%s%s(&%s, ", registered_prefix, method, recv);
            }
            if (cc__recv_pass_direct(ctx, recv_is_ptr))
                return snprintf(out, cap, "%s%s(%s)", registered_prefix, method, recv);
            return snprintf(out, cap, "%s%s(&%s)", registered_prefix, method, recv);
        }
    }
    if (ctx->recv_type_name &&
        (strcmp(ctx->recv_type_name, "CCFile") == 0 || strcmp(ctx->recv_type_name, "CCFile*") == 0)) {
        int file_recv_is_ptr = recv_is_ptr || strcmp(ctx->recv_type_name, "CCFile*") == 0;
        if (has_args) {
            if (cc__recv_pass_direct(ctx, file_recv_is_ptr))
                return snprintf(out, cap, "cc_file_%s(%s, ", method, recv);
            return snprintf(out, cap, "cc_file_%s(&%s, ", method, recv);
        }
        if (cc__recv_pass_direct(ctx, file_recv_is_ptr))
            return snprintf(out, cap, "cc_file_%s(%s)", method, recv);
        return snprintf(out, cap, "cc_file_%s(&%s)", method, recv);
    }
    if (ctx->typed_chan_type) {
        if (g_ufcs_await_context &&
            (strcmp(method, "send") == 0 || strcmp(method, "recv") == 0)) {
            if (!has_args || !args_rewritten) {
                return snprintf(out, cap, "%s((%s%s).raw, NULL, 0)",
                                strcmp(method, "send") == 0 ? "cc_channel_send_task" : "cc_channel_recv_task",
                                recv_is_ptr ? "*" : "", recv);
            }
            if (strcmp(method, "send") == 0) {
                return snprintf(out, cap, "cc_channel_send_task((%s%s).raw, &(%s), sizeof(%s))",
                                recv_is_ptr ? "*" : "", recv, args_rewritten, args_rewritten);
            }
            return snprintf(out, cap, "cc_channel_recv_task((%s%s).raw, %s, sizeof(*(%s)))",
                            recv_is_ptr ? "*" : "", recv, args_rewritten, args_rewritten);
        }
        if (strcmp(method, "send") == 0 || strcmp(method, "recv") == 0 ||
            strcmp(method, "try_send") == 0 || strcmp(method, "try_recv") == 0 ||
            strcmp(method, "close") == 0 || strcmp(method, "free") == 0) {
            if (!has_args || !args_rewritten) {
                return snprintf(out, cap, "%s_%s(%s%s)", ctx->typed_chan_type, method,
                                recv_is_ptr ? "*" : "", recv);
            }
            return snprintf(out, cap, "%s_%s(%s%s, %s)", ctx->typed_chan_type, method,
                            recv_is_ptr ? "*" : "", recv, args_rewritten);
        }
    }
    if (cc__is_untyped_channel_recv_type(ctx->recv_type_name)) {
        int chan_recv_is_ptr = recv_is_ptr || g_ufcs_recv_type_is_ptr ||
                               strcmp(ctx->recv_type_name, "CCChan*") == 0;
        if (strcmp(method, "close") == 0) {
            return chan_recv_is_ptr ? snprintf(out, cap, "cc_channel_close(%s)", recv)
                                    : snprintf(out, cap, "cc_channel_close(&%s)", recv);
        }
        if (strcmp(method, "free") == 0) {
            return chan_recv_is_ptr ? snprintf(out, cap, "cc_channel_free(%s)", recv)
                                    : snprintf(out, cap, "cc_channel_free(&%s)", recv);
        }
    }
    if (cc__is_parser_vec_recv_type(ctx->recv_type_name)) {
        if (strcmp(method, "push") == 0 || strcmp(method, "get") == 0 ||
            strcmp(method, "get_ptr") == 0 || strcmp(method, "pop") == 0 ||
            strcmp(method, "at_grow") == 0 || strcmp(method, "push_ptr") == 0 ||
            strcmp(method, "reserve") == 0 || strcmp(method, "clear") == 0 ||
            strcmp(method, "len") == 0 || strcmp(method, "cap") == 0 ||
            strcmp(method, "begin") == 0 || strcmp(method, "end") == 0 ||
            strcmp(method, "data") == 0) {
            if (has_args) {
                if (cc__recv_pass_direct(ctx, recv_is_ptr))
                    return snprintf(out, cap, "__cc_vec_generic_%s(%s, ", method, recv);
                return snprintf(out, cap, "__cc_vec_generic_%s(&%s, ", method, recv);
            }
            if (cc__recv_pass_direct(ctx, recv_is_ptr))
                return snprintf(out, cap, "__cc_vec_generic_%s(%s)", method, recv);
            return snprintf(out, cap, "__cc_vec_generic_%s(&%s)", method, recv);
        }
    }
    if (cc__is_parser_map_recv_type(ctx->recv_type_name)) {
        if (strcmp(method, "insert") == 0 || strcmp(method, "put") == 0 ||
            strcmp(method, "get") == 0 || strcmp(method, "get_ptr") == 0 ||
            strcmp(method, "remove") == 0 || strcmp(method, "del") == 0 ||
            strcmp(method, "clear") == 0 || strcmp(method, "destroy") == 0 ||
            strcmp(method, "len") == 0) {
            if (has_args) {
                if (cc__recv_pass_direct(ctx, recv_is_ptr))
                    return snprintf(out, cap, "__cc_map_generic_%s(%s, ", method, recv);
                return snprintf(out, cap, "__cc_map_generic_%s(&%s, ", method, recv);
            }
            if (cc__recv_pass_direct(ctx, recv_is_ptr))
                return snprintf(out, cap, "__cc_map_generic_%s(%s)", method, recv);
            return snprintf(out, cap, "__cc_map_generic_%s(&%s)", method, recv);
        }
    }
    if (cc__is_family_recv_type(ctx->recv_type_name)) {
        int by_value = (strncmp(ctx->recv_type_name, "Map_", 4) == 0 ||
                        strncmp(ctx->recv_type_name, "CCResult_", 9) == 0 ||
                        strncmp(ctx->recv_type_name, "CCOptional_", 11) == 0);
        const char* family_method = method;
        if (strncmp(ctx->recv_type_name, "CCResult_", 9) == 0) {
            if (!has_args &&
                (strcmp(method, "error") == 0 || strcmp(method, "unwrap_err") == 0)) {
                const char* err_type = cc__result_err_type_suffix(ctx->recv_type_name);
                if (err_type) {
                    return snprintf(out, cap, "cc_unwrap_err_as(%s, %s)", recv, err_type);
                }
            }
            if (strcmp(method, "value") == 0) family_method = "unwrap";
            else if (strcmp(method, "error") == 0) family_method = "unwrap_err";
        }
        if (has_args) {
            if (cc__recv_pass_direct(ctx, recv_is_ptr) || by_value)
                return snprintf(out, cap, "%s_%s(%s, ", ctx->recv_type_name, family_method, recv);
            return snprintf(out, cap, "%s_%s(&%s, ", ctx->recv_type_name, family_method, recv);
        }
        if (cc__recv_pass_direct(ctx, recv_is_ptr) || by_value)
            return snprintf(out, cap, "%s_%s(%s)", ctx->recv_type_name, family_method, recv);
        return snprintf(out, cap, "%s_%s(&%s)", ctx->recv_type_name, family_method, recv);
    }
    if (ctx->recv_type_name &&
        (strcmp(ctx->recv_type_name, "CCArena") == 0 || strcmp(ctx->recv_type_name, "CCArena*") == 0)) {
        int arena_recv_is_ptr = recv_is_ptr || strcmp(ctx->recv_type_name, "CCArena*") == 0;
        if (strcmp(method, "free") == 0) {
            return arena_recv_is_ptr ? snprintf(out, cap, "cc_arena_free(%s)", recv)
                                     : snprintf(out, cap, "cc_arena_free(&%s)", recv);
        }
        if (strcmp(method, "detach") == 0) {
            return arena_recv_is_ptr ? snprintf(out, cap, "cc_arena_detach(%s)", recv)
                                     : snprintf(out, cap, "cc_arena_detach(&%s)", recv);
        }
        if (strcmp(method, "reset") == 0) {
            return arena_recv_is_ptr ? snprintf(out, cap, "cc_arena_reset(%s)", recv)
                                     : snprintf(out, cap, "cc_arena_reset(&%s)", recv);
        }
        if (strcmp(method, "remaining") == 0) {
            return arena_recv_is_ptr ? snprintf(out, cap, "cc_arena_remaining(%s)", recv)
                                     : snprintf(out, cap, "cc_arena_remaining(&%s)", recv);
        }
        if (strcmp(method, "checkpoint") == 0) {
            return arena_recv_is_ptr ? snprintf(out, cap, "cc_arena_checkpoint(%s)", recv)
                                     : snprintf(out, cap, "cc_arena_checkpoint(&%s)", recv);
        }
    }
    if (ctx->recv_type_name && ctx->recv_type_name[0] &&
        !cc__is_string_recv_type(ctx->recv_type_name) &&
        !cc__is_slice_recv_type(ctx->recv_type_name) &&
        strcmp(ctx->recv_type_name, "CCChanTx") != 0 &&
        strcmp(ctx->recv_type_name, "CCChanRx") != 0) {
        if (has_args) {
            if (cc__recv_pass_direct(ctx, recv_is_ptr))
                return snprintf(out, cap, "%s_%s(%s, ", ctx->recv_type_name, method, recv);
            return snprintf(out, cap, "%s_%s(&%s, ", ctx->recv_type_name, method, recv);
        }
        if (cc__recv_pass_direct(ctx, recv_is_ptr))
            return snprintf(out, cap, "%s_%s(%s)", ctx->recv_type_name, method, recv);
        return snprintf(out, cap, "%s_%s(&%s)", ctx->recv_type_name, method, recv);
    }
    return -1;
}

static int emit_desugared_call(char* out,
                               size_t cap,
                               const char* recv,
                               const char* method,
                               bool recv_is_ptr,
                               const char* args_rewritten,
                               bool has_args) {
    CCUFCSDispatchCtx ctx;
    int dispatch_n;
    if (!out || cap == 0 || !recv || !method) return -1;
    cc__resolve_dispatch_ctx(&ctx, recv);
    if ((strcmp(recv, "std_out") == 0 || strcmp(recv, "cc_std_out") == 0 ||
         strcmp(recv, "std_err") == 0 || strcmp(recv, "cc_std_err") == 0) &&
        strcmp(method, "write") == 0) {
        if ((strcmp(recv, "std_out") == 0 || strcmp(recv, "cc_std_out") == 0)) {
            if (!has_args || !args_rewritten) return snprintf(out, cap, "cc_std_out_write(");

            char tmp[512];
            strncpy(tmp, args_rewritten, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = '\0';
            trim_ws_in_place(tmp);

            if (tmp[0] == '"') {
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
        if (!has_args || !args_rewritten) return snprintf(out, cap, "cc_std_err_write(");
        {
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
    }
    dispatch_n = cc__emit_type_driven_dispatch(out, cap, recv, method, recv_is_ptr, args_rewritten, has_args, &ctx);
    if (dispatch_n >= 0) return dispatch_n;

    /* Channel UFCS:
       - Sync path: lower to the canonical CC_TYPED_CHAN_* helper layer.
       - Await path: lower directly to cc_channel_*_task so async storage stays in
         the caller's frame rather than a wrapper-local temporary.
       - Avoids collisions with libc symbols like close/free. */
    if (strcmp(method, "send") == 0) {
        if (g_ufcs_await_context) {
            /* Emit cc_channel_send_task(recv.raw, &val, sizeof(val)) */
            if (!has_args || !args_rewritten) {
                return snprintf(out, cap, "cc_channel_send_task((%s%s).raw, NULL, 0)",
                               recv_is_ptr ? "*" : "", recv);
            }
            return snprintf(out, cap,
                           "cc_channel_send_task((%s%s).raw, &(%s), sizeof(%s))",
                           recv_is_ptr ? "*" : "", recv, args_rewritten, args_rewritten);
        }
        if (!has_args || !args_rewritten) {
            return snprintf(out, cap, "CC_TYPED_CHAN_SEND((%s%s), 0)", recv_is_ptr ? "*" : "", recv);
        }
        return snprintf(out, cap, "CC_TYPED_CHAN_SEND((%s%s), %s)", recv_is_ptr ? "*" : "", recv, args_rewritten);
    }
    if (strcmp(method, "recv") == 0) {
        if (g_ufcs_await_context) {
            /* Emit cc_channel_recv_task(recv.raw, ptr, sizeof(*ptr)) */
            if (!has_args || !args_rewritten) {
                return snprintf(out, cap, "cc_channel_recv_task((%s%s).raw, NULL, 0)",
                               recv_is_ptr ? "*" : "", recv);
            }
            return snprintf(out, cap,
                           "cc_channel_recv_task((%s%s).raw, %s, sizeof(*(%s)))",
                           recv_is_ptr ? "*" : "", recv, args_rewritten, args_rewritten);
        }
        if (!has_args || !args_rewritten) {
            return snprintf(out, cap, "CC_TYPED_CHAN_RECV((%s%s), NULL)", recv_is_ptr ? "*" : "", recv);
        }
        return snprintf(out, cap, "CC_TYPED_CHAN_RECV((%s%s), %s)", recv_is_ptr ? "*" : "", recv, args_rewritten);
    }
    if (strcmp(method, "send_take") == 0) {
        if (!has_args || !args_rewritten) return snprintf(out, cap, "chan_send_take(%s%s)", recv_is_ptr ? "*":"", recv);
        return snprintf(out, cap, "chan_send_take(%s%s, %s)", recv_is_ptr ? "*":"", recv, args_rewritten);
    }
    if (strcmp(method, "try_send") == 0) {
        if (!has_args || !args_rewritten) {
            return snprintf(out, cap, "CC_TYPED_CHAN_TRY_SEND((%s%s), 0)", recv_is_ptr ? "*" : "", recv);
        }
        return snprintf(out, cap, "CC_TYPED_CHAN_TRY_SEND((%s%s), %s)", recv_is_ptr ? "*" : "", recv, args_rewritten);
    }
    if (strcmp(method, "try_recv") == 0) {
        if (!has_args || !args_rewritten) {
            return snprintf(out, cap, "CC_TYPED_CHAN_TRY_RECV((%s%s), NULL)", recv_is_ptr ? "*" : "", recv);
        }
        return snprintf(out, cap, "CC_TYPED_CHAN_TRY_RECV((%s%s), %s)", recv_is_ptr ? "*" : "", recv, args_rewritten);
    }
    if (strcmp(method, "close") == 0) {
        return snprintf(out, cap, "CC_TYPED_CHAN_CLOSE((%s%s))", recv_is_ptr ? "*" : "", recv);
    }
    /* Special cases for stdlib convenience (String methods).
       
       IMPORTANT: These String-specific handlers run BEFORE the type-qualified
       dispatch below (g_ufcs_recv_type check at ~line 305). This means "push"
       on a String will be handled here, not via Type_push().
       
       Raw UFCS now survives parsing through the patched TCC tolerance path, and
       final emitted C comes from this file's AST-aware lowering. Keep
       String-specific dispatch narrow so it does not steal other families'
       methods. */
    if ((cc__is_string_recv_type(ctx.recv_type_name) || !ctx.recv_type_name) && strcmp(method, "as_slice") == 0) {
        return recv_is_ptr ? snprintf(out, cap, "cc_string_as_slice(%s)", recv)
                           : snprintf(out, cap, "cc_string_as_slice(&%s)", recv);
    }
    if ((cc__is_string_recv_type(ctx.recv_type_name) || !ctx.recv_type_name) &&
        (strcmp(method, "append") == 0 || strcmp(method, "push") == 0)) {
        if (!has_args || !args_rewritten) {
            return recv_is_ptr ? snprintf(out, cap, "cc_string_push(%s, cc_slice_empty())", recv)
                               : snprintf(out, cap, "cc_string_push(&%s, cc_slice_empty())", recv);
        }
        char tmp[512];
        strncpy(tmp, args_rewritten, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        trim_ws_in_place(tmp);
        if (tmp[0] == '"') {
            return recv_is_ptr
                       ? snprintf(out, cap,
                                  "cc_string_push(%s, cc_slice_from_buffer(%s, sizeof(%s) - 1))",
                                  recv, tmp, tmp)
                       : snprintf(out, cap,
                                  "cc_string_push(&%s, cc_slice_from_buffer(%s, sizeof(%s) - 1))",
                                  recv, tmp, tmp);
        }
        return recv_is_ptr ? snprintf(out, cap, "cc_string_push(%s, %s)", recv, tmp)
                           : snprintf(out, cap, "cc_string_push(&%s, %s)", recv, tmp);
    }
    if ((cc__is_string_recv_type(ctx.recv_type_name) || !ctx.recv_type_name) && strcmp(method, "push_char") == 0) {
        return recv_is_ptr ? snprintf(out, cap, "cc_string_push_char(%s, ", recv)
                           : snprintf(out, cap, "cc_string_push_char(&%s, ", recv);
    }
    if ((cc__is_string_recv_type(ctx.recv_type_name) || !ctx.recv_type_name) && strcmp(method, "push_int") == 0) {
        return recv_is_ptr ? snprintf(out, cap, "cc_string_push_int(%s, ", recv)
                           : snprintf(out, cap, "cc_string_push_int(&%s, ", recv);
    }
    if ((cc__is_string_recv_type(ctx.recv_type_name) || !ctx.recv_type_name) && strcmp(method, "push_uint") == 0) {
        return recv_is_ptr ? snprintf(out, cap, "cc_string_push_uint(%s, ", recv)
                           : snprintf(out, cap, "cc_string_push_uint(&%s, ", recv);
    }
    if ((cc__is_string_recv_type(ctx.recv_type_name) || !ctx.recv_type_name) && strcmp(method, "push_float") == 0) {
        return recv_is_ptr ? snprintf(out, cap, "cc_string_push_float(%s, ", recv)
                           : snprintf(out, cap, "cc_string_push_float(&%s, ", recv);
    }
    if ((cc__is_string_recv_type(ctx.recv_type_name) || !ctx.recv_type_name) && strcmp(method, "clear") == 0) {
        return recv_is_ptr ? snprintf(out, cap, "cc_string_clear(%s)", recv)
                           : snprintf(out, cap, "cc_string_clear(&%s)", recv);
    }
    
    /* Slice UFCS methods: s.len(), s.trim(), s.at(i), etc.
       These dispatch to CCSlice_* functions. */
    if ((cc__is_slice_recv_type(ctx.recv_type_name) || !ctx.recv_type_name) && strcmp(method, "len") == 0) {
        return recv_is_ptr ? snprintf(out, cap, "CCSlice_len(%s)", recv)
                           : snprintf(out, cap, "CCSlice_len(&%s)", recv);
    }
    if ((cc__is_slice_recv_type(ctx.recv_type_name) || !ctx.recv_type_name) && strcmp(method, "trim") == 0) {
        return recv_is_ptr ? snprintf(out, cap, "CCSlice_trim(%s)", recv)
                           : snprintf(out, cap, "CCSlice_trim(&%s)", recv);
    }
    if ((cc__is_slice_recv_type(ctx.recv_type_name) || !ctx.recv_type_name) && strcmp(method, "trim_left") == 0) {
        return recv_is_ptr ? snprintf(out, cap, "CCSlice_trim_left(%s)", recv)
                           : snprintf(out, cap, "CCSlice_trim_left(&%s)", recv);
    }
    if ((cc__is_slice_recv_type(ctx.recv_type_name) || !ctx.recv_type_name) && strcmp(method, "trim_right") == 0) {
        return recv_is_ptr ? snprintf(out, cap, "CCSlice_trim_right(%s)", recv)
                           : snprintf(out, cap, "CCSlice_trim_right(&%s)", recv);
    }
    if ((cc__is_slice_recv_type(ctx.recv_type_name) || !ctx.recv_type_name) && strcmp(method, "is_empty") == 0) {
        return recv_is_ptr ? snprintf(out, cap, "CCSlice_is_empty(%s)", recv)
                           : snprintf(out, cap, "CCSlice_is_empty(&%s)", recv);
    }
    if ((cc__is_slice_recv_type(ctx.recv_type_name) || !ctx.recv_type_name) && strcmp(method, "at") == 0) {
        if (!has_args || !args_rewritten) {
            return recv_is_ptr ? snprintf(out, cap, "CCSlice_at(%s, 0)", recv)
                               : snprintf(out, cap, "CCSlice_at(&%s, 0)", recv);
        }
        return recv_is_ptr ? snprintf(out, cap, "CCSlice_at(%s, %s)", recv, args_rewritten)
                           : snprintf(out, cap, "CCSlice_at(&%s, %s)", recv, args_rewritten);
    }
    if ((cc__is_slice_recv_type(ctx.recv_type_name) || !ctx.recv_type_name) && strcmp(method, "sub") == 0) {
        if (!has_args || !args_rewritten) {
            return recv_is_ptr ? snprintf(out, cap, "CCSlice_sub(%s, 0, 0)", recv)
                               : snprintf(out, cap, "CCSlice_sub(&%s, 0, 0)", recv);
        }
        return recv_is_ptr ? snprintf(out, cap, "CCSlice_sub(%s, %s)", recv, args_rewritten)
                           : snprintf(out, cap, "CCSlice_sub(&%s, %s)", recv, args_rewritten);
    }
    if ((cc__is_slice_recv_type(ctx.recv_type_name) || !ctx.recv_type_name) && strcmp(method, "starts_with") == 0) {
        if (!has_args || !args_rewritten) {
            return recv_is_ptr ? snprintf(out, cap, "CCSlice_starts_with(%s, (CCSlice){0})", recv)
                               : snprintf(out, cap, "CCSlice_starts_with(&%s, (CCSlice){0})", recv);
        }
        return recv_is_ptr ? snprintf(out, cap, "CCSlice_starts_with(%s, %s)", recv, args_rewritten)
                           : snprintf(out, cap, "CCSlice_starts_with(&%s, %s)", recv, args_rewritten);
    }
    if ((cc__is_slice_recv_type(ctx.recv_type_name) || !ctx.recv_type_name) && strcmp(method, "ends_with") == 0) {
        if (!has_args || !args_rewritten) {
            return recv_is_ptr ? snprintf(out, cap, "CCSlice_ends_with(%s, (CCSlice){0})", recv)
                               : snprintf(out, cap, "CCSlice_ends_with(&%s, (CCSlice){0})", recv);
        }
        return recv_is_ptr ? snprintf(out, cap, "CCSlice_ends_with(%s, %s)", recv, args_rewritten)
                           : snprintf(out, cap, "CCSlice_ends_with(&%s, %s)", recv, args_rewritten);
    }
    if ((cc__is_slice_recv_type(ctx.recv_type_name) || !ctx.recv_type_name) && strcmp(method, "eq") == 0) {
        if (!has_args || !args_rewritten) {
            return recv_is_ptr ? snprintf(out, cap, "CCSlice_eq(%s, (CCSlice){0})", recv)
                               : snprintf(out, cap, "CCSlice_eq(&%s, (CCSlice){0})", recv);
        }
        return recv_is_ptr ? snprintf(out, cap, "CCSlice_eq(%s, %s)", recv, args_rewritten)
                           : snprintf(out, cap, "CCSlice_eq(&%s, %s)", recv, args_rewritten);
    }
    // Generic UFCS (fallback): method(recv, ...) or method(&recv, ...)
    if (has_args) {
        if (cc__recv_pass_direct(&ctx, recv_is_ptr))
            return snprintf(out, cap, "%s(%s, ", method, recv);
        return snprintf(out, cap, "%s(&%s, ", method, recv);
    }
    if (cc__recv_pass_direct(&ctx, recv_is_ptr))
        return snprintf(out, cap, "%s(%s)", method, recv);
    return snprintf(out, cap, "%s(&%s)", method, recv);
}

static int emit_full_call(char* out,
                          size_t cap,
                          const char* recv,
                          const char* method,
                          bool recv_is_ptr,
                          const char* args_rewritten,
                          bool has_args) {
    char tmp[1024];
    int n = emit_desugared_call(tmp, sizeof(tmp), recv, method, recv_is_ptr, args_rewritten, has_args);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return -1;
    if (n > 0 && tmp[n - 1] == ')') {
        return snprintf(out, cap, "%s", tmp);
    }
    if (has_args && args_rewritten) {
        return snprintf(out, cap, "%s%s)", tmp, args_rewritten);
    }
    return snprintf(out, cap, "%s)", tmp);
}

struct CCUFCSSegment {
    char method[64];
    char args[512];
    bool recv_is_ptr;
};

static int cc__parse_ufcs_chain(const char* in,
                                char* recv,
                                size_t recv_cap,
                                struct CCUFCSSegment* segs,
                                int* seg_count) {
    if (!in || !recv || !segs || !seg_count || recv_cap == 0) return 0;
    *seg_count = 0;

    const char* s = in;
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) return 0;

    int par = 0, br = 0, brc = 0;
    const char* sep = NULL;
    bool sep_is_ptr = false;
    for (const char* p = s; *p; p++) {
        char c = *p;
        if (c == '"' || c == '\'') {
            char q = c;
            p++;
            while (*p) {
                if (*p == '\\' && p[1]) { p++; continue; }
                if (*p == q) break;
                p++;
            }
            continue;
        }
        if (c == '(') { par++; continue; }
        if (c == ')') { if (par > 0) par--; continue; }
        if (c == '[') { br++; continue; }
        if (c == ']') { if (br > 0) br--; continue; }
        if (c == '{') { brc++; continue; }
        if (c == '}') { if (brc > 0) brc--; continue; }
        if (par || br || brc) continue;
        if (c == '.') { sep = p; sep_is_ptr = false; break; }
        if (c == '-' && p[1] == '>') { sep = p; sep_is_ptr = true; break; }
    }
    if (!sep) return 0;

    const char* r_end = sep;
    while (r_end > s && isspace((unsigned char)r_end[-1])) r_end--;
    size_t recv_len = (size_t)(r_end - s);
    if (recv_len == 0 || recv_len >= recv_cap) return 0;
    memcpy(recv, s, recv_len);
    recv[recv_len] = '\0';
    trim_ws_in_place(recv);

    const char* p = sep + (sep_is_ptr ? 2 : 1);
    for (;;) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!is_ident_char(*p)) return 0;
        const char* m_start = p;
        while (is_ident_char(*p)) p++;
        size_t m_len = (size_t)(p - m_start);
        if (m_len == 0 || m_len >= sizeof(segs[0].method)) return 0;

        while (*p && isspace((unsigned char)*p)) p++;
        if (*p != '(') return 0;

        const char* args_start = p + 1;
        int depth = 1;
        p++;
        while (*p && depth > 0) {
            if (*p == '(') depth++;
            else if (*p == ')') depth--;
            else if (*p == '"' || *p == '\'') {
                char q = *p++;
                while (*p) {
                    if (*p == '\\' && p[1]) { p += 2; continue; }
                    if (*p == q) { p++; break; }
                    p++;
                }
                continue;
            }
            if (depth > 0) p++;
        }
        if (depth != 0) return 0;
        const char* args_end = p - 1;

        if (*seg_count >= 8) return 0;
        struct CCUFCSSegment* seg = &segs[(*seg_count)++];
        memcpy(seg->method, m_start, m_len);
        seg->method[m_len] = '\0';
        seg->recv_is_ptr = sep_is_ptr;

        size_t args_len = (size_t)(args_end - args_start);
        if (args_len >= sizeof(seg->args)) args_len = sizeof(seg->args) - 1;
        memcpy(seg->args, args_start, args_len);
        seg->args[args_len] = '\0';

        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '.' || (*p == '-' && p[1] == '>')) {
            sep_is_ptr = (*p == '-');
            p += sep_is_ptr ? 2 : 1;
            continue;
        }
        break;
    }

    while (*p && isspace((unsigned char)*p)) p++;
    return *p == '\0';
}

static int cc__rewrite_ufcs_chain(const char* in, char* out, size_t out_cap) {
    char recv[256];
    struct CCUFCSSegment segs[8];
    int seg_count = 0;
    if (!cc__parse_ufcs_chain(in, recv, sizeof(recv), segs, &seg_count)) return 1;
    if (seg_count <= 0) return 1;

    char rewritten_args[512];
    const char* recv_expr = recv;
    const int recv_needs_tmp = (!segs[0].recv_is_ptr && !is_ident_only(recv) && !is_addr_of_ident(recv));
    const int needs_temps = (seg_count > 1) || recv_needs_tmp;

    if (!needs_temps) {
        if (cc__ufcs_rewrite_line_simple(segs[0].args, rewritten_args, sizeof(rewritten_args)) != 0) {
            strncpy(rewritten_args, segs[0].args, sizeof(rewritten_args) - 1);
            rewritten_args[sizeof(rewritten_args) - 1] = '\0';
        }
        size_t args_len = strnlen(rewritten_args, sizeof(rewritten_args));
        bool has_args = args_len > 0;
        int n = emit_full_call(out, out_cap, recv_expr, segs[0].method, segs[0].recv_is_ptr,
                               rewritten_args, has_args);
        return (n < 0 || (size_t)n >= out_cap) ? -1 : 0;
    }

    char* o = out;
    size_t cap = out_cap;
    int n = snprintf(o, cap, "({ ");
    if (n < 0 || (size_t)n >= cap) return -1;
    o += n; cap -= (size_t)n;
    if (recv_needs_tmp) {
        n = snprintf(o, cap, "__typeof__(%s) __cc_ufcs_recv = %s; ", recv, recv);
        if (n < 0 || (size_t)n >= cap) return -1;
        o += n; cap -= (size_t)n;
        recv_expr = "__cc_ufcs_recv";
    }

    for (int i = 0; i < seg_count; i++) {
        if (cc__ufcs_rewrite_line_simple(segs[i].args, rewritten_args, sizeof(rewritten_args)) != 0) {
            strncpy(rewritten_args, segs[i].args, sizeof(rewritten_args) - 1);
            rewritten_args[sizeof(rewritten_args) - 1] = '\0';
        }
        size_t args_len = strnlen(rewritten_args, sizeof(rewritten_args));
        bool has_args = args_len > 0;

        char call[1024];
        const char* recv_for_call = recv_expr;
        char tmp_name[32];
        if (i > 0) {
            snprintf(tmp_name, sizeof(tmp_name), "__cc_ufcs_tmp%d", i);
            recv_for_call = tmp_name;
        }
        int cn = emit_full_call(call, sizeof(call), recv_for_call, segs[i].method,
                                segs[i].recv_is_ptr, rewritten_args, has_args);
        if (cn < 0 || (size_t)cn >= sizeof(call)) return -1;

        if (i < seg_count - 1) {
            n = snprintf(o, cap, "__typeof__(%s) __cc_ufcs_tmp%d = %s; ", call, i + 1, call);
        } else {
            n = snprintf(o, cap, "%s; ", call);
        }
        if (n < 0 || (size_t)n >= cap) return -1;
        o += n; cap -= (size_t)n;
    }

    n = snprintf(o, cap, "})");
    if (n < 0 || (size_t)n >= cap) return -1;
    o += n; cap -= (size_t)n;
    return 0;
}

static const char* cc__recv_chain_start(const char* line_start, const char* recv_end) {
    const char* seg_start = recv_end + 1;
    while (seg_start > line_start && is_ident_char(*(seg_start - 1))) seg_start--;
    if (seg_start > recv_end || !is_ident_start(*seg_start)) return NULL;
    for (;;) {
        const char* q = seg_start;
        while (q > line_start && isspace((unsigned char)q[-1])) q--;
        if (q > line_start && q[-1] == '.') {
            q--;
        } else if (q > line_start + 1 && q[-2] == '-' && q[-1] == '>') {
            q -= 2;
        } else {
            break;
        }
        while (q > line_start && isspace((unsigned char)q[-1])) q--;
        if (q <= line_start || !is_ident_char(q[-1])) return seg_start;
        seg_start = q;
        while (seg_start > line_start && is_ident_char(*(seg_start - 1))) seg_start--;
        if (!is_ident_start(*seg_start)) return NULL;
    }
    return seg_start;
}

static int cc__ufcs_rewrite_line_simple(const char* in, char* out, size_t out_cap) {
    if (!in || !out || out_cap == 0) return -1;
    const char* p = in;
    char* o = out;
    size_t cap = out_cap;

    while (*p && cap > 1) {
        const char* sep = NULL;
        bool recv_is_ptr = false;
        const char* scan = p;
        while (*scan) {
            bool cand_is_ptr = false;
            if (*scan == '.') {
                sep = scan;
                cand_is_ptr = false;
            } else if (scan[0] == '-' && scan[1] == '>') {
                sep = scan;
                cand_is_ptr = true;
            } else {
                scan++;
                continue;
            }

            const char* m_start = sep + (cand_is_ptr ? 2 : 1);
            while (*m_start && isspace((unsigned char)*m_start)) m_start++;
            if (!is_ident_char(*m_start)) {
                scan = sep + (cand_is_ptr ? 2 : 1);
                sep = NULL;
                continue;
            }
            const char* m_end = m_start;
            while (is_ident_char(*m_end)) m_end++;
            const char* paren = m_end;
            while (*paren && isspace((unsigned char)*paren)) paren++;
            if (*paren == '(') {
                recv_is_ptr = cand_is_ptr;
                break;
            }
            scan = m_end;
            sep = NULL;
        }
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
        const char* r_start = cc__recv_chain_start(p, r_end);
        if (!r_start) {
            size_t chunk = (size_t)((sep + (recv_is_ptr ? 2 : 1)) - p);
            if (chunk >= cap) chunk = cap - 1;
            memcpy(o, p, chunk); o += chunk; cap -= chunk;
            p = sep + (recv_is_ptr ? 2 : 1);
            continue;
        }
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
            size_t chunk = (size_t)((sep + (recv_is_ptr ? 2 : 1)) - p);
            if (chunk >= cap) chunk = cap - 1;
            memcpy(o, p, chunk); o += chunk; cap -= chunk;
            p = sep + (recv_is_ptr ? 2 : 1);
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
            size_t chunk = (size_t)((sep + (recv_is_ptr ? 2 : 1)) - p);
            if (chunk >= cap) chunk = cap - 1;
            memcpy(o, p, chunk); o += chunk; cap -= chunk;
            p = sep + (recv_is_ptr ? 2 : 1);
            continue;
        }
        args_end--; // points to ')'

        // Emit prefix before receiver
        size_t prefix_len = (size_t)(r_start - p);
        if (prefix_len >= cap) prefix_len = cap - 1;
        memcpy(o, p, prefix_len); o += prefix_len; cap -= prefix_len;

        // Build receiver/method strings
        char recv[256] = {0};
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

// Rewrite one line, handling nested method calls.
int cc_ufcs_rewrite_line(const char* in, char* out, size_t out_cap) {
    if (cc__rewrite_ufcs_chain(in, out, out_cap) == 0) return 0;
    return cc__ufcs_rewrite_line_simple(in, out, out_cap);
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
    g_ufcs_recv_type = NULL;
    int rc = cc_ufcs_rewrite_line(in, out, out_cap);
    g_ufcs_await_context = 0;
    g_ufcs_recv_type_is_ptr = 0;
    return rc;
}

// Full UFCS rewrite with receiver type from TCC.
int cc_ufcs_rewrite_line_full(const char* in, char* out, size_t out_cap, 
                              int is_await, int recv_type_is_ptr, const char* recv_type) {
    g_ufcs_await_context = is_await;
    g_ufcs_recv_type_is_ptr = recv_type_is_ptr;
    g_ufcs_recv_type = recv_type;
    int rc = cc_ufcs_rewrite_line(in, out, out_cap);
    g_ufcs_await_context = 0;
    g_ufcs_recv_type_is_ptr = 0;
    g_ufcs_recv_type = NULL;
    return rc;
}