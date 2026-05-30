#!/bin/sh
# Generate cc/src/comptime/emit_tpl_prelude.inc.h from cc_emit_tpl_core.inc.cch
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CORE="$ROOT/cc/include/ccc/cc_emit_tpl_core.inc.cch"
OUT="$ROOT/cc/src/comptime/emit_tpl_prelude.inc.h"

if [ ! -f "$CORE" ]; then
  echo "gen_emit_tpl_prelude: missing $CORE" >&2
  exit 1
fi

{
  cat <<'HDR'
#ifndef CC_COMPTIME_EMIT_TPL_PRELUDE_INC_H
#define CC_COMPTIME_EMIT_TPL_PRELUDE_INC_H

/* AUTO-GENERATED from cc/include/ccc/cc_emit_tpl_core.inc.cch — do not edit. */
#define CC_COMPTIME_EMIT_TPL_PRELUDE \
    "#include <stddef.h>\n" \
    "#include <stdio.h>\n" \
    "#include <stdarg.h>\n" \
    "#include <string.h>\n" \
    "#include <stdint.h>\n" \
    "typedef struct { void* ptr; size_t len; uint64_t id; size_t alen; } CCSlice;\n" \
    "#define CC_SLICE_ID_UNTRACKED 0ULL\n" \
    "static inline CCSlice cc_slice_empty(void) { CCSlice s={0}; return s; }\n" \
    "static inline CCSlice cc_slice_from_parts(char* p,size_t n,uint64_t id,size_t cap){\n" \
    "  CCSlice s={p,n,id,cap}; return s; }\n" \
HDR

  while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in
      ''|'/*'*|' *'|'*/'|'*'*)
        continue
        ;;
    esac
    esc=$(printf '%s' "$line" | sed 's/\\/\\\\/g; s/"/\\"/g')
    printf '    "%s\\n" \\\n' "$esc"
  done < "$CORE"

  cat <<'TAIL'
    "typedef enum { CC_EMIT_AFTER_PRELUDE=0, CC_EMIT_BEFORE_FIRST_USE=1," \
    " CC_EMIT_AT_COMPTIME_SITE=2 } CCEmitAnchor;\n" \
    "extern void cc_emit_raw(int anchor, const char* ptr, size_t len);\n" \
    "extern void cc_instantiate_vec(const char* elem);\n" \
    "extern void cc_instantiate_map(const char* key, const char* val);\n" \
    "extern void cc_instantiate_chan(const char* elem);\n" \
    "extern int cc_reflect_field_count(const char* type_name);\n" \
    "extern int cc_reflect_field_name(const char* type_name, int idx, char* buf, int buf_sz);\n" \
    "extern int cc_reflect_field_type(const char* type_name, int idx, char* buf, int buf_sz);\n" \
    "static void cc_emit_tpl_splice(int anchor, CCSlice fragment) {\n" \
    "  if (!fragment.ptr || !fragment.len) return;\n" \
    "  cc_emit_raw(anchor, (const char*)fragment.ptr, fragment.len); }\n" \
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
    "}\n"

#endif /* CC_COMPTIME_EMIT_TPL_PRELUDE_INC_H */
TAIL
} > "$OUT"

echo "gen_emit_tpl_prelude: wrote $OUT"
