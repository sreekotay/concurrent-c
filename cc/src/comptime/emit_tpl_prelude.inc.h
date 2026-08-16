#ifndef CC_COMPTIME_EMIT_TPL_PRELUDE_INC_H
#define CC_COMPTIME_EMIT_TPL_PRELUDE_INC_H

/* AUTO-GENERATED from cc/include/ccc/cc_emit_tpl_core.inc.cch — do not edit. */
#define CC_COMPTIME_EMIT_TPL_PRELUDE \
    "#include <stddef.h>\n" \
    "#include <stdio.h>\n" \
    "#include <stdarg.h>\n" \
    "#include <string.h>\n" \
    "#include <stdint.h>\n" \
    "#include <stdlib.h>\n" \
    "#include <stdbool.h>\n" \
    "#include <ctype.h>\n" \
    "#include <ccc/cc_slice.h>\n" \
    "#include <ccc/cc_arena.h>\n" \
    "/* cc_arena.h declares this extern (defined in the compiled runtime). The\n" \
    "   comptime TU is standalone (never linked against the runtime), so define a\n" \
    "   per-TU instance here — provenance ids only need uniqueness within one run. */\n" \
    "cc_atomic_u64 cc_arena_prov_counter = 0;\n" \
    "#define CC_COMPTIME 1\n" \
    "#include <ccc/std/string.h>\n" \
    "#include <ccc/std/vec.h>\n" \
    "#include <ccc/std/hash.h>\n" \
    "#include <ccc/std/map.h>\n" \
    "/* Typed maps are file-scope (CC_MAP_DECL_ARENA -> static-inline defs), so a\n" \
    "   comptime block can't declare its own; pre-declare common key types. */\n" \
    "CC_MAP_DECL_ARENA(int, int, CCMapII, cc_map_hash_i32, cc_map_eq_i32)\n" \
    "CC_MAP_DECL_ARENA(uint64_t, int, CCMapU64I, cc_map_hash_u64, cc_map_eq_u64)\n" \
    "CC_MAP_DECL_ARENA(CCSlice, int, CCMapSI, cc_map_hash_slice, cc_map_eq_slice)\n" \
    "static inline void cc_emit_tpl_append_lit(char *buf, size_t *pos, size_t cap,\n" \
    "                                          const char *lit, size_t lit_len) {\n" \
    "    if (!buf || !pos || !lit || lit_len == 0) return;\n" \
    "    if (*pos >= cap) return;\n" \
    "    if (*pos + lit_len >= cap) {\n" \
    "        *pos = cap;\n" \
    "        return;\n" \
    "    }\n" \
    "    memcpy(buf + *pos, lit, lit_len);\n" \
    "    *pos += lit_len;\n" \
    "}\n" \
    "static inline void cc_emit_tpl_append_cstr(char *buf, size_t *pos, size_t cap,\n" \
    "                                           const char *s) {\n" \
    "    if (!s) return;\n" \
    "    cc_emit_tpl_append_lit(buf, pos, cap, s, strlen(s));\n" \
    "}\n" \
    "static inline void cc_emit_tpl_append_slice(char *buf, size_t *pos, size_t cap,\n" \
    "                                            CCSlice s) {\n" \
    "    if (!s.ptr || s.len == 0) return;\n" \
    "    cc_emit_tpl_append_lit(buf, pos, cap, (const char *)s.ptr, s.len);\n" \
    "}\n" \
    "static inline void cc_emit_tpl_append_int(char *buf, size_t *pos, size_t cap,\n" \
    "                                          long long v) {\n" \
    "    char tmp[32];\n" \
    "    int n = snprintf(tmp, sizeof(tmp), \"%lld\", v);\n" \
    "    if (n > 0) cc_emit_tpl_append_lit(buf, pos, cap, tmp, (size_t)n);\n" \
    "}\n" \
    "static inline void cc_emit_tpl_append_uint(char *buf, size_t *pos, size_t cap,\n" \
    "                                           unsigned long long v) {\n" \
    "    char tmp[32];\n" \
    "    int n = snprintf(tmp, sizeof(tmp), \"%llu\", v);\n" \
    "    if (n > 0) cc_emit_tpl_append_lit(buf, pos, cap, tmp, (size_t)n);\n" \
    "}\n" \
    "static inline void cc_emit_tpl_append_double(char *buf, size_t *pos, size_t cap,\n" \
    "                                             double v) {\n" \
    "    char tmp[64];\n" \
    "    int n = snprintf(tmp, sizeof(tmp), \"%g\", v);\n" \
    "    if (n > 0) cc_emit_tpl_append_lit(buf, pos, cap, tmp, (size_t)n);\n" \
    "}\n" \
    "#define cc_emit_tpl_append_slot(buf, pos, cap, data) _Generic((data), \\\n" \
    "    CCSlice: cc_emit_tpl_append_slice, \\\n" \
    "    char *: cc_emit_tpl_append_cstr, \\\n" \
    "    const char *: cc_emit_tpl_append_cstr, \\\n" \
    "    int: cc_emit_tpl_append_int, \\\n" \
    "    unsigned int: cc_emit_tpl_append_uint, \\\n" \
    "    long: cc_emit_tpl_append_int, \\\n" \
    "    unsigned long: cc_emit_tpl_append_uint, \\\n" \
    "    long long: cc_emit_tpl_append_int, \\\n" \
    "    unsigned long long: cc_emit_tpl_append_uint, \\\n" \
    "    float: cc_emit_tpl_append_double, \\\n" \
    "    double: cc_emit_tpl_append_double, \\\n" \
    "    default: cc_emit_tpl_append_cstr \\\n" \
    ")((buf), (pos), (cap), (data))\n" \
    "static inline CCSlice cc_emit_tpl_finish(char *buf, size_t pos, size_t cap) {\n" \
    "    if (!buf || cap == 0) return cc_slice_empty();\n" \
    "    if (pos >= cap) return cc_slice_empty();\n" \
    "    buf[pos] = '\\0';\n" \
    "    return cc_slice_from_parts(buf, pos, CC_SLICE_ID_UNTRACKED);\n" \
    "}\n" \
    "typedef enum { CC_EMIT_AFTER_PRELUDE=0, CC_EMIT_BEFORE_FIRST_USE=1," \
    " CC_EMIT_AT_COMPTIME_SITE=2 } CCEmitAnchor;\n" \
    "extern void cc_emit_raw(int anchor, const char* ptr, size_t len);\n" \
    "extern void cc_instantiate_vec(const char* elem);\n" \
    "extern void cc_instantiate_map(const char* key, const char* val);\n" \
    "extern void cc_instantiate_chan(const char* elem);\n" \
    "extern int cc_reflect_field_count(const char* type_name);\n" \
    "extern int cc_reflect_field_name(const char* type_name, int idx, char* buf, int buf_sz);\n" \
    "extern int cc_reflect_field_type(const char* type_name, int idx, char* buf, int buf_sz);\n" \
    "extern int cc_reflect_field_is_as(const char* type_name, int idx);\n" \
    "extern int cc_result_box_name(const char* ok_type, const char* err_type, char* buf, int buf_sz);\n" \
    "extern int cc_reflect_method_count(const char* type_name);\n" \
    "extern int cc_reflect_method_name(const char* type_name, int idx, char* buf, int buf_sz);\n" \
    "extern int cc_reflect_param_count(const char* params);\n" \
    "extern int cc_reflect_param_name(const char* params, int idx, char* buf, int buf_sz);\n" \
    "extern int cc_reflect_param_type(const char* params, int idx, char* buf, int buf_sz);\n" \
    "extern int cc_reflect_param_default(const char* params, int idx, char* buf, int buf_sz);\n" \
    "extern int cc_reflect_params_c_abi(const char* params, char* buf, int buf_sz);\n" \
    "extern int cc_reflect_method_member(const char* type_name, int idx, char* buf, int buf_sz);\n" \
    "extern int cc_reflect_method_params(const char* type_name, int idx, char* buf, int buf_sz);\n" \
    "extern int cc_reflect_method_args(const char* type_name, int idx, char* buf, int buf_sz);\n" \
    "extern int cc_reflect_method_ret(const char* type_name, int idx, char* buf, int buf_sz);\n" \
    "extern int cc_reflect_method_err(const char* type_name, int idx, char* buf, int buf_sz);\n" \
    "extern int cc_reflect_enum_count(const char* enum_name);\n" \
    "extern int cc_reflect_enum_name(const char* enum_name, int idx, char* buf, int buf_sz);\n" \
    "extern int cc_reflect_enum_value(const char* enum_name, int idx, long long* out);\n" \
    "extern int cc_reflect_kind(const char* type_name);\n" \
    "extern int cc_canonical_name(const char* base, const char** args, int nargs, char* out, int out_sz);\n" \
    "extern int cc_reflect_tagged_count(const char* tag);\n" \
    "extern int cc_reflect_tagged_name(const char* tag, int idx, char* buf, int buf_sz);\n" \
    "enum { CC_REFLECT_KIND_UNKNOWN=0, CC_REFLECT_KIND_PRIMITIVE=1," \
    " CC_REFLECT_KIND_POINTER=2, CC_REFLECT_KIND_STRUCT=3, CC_REFLECT_KIND_ENUM=4 };\n" \
    "typedef struct CCReflectField { char name[128]; char type[128]; int index; int is_as; } CCReflectField;\n" \
    "static inline int cc_reflect_field_at(const char* type_name, int idx, CCReflectField* out) {\n" \
    "  int as;\n" \
    "  if (!out || idx < 0) return -1;\n" \
    "  out->index = idx; out->is_as = 0;\n" \
    "  if (cc_reflect_field_name(type_name, idx, out->name, (int)sizeof(out->name)) < 0) return -1;\n" \
    "  if (cc_reflect_field_type(type_name, idx, out->type, (int)sizeof(out->type)) < 0) return -1;\n" \
    "  as = cc_reflect_field_is_as(type_name, idx);\n" \
    "  if (as < 0) return -1;\n" \
    "  out->is_as = as ? 1 : 0;\n" \
    "  return 0;\n" \
    "}\n" \
    "typedef struct CCReflectEnumMember { char name[128]; long long value; int index; } CCReflectEnumMember;\n" \
    "static inline int cc_reflect_enum_at(const char* enum_name, int idx, CCReflectEnumMember* out) {\n" \
    "  if (!out || idx < 0) return -1;\n" \
    "  out->index = idx;\n" \
    "  if (cc_reflect_enum_name(enum_name, idx, out->name, (int)sizeof(out->name)) < 0) return -1;\n" \
    "  if (cc_reflect_enum_value(enum_name, idx, &out->value) < 0) return -1;\n" \
    "  return 0;\n" \
    "}\n" \
    "static inline void cc_instantiate_result(const char* ok_mangled, const char* err_mangled) {\n" \
    "  (void)ok_mangled; (void)err_mangled;\n" \
    "}\n" \
    "extern void cc_emit_raw_at(int anchor, const char* file, int line, const char* ptr, size_t len);\n" \
    "extern void cc_emit_error(const char* msg);\n" \
    "extern void cc_emit_warning(const char* msg);\n" \
    "extern void cc_emit_error_at(const char* file, int line, const char* msg);\n" \
    "extern void cc_emit_warning_at(const char* file, int line, const char* msg);\n" \
    "static void cc_emit_tpl_splice(int anchor, CCSlice fragment) {\n" \
    "  if (!fragment.ptr || !fragment.len) return;\n" \
    "  cc_emit_raw(anchor, (const char*)fragment.ptr, fragment.len); }\n" \
    "static void cc_emit_tpl_splice_at(int anchor, const char* file, int line, CCSlice fragment) {\n" \
    "  if (!fragment.ptr || !fragment.len) return;\n" \
    "  cc_emit_raw_at(anchor, file, line, (const char*)fragment.ptr, fragment.len); }\n" \
    "static int cc_emit_cstr(int anchor, const char* cstr) {\n" \
    "  if (!cstr) return 0;\n" \
    "  cc_emit_raw(anchor, cstr, strlen(cstr));\n" \
    "  return 0;\n" \
    "}\n" \
    "static int cc_emit_format(int anchor, const char* fmt, ...) {\n" \
    "  char buf[16384];\n" \
    "  va_list ap;\n" \
    "  va_start(ap, fmt);\n" \
    "  int n = vsnprintf(buf, sizeof(buf), fmt, ap);\n" \
    "  va_end(ap);\n" \
    "  if (n < 0 || (size_t)n >= sizeof(buf)) return -1;\n" \
    "  cc_emit_raw(anchor, buf, (size_t)n);\n" \
    "  return 0;\n" \
    "}\n" \
    "#ifdef CC_COMPTIME_EXEC\n" \
    "/* Factory-body sugar: arg(i) == type_args.items[(i)], bounds-checked.  An\n" \
    "   out-of-range index reports the factory, the index and the arity, then\n" \
    "   yields the empty fragment so the caller's factory-failed diagnostic\n" \
    "   names the instance.  Never exit(): a factory body can run in-process\n" \
    "   under the libtcc executor.  Defined under CC_COMPTIME_EXEC: set by\n" \
    "   compiled-factory TUs, and by block/eval TUs whose registry defs\n" \
    "   carry a factory body along (its arg() calls need the sugar even\n" \
    "   when the block itself never touches it). */\n" \
    "static CCSlice cc__tpl_arg(CCSliceArray ta, long i, CCSlice gname) {\n" \
    "  if (i < 0 || (unsigned long)i >= (unsigned long)ta.len) {\n" \
    "    char m[256];\n" \
    "    snprintf(m, sizeof(m), \"factory '%.*s': arg(%ld) is out of range \"\n" \
    "             \"(%lu type argument%s)\", (int)gname.len,\n" \
    "             (const char*)gname.ptr, i, (unsigned long)ta.len,\n" \
    "             ta.len == 1 ? \"\" : \"s\");\n" \
    "    cc_emit_error(m);\n" \
    "    return cc_slice_empty();\n" \
    "  }\n" \
    "  return ta.items[i];\n" \
    "}\n" \
    "#define arg(i) cc__tpl_arg(type_args, (long)(i), generic_name)\n" \
    "#endif\n"

#endif /* CC_COMPTIME_EMIT_TPL_PRELUDE_INC_H */
