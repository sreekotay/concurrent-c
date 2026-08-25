/*
 * Emit-time helpers for py_module / py_expose trampoline generation.
 * Compiled into generic-factory TUs (via hook_compile prelude include).
 *
 * Prefer arity-specialized enter (shared state+bind) + specialized CC_PY_IN
 * in the stub.  Defaults or arity > 8 → fully inline legacy trampoline.
 */
#ifndef CC_PY_EXPORT_EMIT_CCH
#define CC_PY_EXPORT_EMIT_CCH

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CC__PY_DISP_MAX 8

/* Emit forward decl + trampoline for one reflected method.
 * `prefix` is "x" (py_expose) or "m" (py_module). */
static int cc__py_emit_method_tramp(CCString *s, CCArena arena, const char *T,
                                    const char *nm, const char *mb,
                                    const char *pr, const char *pr_abi,
                                    const char *ar, const char *rt,
                                    const char *er, const char *box,
                                    const char *prefix) {
    char p0t[192], p0n[128];
    char buf[24576];
    char names_lit[1024], opt_lit[512];
    char recv_init[256], scratch_line[64];
    int pn, argc, p0ptr, needs_ts = 0, has_default = 0, can_enter = 1;
    int k, off, ooff, nwr, seen_default = 0;
    pn = cc_reflect_param_count(pr);
    if (pn < 1) return -1;
    argc = pn - 1;
    cc_reflect_param_type(pr, 0, p0t, sizeof(p0t));
    cc_reflect_param_name(pr, 0, p0n, sizeof(p0n));
    p0ptr = p0t[0] && p0t[strlen(p0t) - 1] == '*';
    if (argc > CC__PY_DISP_MAX) can_enter = 0;
    names_lit[0] = opt_lit[0] = 0;
    off = ooff = 0;
    for (k = 1; k < pn; k++) {
        char pty[192], pnm[128], def[128];
        int dlen;
        cc_reflect_param_type(pr, k, pty, sizeof(pty));
        cc_reflect_param_name(pr, k, pnm, sizeof(pnm));
        dlen = cc_reflect_param_default(pr, k, def, sizeof(def));
        if (dlen < 0) {
            cc_emit_error("py export: malformed parameter default");
            return -1;
        }
        if (dlen > 0) {
            has_default = 1;
            seen_default = 1;
        } else if (seen_default) {
            cc_emit_error("py export: a required parameter follows one with a "
                          "default — spell defaults on every trailing parameter");
            return -1;
        }
        if (strncmp(pty, "CCSlice_", 8) == 0) needs_ts = 1;
        if (k > 1) {
            if (off + 2 < (int)sizeof(names_lit)) {
                names_lit[off++] = ','; names_lit[off++] = ' '; names_lit[off] = 0;
            }
            if (ooff + 2 < (int)sizeof(opt_lit)) {
                opt_lit[ooff++] = ','; opt_lit[ooff++] = ' '; opt_lit[ooff] = 0;
            }
        }
        nwr = snprintf(names_lit + off, sizeof(names_lit) - (size_t)off, "\"%s\"", pnm);
        if (nwr > 0) off += nwr;
        nwr = snprintf(opt_lit + ooff, sizeof(opt_lit) - (size_t)ooff, "%u",
                       dlen > 0 ? 1u : 0u);
        if (nwr > 0) ooff += nwr;
    }
    if (has_default) can_enter = 0;

    if (er && er[0] && box && box[0]) {
        nwr = snprintf(buf, sizeof(buf),
                       "\n#ifndef %s_DEFINED\n#define %s_DEFINED 1\n"
                       "CC_DECL_RESULT_SPEC(%s, %s, %s)\n#endif\n",
                       box, box, box, rt, er);
        if (nwr > 0) cc_string_push_buffer(s, buf, (uint32_t)nwr, arena);
    }
    nwr = snprintf(buf, sizeof(buf), "\n            static %s %s%s;\n",
                   (er && er[0] && box && box[0]) ? box : rt, nm, pr_abi);
    if (nwr > 0) cc_string_push_buffer(s, buf, (uint32_t)nwr, arena);

    if (can_enter) {
        if (p0ptr)
            snprintf(recv_init, sizeof(recv_init), "%s %s = (%s)self__;", p0t, p0n, p0t);
        else
            snprintf(recv_init, sizeof(recv_init), "%s %s = *(%s *)self__;", p0t, p0n, T);
        if (argc == 0) {
            nwr = snprintf(buf, sizeof(buf),
                "            static void *cc__py_%s_%s(void *mod__, void *const *args__,\n"
                "                                        intptr_t nargs__, void *kwnames__) {\n"
                "                void *self__;\n"
                "                if (cc__py_enter_0(mod__, args__, nargs__, kwnames__,\n"
                "                                   \"%s\", NULL, NULL, %u, &self__, NULL) != 0)\n"
                "                    return NULL;\n"
                "                %s\n",
                prefix, nm, mb, needs_ts ? 1u : 0u, recv_init);
        } else {
            nwr = snprintf(buf, sizeof(buf),
                "            static void *cc__py_%s_%s(void *mod__, void *const *args__,\n"
                "                                        intptr_t nargs__, void *kwnames__) {\n"
                "                static const char *const names__[] = { %s };\n"
                "                void *self__;\n"
                "                void *bound__[%d];\n"
                "                if (cc__py_enter_%d(mod__, args__, nargs__, kwnames__,\n"
                "                                   \"%s\", names__, NULL, %u, &self__, bound__) != 0)\n"
                "                    return NULL;\n"
                "                %s\n",
                prefix, nm, names_lit, argc, argc, mb, needs_ts ? 1u : 0u, recv_init);
        }
        if (nwr > 0) cc_string_push_buffer(s, buf, (uint32_t)nwr, arena);
        for (k = 1; k < pn; k++) {
            char pty[192], pnm[128];
            cc_reflect_param_type(pr, k, pty, sizeof(pty));
            cc_reflect_param_name(pr, k, pnm, sizeof(pnm));
            nwr = snprintf(buf, sizeof(buf),
                "                %s %s;\n"
                "                if (CC_PY_IN(%s, bound__[%d]) != 0) return NULL;\n",
                pty, pnm, pnm, k - 1);
            if (nwr > 0) cc_string_push_buffer(s, buf, (uint32_t)nwr, arena);
        }
        if (er && er[0]) {
            if (rt[0] == 'v' && rt[1] == 'o')
                nwr = snprintf(buf, sizeof(buf),
                    "                { %s r__ = %s%s;\n"
                    "                  if (!cc_is_ok(r__))\n"
                    "                      return cc__py_raise_cc_error((int)cc_error(r__).kind,\n"
                    "                                                   cc_error(r__).message);\n"
                    "                  return cc__py_none(); } }\n",
                    box, nm, ar);
            else
                nwr = snprintf(buf, sizeof(buf),
                    "                { %s r__ = %s%s;\n"
                    "                  if (!cc_is_ok(r__))\n"
                    "                      return cc__py_raise_cc_error((int)cc_error(r__).kind,\n"
                    "                                                   cc_error(r__).message);\n"
                    "                  { %s v__ = cc_value(r__); return CC_PY_OUT(v__); } } }\n",
                    box, nm, ar, rt);
        } else if (rt[0] == 'v' && rt[1] == 'o') {
            nwr = snprintf(buf, sizeof(buf),
                "                %s%s;\n"
                "                return cc__py_none(); }\n",
                nm, ar);
        } else {
            nwr = snprintf(buf, sizeof(buf),
                "                { %s v__ = %s%s; return CC_PY_OUT(v__); } }\n",
                rt, nm, ar);
        }
        if (nwr > 0) cc_string_push_buffer(s, buf, (uint32_t)nwr, arena);
        return 0;
    }

    /* ---- legacy inline trampoline (defaults / arity > max) ---- */
    if (needs_ts)
        snprintf(scratch_line, sizeof(scratch_line),
                 "\n                cc__py_ts_scratch_begin();");
    else
        scratch_line[0] = 0;
    if (p0ptr)
        snprintf(recv_init, sizeof(recv_init), "%s %s = cc__st__;", p0t, p0n);
    else
        snprintf(recv_init, sizeof(recv_init), "%s %s = *cc__st__;", p0t, p0n);
    nwr = snprintf(buf, sizeof(buf),
        "            static void *cc__py_%s_%s(void *mod__, void *const *args__,\n"
        "                                        intptr_t nargs__, void *kwnames__) {\n"
        "                %s *cc__st__ = (%s *)cc__py_callable_state(mod__);\n"
        "                if (!cc__st__) return cc__py_raise(\"cc: module state unavailable\");\n"
        "                %s%s\n",
        prefix, nm, T, T, recv_init, scratch_line);
    if (nwr > 0) cc_string_push_buffer(s, buf, (uint32_t)nwr, arena);
    if (argc == 0) {
        nwr = snprintf(buf, sizeof(buf),
            "                if (cc__py_bind_fast(args__, nargs__, kwnames__, 0, NULL, NULL, NULL,\n"
            "                                     \"%s\") != 0) return NULL;\n",
            mb);
        if (nwr > 0) cc_string_push_buffer(s, buf, (uint32_t)nwr, arena);
    } else {
        nwr = snprintf(buf, sizeof(buf),
            "                static const char *const names__[] = { %s };\n"
            "                static const unsigned char optional__[] = { %s };\n"
            "                void *bound__[%d];\n"
            "                if (cc__py_bind_fast(args__, nargs__, kwnames__, %d, names__,\n"
            "                                     optional__, bound__, \"%s\") != 0) return NULL;\n",
            names_lit, opt_lit, argc, argc, mb);
        if (nwr > 0) cc_string_push_buffer(s, buf, (uint32_t)nwr, arena);
        for (k = 1; k < pn; k++) {
            char pty[192], pnm[128], def[128];
            int dlen;
            cc_reflect_param_type(pr, k, pty, sizeof(pty));
            cc_reflect_param_name(pr, k, pnm, sizeof(pnm));
            dlen = cc_reflect_param_default(pr, k, def, sizeof(def));
            if (dlen > 0)
                nwr = snprintf(buf, sizeof(buf),
                    "                %s %s;\n"
                    "                if (!bound__[%d]) %s = %s;\n"
                    "                else if (CC_PY_IN(%s, bound__[%d]) != 0) return NULL;\n",
                    pty, pnm, k - 1, pnm, def, pnm, k - 1);
            else
                nwr = snprintf(buf, sizeof(buf),
                    "                %s %s;\n"
                    "                if (CC_PY_IN(%s, bound__[%d]) != 0) return NULL;\n",
                    pty, pnm, pnm, k - 1);
            if (nwr > 0) cc_string_push_buffer(s, buf, (uint32_t)nwr, arena);
        }
    }
    if (er && er[0]) {
        if (rt[0] == 'v' && rt[1] == 'o')
            nwr = snprintf(buf, sizeof(buf),
                "                { %s r__ = %s%s;\n"
                "                  if (!cc_is_ok(r__))\n"
                "                      return cc__py_raise_cc_error((int)cc_error(r__).kind,\n"
                "                                                   cc_error(r__).message);\n"
                "                  return cc__py_none(); } }\n",
                box, nm, ar);
        else
            nwr = snprintf(buf, sizeof(buf),
                "                { %s r__ = %s%s;\n"
                "                  if (!cc_is_ok(r__))\n"
                "                      return cc__py_raise_cc_error((int)cc_error(r__).kind,\n"
                "                                                   cc_error(r__).message);\n"
                "                  { %s v__ = cc_value(r__); return CC_PY_OUT(v__); } } }\n",
                box, nm, ar, rt);
    } else if (rt[0] == 'v' && rt[1] == 'o') {
        nwr = snprintf(buf, sizeof(buf),
            "                %s%s;\n"
            "                return cc__py_none(); }\n",
            nm, ar);
    } else {
        nwr = snprintf(buf, sizeof(buf),
            "                { %s v__ = %s%s; return CC_PY_OUT(v__); } }\n",
            rt, nm, ar);
    }
    if (nwr > 0) cc_string_push_buffer(s, buf, (uint32_t)nwr, arena);
    return 0;
}

#define cc__py_emit_method_tramp(s, a, ...) \
    (cc__py_emit_method_tramp)((s), CC__ARENA_HANDLE(a), __VA_ARGS__)

#endif /* CC_PY_EXPORT_EMIT_CCH */
