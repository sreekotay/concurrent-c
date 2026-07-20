/* CC-AUTHORED EMITTER SECTIONS — grammar_rules.c peeled into the language
 * itself, one section at a time.  This file is user CC (@string templates,
 * CCString, arenas), lowered at compiler-build time by the SAME shared
 * passes as every user header (lower_headers stage) and compiled into ccc.
 * The lowered .h is CHECKED IN beside this file (bootstrap: lower_headers
 * itself links grammar_rules.c); `make engine-headers` regenerates it.
 *
 * INVARIANT: engine source is user CC.  Any construct valid in a .ccs is
 * valid here; a lowering gap is a BUG in header-lowering convergence, not
 * a boundary to write around.
 *
 * PORT RULE: every section must emit BYTE-IDENTICAL text to the printf
 * emitter it replaces — verified by diffing the generated C of the five
 * reference TUs (json bench, write bench, resp bench, union smoke,
 * shaped-dom smoke) before landing. */
#ifndef CC_GRAMMAR_EMIT_CCH
#define CC_GRAMMAR_EMIT_CCH

/* Compiler-internal consumers (ccc itself, lower_headers) get the
 * header-only bodies — same standalone flavor comptime dylibs use — so
 * no runtime link is required wherever this header lands. */
#ifndef CC_COMPTIME
#define CC_COMPTIME 1
#endif

#include <ccc/cc_arena.h>
#include <ccc/std/string.h>

static inline void cc_gr__cat(CCString* o, CCString piece, CCArena* a) {
    cc_string_push_slice(o, cc_string_as_slice(&piece), a);
}

/* ---- run-scanner: stop set {b < T} u {b1} u {b2} as an arch ladder ----
 * 16 B SSE2 where the host compiler has it, portable 8 B SWAR otherwise
 * (also what the TCC front-end parses).  On a hit every arm advances p to
 * the FIRST stop byte.  NO NEON ARM by measurement: 33% regression on
 * Apple Silicon (vector->GPR lane-extract latency dominates ~11-byte
 * median runs); re-adding needs a stay-in-vector-domain design plus
 * on-hardware interleaved numbers. */
static inline CCString cc_gr_swar_run_text(CCArena* a, int k, int T, int b1, int b2) {
    CCString o = cc_string_new();
    cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_0 = (a); CCString __cc_tpl_0 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_0, "#if defined(__SSE2__)\n    { while (p + 16 <= n) {\n        __m128i w", 67, __cc_tpl_arena_0); cc__string_slot_push(&__cc_tpl_0, (k), __cc_tpl_arena_0); cc_string_push_buffer(&__cc_tpl_0, " = _mm_loadu_si128((const __m128i*)(s + p));\n        __m128i m", 62, __cc_tpl_arena_0); cc__string_slot_push(&__cc_tpl_0, (k), __cc_tpl_arena_0); cc_string_push_buffer(&__cc_tpl_0, " = _mm_setzero_si128();\n", 24, __cc_tpl_arena_0); __cc_tpl_0; }), a);
    if (T > 0)
        cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_1 = (a); CCString __cc_tpl_1 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_1, "        m", 9, __cc_tpl_arena_1); cc__string_slot_push(&__cc_tpl_1, (k), __cc_tpl_arena_1); cc_string_push_buffer(&__cc_tpl_1, " = _mm_cmpeq_epi8(_mm_min_epu8(w", 32, __cc_tpl_arena_1); cc__string_slot_push(&__cc_tpl_1, (k), __cc_tpl_arena_1); cc_string_push_buffer(&__cc_tpl_1, ", _mm_set1_epi8((char)", 22, __cc_tpl_arena_1); cc__string_slot_push(&__cc_tpl_1, (T - 1), __cc_tpl_arena_1); cc_string_push_buffer(&__cc_tpl_1, ")), w", 5, __cc_tpl_arena_1); cc__string_slot_push(&__cc_tpl_1, (k), __cc_tpl_arena_1); cc_string_push_buffer(&__cc_tpl_1, ");\n", 3, __cc_tpl_arena_1); __cc_tpl_1; }), a);
    if (b1 >= 0)
        cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_2 = (a); CCString __cc_tpl_2 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_2, "        m", 9, __cc_tpl_arena_2); cc__string_slot_push(&__cc_tpl_2, (k), __cc_tpl_arena_2); cc_string_push_buffer(&__cc_tpl_2, " = _mm_or_si128(m", 17, __cc_tpl_arena_2); cc__string_slot_push(&__cc_tpl_2, (k), __cc_tpl_arena_2); cc_string_push_buffer(&__cc_tpl_2, ", _mm_cmpeq_epi8(w", 18, __cc_tpl_arena_2); cc__string_slot_push(&__cc_tpl_2, (k), __cc_tpl_arena_2); cc_string_push_buffer(&__cc_tpl_2, ", _mm_set1_epi8((char)", 22, __cc_tpl_arena_2); cc__string_slot_push(&__cc_tpl_2, (b1), __cc_tpl_arena_2); cc_string_push_buffer(&__cc_tpl_2, ")));\n", 5, __cc_tpl_arena_2); __cc_tpl_2; }), a);
    if (b2 >= 0)
        cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_3 = (a); CCString __cc_tpl_3 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_3, "        m", 9, __cc_tpl_arena_3); cc__string_slot_push(&__cc_tpl_3, (k), __cc_tpl_arena_3); cc_string_push_buffer(&__cc_tpl_3, " = _mm_or_si128(m", 17, __cc_tpl_arena_3); cc__string_slot_push(&__cc_tpl_3, (k), __cc_tpl_arena_3); cc_string_push_buffer(&__cc_tpl_3, ", _mm_cmpeq_epi8(w", 18, __cc_tpl_arena_3); cc__string_slot_push(&__cc_tpl_3, (k), __cc_tpl_arena_3); cc_string_push_buffer(&__cc_tpl_3, ", _mm_set1_epi8((char)", 22, __cc_tpl_arena_3); cc__string_slot_push(&__cc_tpl_3, (b2), __cc_tpl_arena_3); cc_string_push_buffer(&__cc_tpl_3, ")));\n", 5, __cc_tpl_arena_3); __cc_tpl_3; }), a);
    cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_4 = (a); CCString __cc_tpl_4 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_4, "        { int hm", 16, __cc_tpl_arena_4); cc__string_slot_push(&__cc_tpl_4, (k), __cc_tpl_arena_4); cc_string_push_buffer(&__cc_tpl_4, " = _mm_movemask_epi8(m", 22, __cc_tpl_arena_4); cc__string_slot_push(&__cc_tpl_4, (k), __cc_tpl_arena_4); cc_string_push_buffer(&__cc_tpl_4, ");\n          if (hm", 19, __cc_tpl_arena_4); cc__string_slot_push(&__cc_tpl_4, (k), __cc_tpl_arena_4); cc_string_push_buffer(&__cc_tpl_4, ") { p += (size_t)__builtin_ctz((unsigned)hm", 43, __cc_tpl_arena_4); cc__string_slot_push(&__cc_tpl_4, (k), __cc_tpl_arena_4); cc_string_push_buffer(&__cc_tpl_4, "); break; } }\n        p += 16;\n    } }\n#else\n    { const unsigned long long L", 77, __cc_tpl_arena_4); cc__string_slot_push(&__cc_tpl_4, (k), __cc_tpl_arena_4); cc_string_push_buffer(&__cc_tpl_4, " = 0x0101010101010101ULL, H", 27, __cc_tpl_arena_4); cc__string_slot_push(&__cc_tpl_4, (k), __cc_tpl_arena_4); cc_string_push_buffer(&__cc_tpl_4, " = 0x8080808080808080ULL;\n    while (p + 8 <= n) { unsigned long long w", 71, __cc_tpl_arena_4); cc__string_slot_push(&__cc_tpl_4, (k), __cc_tpl_arena_4); cc_string_push_buffer(&__cc_tpl_4, "; memcpy(&w", 11, __cc_tpl_arena_4); cc__string_slot_push(&__cc_tpl_4, (k), __cc_tpl_arena_4); cc_string_push_buffer(&__cc_tpl_4, ", s + p, 8);\n        unsigned long long m", 41, __cc_tpl_arena_4); cc__string_slot_push(&__cc_tpl_4, (k), __cc_tpl_arena_4); cc_string_push_buffer(&__cc_tpl_4, " = 0;\n", 6, __cc_tpl_arena_4); __cc_tpl_4; }), a);
    if (T > 0)
        cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_5 = (a); CCString __cc_tpl_5 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_5, "        m", 9, __cc_tpl_arena_5); cc__string_slot_push(&__cc_tpl_5, (k), __cc_tpl_arena_5); cc_string_push_buffer(&__cc_tpl_5, " |= (w", 6, __cc_tpl_arena_5); cc__string_slot_push(&__cc_tpl_5, (k), __cc_tpl_arena_5); cc_string_push_buffer(&__cc_tpl_5, " - L", 4, __cc_tpl_arena_5); cc__string_slot_push(&__cc_tpl_5, (k), __cc_tpl_arena_5); cc_string_push_buffer(&__cc_tpl_5, " * ", 3, __cc_tpl_arena_5); cc__string_slot_push(&__cc_tpl_5, (T), __cc_tpl_arena_5); cc_string_push_buffer(&__cc_tpl_5, ") & ~w", 6, __cc_tpl_arena_5); cc__string_slot_push(&__cc_tpl_5, (k), __cc_tpl_arena_5); cc_string_push_buffer(&__cc_tpl_5, " & H", 4, __cc_tpl_arena_5); cc__string_slot_push(&__cc_tpl_5, (k), __cc_tpl_arena_5); cc_string_push_buffer(&__cc_tpl_5, ";\n", 2, __cc_tpl_arena_5); __cc_tpl_5; }), a);
    if (b1 >= 0)
        cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_6 = (a); CCString __cc_tpl_6 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_6, "        { unsigned long long x = w", 34, __cc_tpl_arena_6); cc__string_slot_push(&__cc_tpl_6, (k), __cc_tpl_arena_6); cc_string_push_buffer(&__cc_tpl_6, " ^ (L", 5, __cc_tpl_arena_6); cc__string_slot_push(&__cc_tpl_6, (k), __cc_tpl_arena_6); cc_string_push_buffer(&__cc_tpl_6, " * ", 3, __cc_tpl_arena_6); cc__string_slot_push(&__cc_tpl_6, (b1), __cc_tpl_arena_6); cc_string_push_buffer(&__cc_tpl_6, "); m", 4, __cc_tpl_arena_6); cc__string_slot_push(&__cc_tpl_6, (k), __cc_tpl_arena_6); cc_string_push_buffer(&__cc_tpl_6, " |= (x - L", 10, __cc_tpl_arena_6); cc__string_slot_push(&__cc_tpl_6, (k), __cc_tpl_arena_6); cc_string_push_buffer(&__cc_tpl_6, ") & ~x & H", 10, __cc_tpl_arena_6); cc__string_slot_push(&__cc_tpl_6, (k), __cc_tpl_arena_6); cc_string_push_buffer(&__cc_tpl_6, "; }\n", 4, __cc_tpl_arena_6); __cc_tpl_6; }), a);
    if (b2 >= 0)
        cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_7 = (a); CCString __cc_tpl_7 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_7, "        { unsigned long long x = w", 34, __cc_tpl_arena_7); cc__string_slot_push(&__cc_tpl_7, (k), __cc_tpl_arena_7); cc_string_push_buffer(&__cc_tpl_7, " ^ (L", 5, __cc_tpl_arena_7); cc__string_slot_push(&__cc_tpl_7, (k), __cc_tpl_arena_7); cc_string_push_buffer(&__cc_tpl_7, " * ", 3, __cc_tpl_arena_7); cc__string_slot_push(&__cc_tpl_7, (b2), __cc_tpl_arena_7); cc_string_push_buffer(&__cc_tpl_7, "); m", 4, __cc_tpl_arena_7); cc__string_slot_push(&__cc_tpl_7, (k), __cc_tpl_arena_7); cc_string_push_buffer(&__cc_tpl_7, " |= (x - L", 10, __cc_tpl_arena_7); cc__string_slot_push(&__cc_tpl_7, (k), __cc_tpl_arena_7); cc_string_push_buffer(&__cc_tpl_7, ") & ~x & H", 10, __cc_tpl_arena_7); cc__string_slot_push(&__cc_tpl_7, (k), __cc_tpl_arena_7); cc_string_push_buffer(&__cc_tpl_7, "; }\n", 4, __cc_tpl_arena_7); __cc_tpl_7; }), a);
    cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_8 = (a); CCString __cc_tpl_8 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_8, "        if (m", 13, __cc_tpl_arena_8); cc__string_slot_push(&__cc_tpl_8, (k), __cc_tpl_arena_8); cc_string_push_buffer(&__cc_tpl_8, ") { p += (size_t)(__builtin_ctzll(m", 35, __cc_tpl_arena_8); cc__string_slot_push(&__cc_tpl_8, (k), __cc_tpl_arena_8); cc_string_push_buffer(&__cc_tpl_8, ") >> 3); break; }\n        p += 8;\n    } }\n#endif\n", 49, __cc_tpl_arena_8); __cc_tpl_8; }), a);
    return o;
}

/* ---- JSON-number fast path, the tail after the int part ----
 * Emitted after the int-shape scan: if the next byte is not . / e / E the
 * frac/exp scaffolding is skipped entirely (the common path for ids and
 * plain integers).  svf/sve rollbacks preserve the grammar's opt-frac /
 * opt-exp semantics ("1.5e" parses as the number "1.5"). */
static inline CCString cc_gr_number_fast_tail_text(CCArena* a, int k) {
    return ({ CCArena* __cc_tpl_arena_9 = (a); CCString __cc_tpl_9 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_9, "    if (p < n && (s[p] == 46 || s[p] == 69 || s[p] == 101)) {\n    if (s[p] == 46) {\n      size_t svf", 100, __cc_tpl_arena_9); cc__string_slot_push(&__cc_tpl_9, (k), __cc_tpl_arena_9); cc_string_push_buffer(&__cc_tpl_9, " = p; p++;\n      if (!(p < n && ((unsigned)(s[p] - 48) <= 9u))) p = svf", 71, __cc_tpl_arena_9); cc__string_slot_push(&__cc_tpl_9, (k), __cc_tpl_arena_9); cc_string_push_buffer(&__cc_tpl_9, ";\n      else { p++; while (p < n && ((unsigned)(s[p] - 48) <= 9u)) p++; }\n    }\n    if (p < n && (s[p] == 69 || s[p] == 101)) {\n      size_t sve", 144, __cc_tpl_arena_9); cc__string_slot_push(&__cc_tpl_9, (k), __cc_tpl_arena_9); cc_string_push_buffer(&__cc_tpl_9, " = p; p++;\n      if (p < n && (s[p] == 43 || s[p] == 45)) p++;\n      if (!(p < n && ((unsigned)(s[p] - 48) <= 9u))) p = sve", 123, __cc_tpl_arena_9); cc__string_slot_push(&__cc_tpl_9, (k), __cc_tpl_arena_9); cc_string_push_buffer(&__cc_tpl_9, ";\n      else { p++; while (p < n && ((unsigned)(s[p] - 48) <= 9u)) p++; }\n    }\n    }\n", 86, __cc_tpl_arena_9); __cc_tpl_9; });
}

/* ---- tape-node API: adjacency and span ARE the tree ----
 * Spans live in meta (bits 10+) for interior nodes; u stays a byte pointer
 * everywhere.  Includes the lazy ffc number accessor (borrow stays the
 * default; ffc runs on use; FFC_IMPL lives in cc/runtime/arena_state.c). */
static inline CCString cc_gr_node_api_text(CCArena* a, const char* n) {
    return ({ CCArena* __cc_tpl_arena_10 = (a); CCString __cc_tpl_10 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_10, "static int ", 11, __cc_tpl_arena_10); cc__string_slot_push(&__cc_tpl_10, (n), __cc_tpl_arena_10); cc_string_push_buffer(&__cc_tpl_10, "Node_id(const ", 14, __cc_tpl_arena_10); cc__string_slot_push(&__cc_tpl_10, (n), __cc_tpl_arena_10); cc_string_push_buffer(&__cc_tpl_10, "Node* nd) { return (int)(nd->meta & 0xFFu); }\nstatic int ", 57, __cc_tpl_arena_10); cc__string_slot_push(&__cc_tpl_10, (n), __cc_tpl_arena_10); cc_string_push_buffer(&__cc_tpl_10, "Node_is_list(const ", 19, __cc_tpl_arena_10); cc__string_slot_push(&__cc_tpl_10, (n), __cc_tpl_arena_10); cc_string_push_buffer(&__cc_tpl_10, "Node* nd) { return (int)((nd->meta >> 8) & 1u); }\nstatic size_t ", 64, __cc_tpl_arena_10); cc__string_slot_push(&__cc_tpl_10, (n), __cc_tpl_arena_10); cc_string_push_buffer(&__cc_tpl_10, "Node_len(const ", 15, __cc_tpl_arena_10); cc__string_slot_push(&__cc_tpl_10, (n), __cc_tpl_arena_10); cc_string_push_buffer(&__cc_tpl_10, "Node* nd) { return (size_t)(nd->meta >> 10); }\nstatic ", 54, __cc_tpl_arena_10); cc__string_slot_push(&__cc_tpl_10, (n), __cc_tpl_arena_10); cc_string_push_buffer(&__cc_tpl_10, "Node* ", 6, __cc_tpl_arena_10); cc__string_slot_push(&__cc_tpl_10, (n), __cc_tpl_arena_10); cc_string_push_buffer(&__cc_tpl_10, "Node_first(", 11, __cc_tpl_arena_10); cc__string_slot_push(&__cc_tpl_10, (n), __cc_tpl_arena_10); cc_string_push_buffer(&__cc_tpl_10, "Node* nd) {\n    return ", 23, __cc_tpl_arena_10); cc__string_slot_push(&__cc_tpl_10, (n), __cc_tpl_arena_10); cc_string_push_buffer(&__cc_tpl_10, "Node_is_list(nd) && (nd->meta >> 10) > 1 ? nd + 1 : 0; }\nstatic ", 64, __cc_tpl_arena_10); cc__string_slot_push(&__cc_tpl_10, (n), __cc_tpl_arena_10); cc_string_push_buffer(&__cc_tpl_10, "Node* ", 6, __cc_tpl_arena_10); cc__string_slot_push(&__cc_tpl_10, (n), __cc_tpl_arena_10); cc_string_push_buffer(&__cc_tpl_10, "Node_next(", 10, __cc_tpl_arena_10); cc__string_slot_push(&__cc_tpl_10, (n), __cc_tpl_arena_10); cc_string_push_buffer(&__cc_tpl_10, "Node* nd, ", 10, __cc_tpl_arena_10); cc__string_slot_push(&__cc_tpl_10, (n), __cc_tpl_arena_10); cc_string_push_buffer(&__cc_tpl_10, "Node* parent) {\n    ", 20, __cc_tpl_arena_10); cc__string_slot_push(&__cc_tpl_10, (n), __cc_tpl_arena_10); cc_string_push_buffer(&__cc_tpl_10, "Node* nx = nd + (", 17, __cc_tpl_arena_10); cc__string_slot_push(&__cc_tpl_10, (n), __cc_tpl_arena_10); cc_string_push_buffer(&__cc_tpl_10, "Node_is_list(nd) ? (size_t)(nd->meta >> 10) : 1);\n    return nx < parent + (size_t)(parent->meta >> 10) ? nx : 0; }\nstatic size_t ", 130, __cc_tpl_arena_10); cc__string_slot_push(&__cc_tpl_10, (n), __cc_tpl_arena_10); cc_string_push_buffer(&__cc_tpl_10, "Node_count(", 11, __cc_tpl_arena_10); cc__string_slot_push(&__cc_tpl_10, (n), __cc_tpl_arena_10); cc_string_push_buffer(&__cc_tpl_10, "Node* nd) {\n    size_t k = 0;\n    for (", 39, __cc_tpl_arena_10); cc__string_slot_push(&__cc_tpl_10, (n), __cc_tpl_arena_10); cc_string_push_buffer(&__cc_tpl_10, "Node* ch = ", 11, __cc_tpl_arena_10); cc__string_slot_push(&__cc_tpl_10, (n), __cc_tpl_arena_10); cc_string_push_buffer(&__cc_tpl_10, "Node_first(nd); ch; ch = ", 25, __cc_tpl_arena_10); cc__string_slot_push(&__cc_tpl_10, (n), __cc_tpl_arena_10); cc_string_push_buffer(&__cc_tpl_10, "Node_next(ch, nd)) k++;\n    return k; }\nstatic void ", 52, __cc_tpl_arena_10); cc__string_slot_push(&__cc_tpl_10, (n), __cc_tpl_arena_10); cc_string_push_buffer(&__cc_tpl_10, "Node_slice(const ", 17, __cc_tpl_arena_10); cc__string_slot_push(&__cc_tpl_10, (n), __cc_tpl_arena_10); cc_string_push_buffer(&__cc_tpl_10, "Node* nd, CCSlice* out) {\n    size_t l = (size_t)(nd->meta >> 10);\n    if ((nd->meta >> 9) & 1u)\n        *out = cc_slice_from_parts((void*)nd->u.bytes, l,\n                   cc_slice_make_id(3ULL, true, false, false), l);\n    else\n        *out = cc_slice_from_buffer((void*)nd->u.bytes, l);\n}\n#include <ccc/vendor/ffc.h>\nstatic inline double ", 342, __cc_tpl_arena_10); cc__string_slot_push(&__cc_tpl_10, (n), __cc_tpl_arena_10); cc_string_push_buffer(&__cc_tpl_10, "Node_as_f64(const ", 18, __cc_tpl_arena_10); cc__string_slot_push(&__cc_tpl_10, (n), __cc_tpl_arena_10); cc_string_push_buffer(&__cc_tpl_10, "Node* nd) {\n    if ((nd->meta >> 8) & 1u) return 0.0;\n    { double v = 0.0; size_t l = (size_t)(nd->meta >> 10);\n      ffc_parse_options o; o.format = FFC_PRESET_JSON; o.decimal_point = '.';\n      ffc_result r = ffc_from_chars_double_options(\n          nd->u.bytes, nd->u.bytes + l, &v, o);\n      return r.outcome == FFC_OUTCOME_OK ? v : 0.0; }\n}\n", 347, __cc_tpl_arena_10); __cc_tpl_10; });
}

/* ---- tape substrate: Node, ctx, tgrow ----
 * The ctx only holds POINTERS to shape types so the substrate stays
 * dependency-free (plain struct forward decls); full definitions are
 * needed only when cc_dom is demanded.  mat_* is the eager keep/decode
 * stash: dirty keeps decode while the span is hot, u.bytes stays the
 * SOURCE anchor so __unwind works; materialize installs without a
 * re-scan.  tgrow doubles the tape (and, when the grammar has codecs,
 * the stash arrays in lockstep). */
static inline CCString cc_gr_tape_substrate_text(CCArena* a, const char* n, int has_codecs) {
    CCString o = cc_string_new();
    cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_11 = (a); CCString __cc_tpl_11 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_11, "typedef struct { unsigned long long meta;\n    union { const char* bytes; } u; } ", 80, __cc_tpl_arena_11); cc__string_slot_push(&__cc_tpl_11, (n), __cc_tpl_arena_11); cc_string_push_buffer(&__cc_tpl_11, "Node;\nstruct CCShapeB; struct CCShapeVal;\ntypedef struct { ", 59, __cc_tpl_arena_11); cc__string_slot_push(&__cc_tpl_11, (n), __cc_tpl_arena_11); cc_string_push_buffer(&__cc_tpl_11, "Node* tape; size_t total, cap;\n    size_t bstack[512]; size_t bdepth; CCArena* arena;\n    const char** mat_ptr; size_t* mat_len; unsigned char* mat_cow;\n    /* shaped-DOM hook (armed by _dom; NULL for plain parse):\n     * completed containers hand their value up this stack —\n     * one push per container, anchored for derived restore */\n    struct CCShapeB* sb;\n    struct CCShapeVal* vstack; const unsigned char** vanchor;\n    size_t vsp, vcap;\n    size_t vbase[512]; } ", 475, __cc_tpl_arena_11); cc__string_slot_push(&__cc_tpl_11, (n), __cc_tpl_arena_11); cc_string_push_buffer(&__cc_tpl_11, "__ctx;\nstatic __attribute__((noinline)) int ", 44, __cc_tpl_arena_11); cc__string_slot_push(&__cc_tpl_11, (n), __cc_tpl_arena_11); cc_string_push_buffer(&__cc_tpl_11, "__tgrow(", 8, __cc_tpl_arena_11); cc__string_slot_push(&__cc_tpl_11, (n), __cc_tpl_arena_11); cc_string_push_buffer(&__cc_tpl_11, "__ctx* c) {\n    size_t nc = c->cap * 2;\n    ", 44, __cc_tpl_arena_11); cc__string_slot_push(&__cc_tpl_11, (n), __cc_tpl_arena_11); cc_string_push_buffer(&__cc_tpl_11, "Node* nt = (", 12, __cc_tpl_arena_11); cc__string_slot_push(&__cc_tpl_11, (n), __cc_tpl_arena_11); cc_string_push_buffer(&__cc_tpl_11, "Node*)cc_arena_realloc(c->arena, c->arena, c->tape,\n        c->cap * sizeof(", 76, __cc_tpl_arena_11); cc__string_slot_push(&__cc_tpl_11, (n), __cc_tpl_arena_11); cc_string_push_buffer(&__cc_tpl_11, "Node), nc * sizeof(", 19, __cc_tpl_arena_11); cc__string_slot_push(&__cc_tpl_11, (n), __cc_tpl_arena_11); cc_string_push_buffer(&__cc_tpl_11, "Node), _Alignof(", 16, __cc_tpl_arena_11); cc__string_slot_push(&__cc_tpl_11, (n), __cc_tpl_arena_11); cc_string_push_buffer(&__cc_tpl_11, "Node));\n    if (!nt) return 0;\n    c->tape = nt;\n", 49, __cc_tpl_arena_11); __cc_tpl_11; }), a);
    if (has_codecs)
        cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_12 = (a); CCString __cc_tpl_12 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_12, "    if (c->mat_ptr) {\n        const char** np = (const char**)cc_arena_realloc(c->arena, c->arena, c->mat_ptr,\n            c->cap * sizeof(char*), nc * sizeof(char*), 8);\n        size_t* nl = (size_t*)cc_arena_realloc(c->arena, c->arena, c->mat_len,\n            c->cap * sizeof(size_t), nc * sizeof(size_t), 8);\n        unsigned char* ncw = (unsigned char*)cc_arena_realloc(c->arena, c->arena, c->mat_cow,\n            c->cap, nc, 1);\n        if (!np || !nl || !ncw) return 0;\n        memset(np + c->cap, 0, (nc - c->cap) * sizeof(char*));\n        c->mat_ptr = np; c->mat_len = nl; c->mat_cow = ncw;\n    }\n", 605, __cc_tpl_arena_12); __cc_tpl_12; }), a);
    cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_13 = (a); CCString __cc_tpl_13 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_13, "    c->cap = nc; return 1;\n}\n", 29, __cc_tpl_arena_13); __cc_tpl_13; }), a);
    return o;
}

/* ---- write tier: measure/put/chk text generators ----
 * Mode values mirror grammar_rules.c's RW_MEASURE/RW_PUT/RW_CHK.  Label
 * allocation and constant-run flushing stay with the C callers so the
 * emitted label sequence (and therefore the bytes) cannot drift. */
enum { CC_GR_WMEASURE = 0, CC_GR_WPUT = 1, CC_GR_WCHK = 2 };

/* printable-byte annotation for emitted compares: 61 -> "'='". '*' and '/'
 * stay hex so a comment can never contain a comment terminator. */
static inline CCString cc_gr_chr_text(CCArena* a, int b) {
    if (b >= 0x21 && b <= 0x7e && b != '\'' && b != '\\' && b != '*' && b != '/') {
        char ch = (char)b;
        return ({ CCArena* __cc_tpl_arena_14 = (a); CCString __cc_tpl_14 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_14, "'", 1, __cc_tpl_arena_14); cc__string_slot_push(&__cc_tpl_14, (ch), __cc_tpl_arena_14); cc_string_push_buffer(&__cc_tpl_14, "'", 1, __cc_tpl_arena_14); __cc_tpl_14; });
    }
    if (b == ' ')  return ({ CCArena* __cc_tpl_arena_15 = (a); CCString __cc_tpl_15 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_15, "' '", 3, __cc_tpl_arena_15); __cc_tpl_15; });
    if (b == '\n') return ({ CCArena* __cc_tpl_arena_16 = (a); CCString __cc_tpl_16 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_16, "nl", 2, __cc_tpl_arena_16); __cc_tpl_16; });
    if (b == '\r') return ({ CCArena* __cc_tpl_arena_17 = (a); CCString __cc_tpl_17 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_17, "cr", 2, __cc_tpl_arena_17); __cc_tpl_17; });
    if (b == '\t') return ({ CCArena* __cc_tpl_arena_18 = (a); CCString __cc_tpl_18 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_18, "tab", 3, __cc_tpl_arena_18); __cc_tpl_18; });
    {
        const char* hx = "0123456789ABCDEF";
        char hi = hx[(b >> 4) & 0xF];
        char lo = hx[b & 0xF];
        return ({ CCArena* __cc_tpl_arena_19 = (a); CCString __cc_tpl_19 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_19, "0x", 2, __cc_tpl_arena_19); cc__string_slot_push(&__cc_tpl_19, (hi), __cc_tpl_arena_19); cc__string_slot_push(&__cc_tpl_19, (lo), __cc_tpl_arena_19); __cc_tpl_19; });
    }
}

/* C-string-literal escape for emitted byte runs: identifier-ish bytes pass,
 * everything else becomes \NNN octal (always 3 digits, so a following
 * digit can't extend the escape). */
static inline CCString cc_gr_esc_text(CCArena* a, const unsigned char* src, int len) {
    CCString o = cc_string_new();
    for (int i = 0; i < len; i++) {
        unsigned char c = src[i];
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') || c == '_' || c == ' ' || c == '-') {
            char ch = (char)c;
            cc_string_push_buffer(&o, &ch, 1, a);
        } else {
            char b[4];
            b[0] = '\\';
            b[1] = (char)('0' + ((c >> 6) & 7));
            b[2] = (char)('0' + ((c >> 3) & 7));
            b[3] = (char)('0' + (c & 7));
            cc_string_push_buffer(&o, b, 4, a);
        }
    }
    return o;
}

/* pending constant-byte run: short runs as annotated stores, long runs as
 * one memcpy of an escaped literal; chk proves space once per run */
static inline CCString cc_gr_wflush_text(CCArena* a, int md, const unsigned char* lit, int nlit) {
    CCString o = cc_string_new();
    if (md == CC_GR_WCHK)
        cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_20 = (a); CCString __cc_tpl_20 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_20, "    if (cap - o < ", 18, __cc_tpl_arena_20); cc__string_slot_push(&__cc_tpl_20, (nlit), __cc_tpl_arena_20); cc_string_push_buffer(&__cc_tpl_20, ") return 0;\n", 12, __cc_tpl_arena_20); __cc_tpl_20; }), a);
    if (nlit <= 4) {
        for (int i = 0; i < nlit; i++) {
            int b = (int)lit[i];
            CCString c = cc_gr_chr_text(a, b);
            cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_21 = (a); CCString __cc_tpl_21 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_21, "    dst[o++] = (char)", 21, __cc_tpl_arena_21); cc__string_slot_push(&__cc_tpl_21, (b), __cc_tpl_arena_21); cc_string_push_buffer(&__cc_tpl_21, "; /*", 4, __cc_tpl_arena_21); cc__string_slot_push(&__cc_tpl_21, (c), __cc_tpl_arena_21); cc_string_push_buffer(&__cc_tpl_21, "*/\n", 3, __cc_tpl_arena_21); __cc_tpl_21; }), a);
        }
    } else {
        CCString esc = cc_gr_esc_text(a, lit, nlit);
        cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_22 = (a); CCString __cc_tpl_22 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_22, "    memcpy(dst + o, \"", 21, __cc_tpl_arena_22); cc__string_slot_push(&__cc_tpl_22, (esc), __cc_tpl_arena_22); cc_string_push_buffer(&__cc_tpl_22, "\", ", 3, __cc_tpl_arena_22); cc__string_slot_push(&__cc_tpl_22, (nlit), __cc_tpl_arena_22); cc_string_push_buffer(&__cc_tpl_22, "); o += ", 8, __cc_tpl_arena_22); cc__string_slot_push(&__cc_tpl_22, (nlit), __cc_tpl_arena_22); cc_string_push_buffer(&__cc_tpl_22, ";\n", 2, __cc_tpl_arena_22); __cc_tpl_22; }), a);
    }
    return o;
}

/* compare-chain digit count into `dest` — no divisions on the 1-6 digit
 * values wire formats actually carry; full 20-digit chain so it's total */
static inline CCString cc_gr_digits_expr_text(CCArena* a, int k, CCString dest) {
    CCString o = cc_string_new();
    unsigned long long p = 10;
    cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_23 = (a); CCString __cc_tpl_23 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_23, "      ", 6, __cc_tpl_arena_23); cc__string_slot_push(&__cc_tpl_23, (dest), __cc_tpl_arena_23); cc_string_push_buffer(&__cc_tpl_23, " ", 1, __cc_tpl_arena_23); __cc_tpl_23; }), a);
    for (int d = 1; d <= 19; d++) {
        cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_24 = (a); CCString __cc_tpl_24 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_24, "u", 1, __cc_tpl_arena_24); cc__string_slot_push(&__cc_tpl_24, (k), __cc_tpl_arena_24); cc_string_push_buffer(&__cc_tpl_24, " < ", 3, __cc_tpl_arena_24); cc__string_slot_push(&__cc_tpl_24, (p), __cc_tpl_arena_24); cc_string_push_buffer(&__cc_tpl_24, "ULL ? ", 6, __cc_tpl_arena_24); cc__string_slot_push(&__cc_tpl_24, (d), __cc_tpl_arena_24); cc_string_push_buffer(&__cc_tpl_24, "\n         : ", 12, __cc_tpl_arena_24); __cc_tpl_24; }), a);
        if (d < 19) p *= 10;
    }
    cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_25 = (a); CCString __cc_tpl_25 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_25, "20;\n", 4, __cc_tpl_arena_25); __cc_tpl_25; }), a);
    return o;
}

/* integer writer body (flush already done by the caller):
 *   measure — digit count by compare chain, no divisions
 *   put     — reversed digit buffer, space already proven
 *   chk     — count first, prove space, then write BACKWARD in place */
static inline CCString cc_gr_wint_text(CCArena* a, int k, int md, const char* expr, int is_unsigned) {
    CCString o = cc_string_new();
    if (md == CC_GR_WPUT && is_unsigned) {
        cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_26 = (a); CCString __cc_tpl_26 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_26, "    { unsigned long long u", 26, __cc_tpl_arena_26); cc__string_slot_push(&__cc_tpl_26, (k), __cc_tpl_arena_26); cc_string_push_buffer(&__cc_tpl_26, " = (unsigned long long)(", 24, __cc_tpl_arena_26); cc__string_slot_push(&__cc_tpl_26, (expr), __cc_tpl_arena_26); cc_string_push_buffer(&__cc_tpl_26, ");\n      char nb", 16, __cc_tpl_arena_26); cc__string_slot_push(&__cc_tpl_26, (k), __cc_tpl_arena_26); cc_string_push_buffer(&__cc_tpl_26, "[21]; int nl", 12, __cc_tpl_arena_26); cc__string_slot_push(&__cc_tpl_26, (k), __cc_tpl_arena_26); cc_string_push_buffer(&__cc_tpl_26, " = 0;\n      do { nb", 19, __cc_tpl_arena_26); cc__string_slot_push(&__cc_tpl_26, (k), __cc_tpl_arena_26); cc_string_push_buffer(&__cc_tpl_26, "[nl", 3, __cc_tpl_arena_26); cc__string_slot_push(&__cc_tpl_26, (k), __cc_tpl_arena_26); cc_string_push_buffer(&__cc_tpl_26, "++] = (char)('0' + (u", 21, __cc_tpl_arena_26); cc__string_slot_push(&__cc_tpl_26, (k), __cc_tpl_arena_26); cc_string_push_buffer(&__cc_tpl_26, " % 10)); u", 10, __cc_tpl_arena_26); cc__string_slot_push(&__cc_tpl_26, (k), __cc_tpl_arena_26); cc_string_push_buffer(&__cc_tpl_26, " /= 10; } while (u", 18, __cc_tpl_arena_26); cc__string_slot_push(&__cc_tpl_26, (k), __cc_tpl_arena_26); cc_string_push_buffer(&__cc_tpl_26, ");\n      while (nl", 18, __cc_tpl_arena_26); cc__string_slot_push(&__cc_tpl_26, (k), __cc_tpl_arena_26); cc_string_push_buffer(&__cc_tpl_26, ") dst[o++] = nb", 15, __cc_tpl_arena_26); cc__string_slot_push(&__cc_tpl_26, (k), __cc_tpl_arena_26); cc_string_push_buffer(&__cc_tpl_26, "[--nl", 5, __cc_tpl_arena_26); cc__string_slot_push(&__cc_tpl_26, (k), __cc_tpl_arena_26); cc_string_push_buffer(&__cc_tpl_26, "]; }\n", 5, __cc_tpl_arena_26); __cc_tpl_26; }), a);
    } else if (md == CC_GR_WPUT) {
        cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_27 = (a); CCString __cc_tpl_27 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_27, "    { long long x", 17, __cc_tpl_arena_27); cc__string_slot_push(&__cc_tpl_27, (k), __cc_tpl_arena_27); cc_string_push_buffer(&__cc_tpl_27, " = (long long)(", 15, __cc_tpl_arena_27); cc__string_slot_push(&__cc_tpl_27, (expr), __cc_tpl_arena_27); cc_string_push_buffer(&__cc_tpl_27, ");\n      char nb", 16, __cc_tpl_arena_27); cc__string_slot_push(&__cc_tpl_27, (k), __cc_tpl_arena_27); cc_string_push_buffer(&__cc_tpl_27, "[21]; int nl", 12, __cc_tpl_arena_27); cc__string_slot_push(&__cc_tpl_27, (k), __cc_tpl_arena_27); cc_string_push_buffer(&__cc_tpl_27, " = 0;\n      unsigned long long u", 32, __cc_tpl_arena_27); cc__string_slot_push(&__cc_tpl_27, (k), __cc_tpl_arena_27); cc_string_push_buffer(&__cc_tpl_27, " = x", 4, __cc_tpl_arena_27); cc__string_slot_push(&__cc_tpl_27, (k), __cc_tpl_arena_27); cc_string_push_buffer(&__cc_tpl_27, " < 0 ? (unsigned long long)-x", 29, __cc_tpl_arena_27); cc__string_slot_push(&__cc_tpl_27, (k), __cc_tpl_arena_27); cc_string_push_buffer(&__cc_tpl_27, " : (unsigned long long)x", 24, __cc_tpl_arena_27); cc__string_slot_push(&__cc_tpl_27, (k), __cc_tpl_arena_27); cc_string_push_buffer(&__cc_tpl_27, ";\n      if (x", 13, __cc_tpl_arena_27); cc__string_slot_push(&__cc_tpl_27, (k), __cc_tpl_arena_27); cc_string_push_buffer(&__cc_tpl_27, " < 0) dst[o++] = '-';\n      do { nb", 35, __cc_tpl_arena_27); cc__string_slot_push(&__cc_tpl_27, (k), __cc_tpl_arena_27); cc_string_push_buffer(&__cc_tpl_27, "[nl", 3, __cc_tpl_arena_27); cc__string_slot_push(&__cc_tpl_27, (k), __cc_tpl_arena_27); cc_string_push_buffer(&__cc_tpl_27, "++] = (char)('0' + (u", 21, __cc_tpl_arena_27); cc__string_slot_push(&__cc_tpl_27, (k), __cc_tpl_arena_27); cc_string_push_buffer(&__cc_tpl_27, " % 10)); u", 10, __cc_tpl_arena_27); cc__string_slot_push(&__cc_tpl_27, (k), __cc_tpl_arena_27); cc_string_push_buffer(&__cc_tpl_27, " /= 10; } while (u", 18, __cc_tpl_arena_27); cc__string_slot_push(&__cc_tpl_27, (k), __cc_tpl_arena_27); cc_string_push_buffer(&__cc_tpl_27, ");\n      while (nl", 18, __cc_tpl_arena_27); cc__string_slot_push(&__cc_tpl_27, (k), __cc_tpl_arena_27); cc_string_push_buffer(&__cc_tpl_27, ") dst[o++] = nb", 15, __cc_tpl_arena_27); cc__string_slot_push(&__cc_tpl_27, (k), __cc_tpl_arena_27); cc_string_push_buffer(&__cc_tpl_27, "[--nl", 5, __cc_tpl_arena_27); cc__string_slot_push(&__cc_tpl_27, (k), __cc_tpl_arena_27); cc_string_push_buffer(&__cc_tpl_27, "]; }\n", 5, __cc_tpl_arena_27); __cc_tpl_27; }), a);
    } else if (md == CC_GR_WCHK && is_unsigned) {
        cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_28 = (a); CCString __cc_tpl_28 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_28, "    { unsigned long long u", 26, __cc_tpl_arena_28); cc__string_slot_push(&__cc_tpl_28, (k), __cc_tpl_arena_28); cc_string_push_buffer(&__cc_tpl_28, " = (unsigned long long)(", 24, __cc_tpl_arena_28); cc__string_slot_push(&__cc_tpl_28, (expr), __cc_tpl_arena_28); cc_string_push_buffer(&__cc_tpl_28, ");\n      size_t nd", 18, __cc_tpl_arena_28); cc__string_slot_push(&__cc_tpl_28, (k), __cc_tpl_arena_28); cc_string_push_buffer(&__cc_tpl_28, ";\n", 2, __cc_tpl_arena_28); __cc_tpl_28; }), a);
        cc_gr__cat(&o, cc_gr_digits_expr_text(a, k, ({ CCArena* __cc_tpl_arena_29 = (a); CCString __cc_tpl_29 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_29, "nd", 2, __cc_tpl_arena_29); cc__string_slot_push(&__cc_tpl_29, (k), __cc_tpl_arena_29); cc_string_push_buffer(&__cc_tpl_29, " =", 2, __cc_tpl_arena_29); __cc_tpl_29; })), a);
        cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_30 = (a); CCString __cc_tpl_30 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_30, "      if (cap - o < nd", 22, __cc_tpl_arena_30); cc__string_slot_push(&__cc_tpl_30, (k), __cc_tpl_arena_30); cc_string_push_buffer(&__cc_tpl_30, ") return 0;\n      o += nd", 25, __cc_tpl_arena_30); cc__string_slot_push(&__cc_tpl_30, (k), __cc_tpl_arena_30); cc_string_push_buffer(&__cc_tpl_30, ";\n      { size_t q", 18, __cc_tpl_arena_30); cc__string_slot_push(&__cc_tpl_30, (k), __cc_tpl_arena_30); cc_string_push_buffer(&__cc_tpl_30, " = o;\n        do { dst[--q", 26, __cc_tpl_arena_30); cc__string_slot_push(&__cc_tpl_30, (k), __cc_tpl_arena_30); cc_string_push_buffer(&__cc_tpl_30, "] = (char)('0' + (u", 19, __cc_tpl_arena_30); cc__string_slot_push(&__cc_tpl_30, (k), __cc_tpl_arena_30); cc_string_push_buffer(&__cc_tpl_30, " % 10)); u", 10, __cc_tpl_arena_30); cc__string_slot_push(&__cc_tpl_30, (k), __cc_tpl_arena_30); cc_string_push_buffer(&__cc_tpl_30, " /= 10; } while (u", 18, __cc_tpl_arena_30); cc__string_slot_push(&__cc_tpl_30, (k), __cc_tpl_arena_30); cc_string_push_buffer(&__cc_tpl_30, "); } }\n", 7, __cc_tpl_arena_30); __cc_tpl_30; }), a);
    } else if (md == CC_GR_WCHK) {
        cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_31 = (a); CCString __cc_tpl_31 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_31, "    { long long x", 17, __cc_tpl_arena_31); cc__string_slot_push(&__cc_tpl_31, (k), __cc_tpl_arena_31); cc_string_push_buffer(&__cc_tpl_31, " = (long long)(", 15, __cc_tpl_arena_31); cc__string_slot_push(&__cc_tpl_31, (expr), __cc_tpl_arena_31); cc_string_push_buffer(&__cc_tpl_31, ");\n      unsigned long long u", 29, __cc_tpl_arena_31); cc__string_slot_push(&__cc_tpl_31, (k), __cc_tpl_arena_31); cc_string_push_buffer(&__cc_tpl_31, " = x", 4, __cc_tpl_arena_31); cc__string_slot_push(&__cc_tpl_31, (k), __cc_tpl_arena_31); cc_string_push_buffer(&__cc_tpl_31, " < 0 ? (unsigned long long)-x", 29, __cc_tpl_arena_31); cc__string_slot_push(&__cc_tpl_31, (k), __cc_tpl_arena_31); cc_string_push_buffer(&__cc_tpl_31, " : (unsigned long long)x", 24, __cc_tpl_arena_31); cc__string_slot_push(&__cc_tpl_31, (k), __cc_tpl_arena_31); cc_string_push_buffer(&__cc_tpl_31, ";\n      size_t nd", 17, __cc_tpl_arena_31); cc__string_slot_push(&__cc_tpl_31, (k), __cc_tpl_arena_31); cc_string_push_buffer(&__cc_tpl_31, ";\n", 2, __cc_tpl_arena_31); __cc_tpl_31; }), a);
        cc_gr__cat(&o, cc_gr_digits_expr_text(a, k, ({ CCArena* __cc_tpl_arena_32 = (a); CCString __cc_tpl_32 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_32, "nd", 2, __cc_tpl_arena_32); cc__string_slot_push(&__cc_tpl_32, (k), __cc_tpl_arena_32); cc_string_push_buffer(&__cc_tpl_32, " =", 2, __cc_tpl_arena_32); __cc_tpl_32; })), a);
        cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_33 = (a); CCString __cc_tpl_33 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_33, "      if (x", 11, __cc_tpl_arena_33); cc__string_slot_push(&__cc_tpl_33, (k), __cc_tpl_arena_33); cc_string_push_buffer(&__cc_tpl_33, " < 0) nd", 8, __cc_tpl_arena_33); cc__string_slot_push(&__cc_tpl_33, (k), __cc_tpl_arena_33); cc_string_push_buffer(&__cc_tpl_33, "++;\n      if (cap - o < nd", 26, __cc_tpl_arena_33); cc__string_slot_push(&__cc_tpl_33, (k), __cc_tpl_arena_33); cc_string_push_buffer(&__cc_tpl_33, ") return 0;\n      o += nd", 25, __cc_tpl_arena_33); cc__string_slot_push(&__cc_tpl_33, (k), __cc_tpl_arena_33); cc_string_push_buffer(&__cc_tpl_33, ";\n      { size_t q", 18, __cc_tpl_arena_33); cc__string_slot_push(&__cc_tpl_33, (k), __cc_tpl_arena_33); cc_string_push_buffer(&__cc_tpl_33, " = o;\n        do { dst[--q", 26, __cc_tpl_arena_33); cc__string_slot_push(&__cc_tpl_33, (k), __cc_tpl_arena_33); cc_string_push_buffer(&__cc_tpl_33, "] = (char)('0' + (u", 19, __cc_tpl_arena_33); cc__string_slot_push(&__cc_tpl_33, (k), __cc_tpl_arena_33); cc_string_push_buffer(&__cc_tpl_33, " % 10)); u", 10, __cc_tpl_arena_33); cc__string_slot_push(&__cc_tpl_33, (k), __cc_tpl_arena_33); cc_string_push_buffer(&__cc_tpl_33, " /= 10; } while (u", 18, __cc_tpl_arena_33); cc__string_slot_push(&__cc_tpl_33, (k), __cc_tpl_arena_33); cc_string_push_buffer(&__cc_tpl_33, ");\n        if (x", 16, __cc_tpl_arena_33); cc__string_slot_push(&__cc_tpl_33, (k), __cc_tpl_arena_33); cc_string_push_buffer(&__cc_tpl_33, " < 0) dst[--q", 13, __cc_tpl_arena_33); cc__string_slot_push(&__cc_tpl_33, (k), __cc_tpl_arena_33); cc_string_push_buffer(&__cc_tpl_33, "] = '-'; } }\n", 13, __cc_tpl_arena_33); __cc_tpl_33; }), a);
    } else if (is_unsigned) {
        cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_34 = (a); CCString __cc_tpl_34 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_34, "    { unsigned long long u", 26, __cc_tpl_arena_34); cc__string_slot_push(&__cc_tpl_34, (k), __cc_tpl_arena_34); cc_string_push_buffer(&__cc_tpl_34, " = (unsigned long long)(", 24, __cc_tpl_arena_34); cc__string_slot_push(&__cc_tpl_34, (expr), __cc_tpl_arena_34); cc_string_push_buffer(&__cc_tpl_34, ");\n", 3, __cc_tpl_arena_34); __cc_tpl_34; }), a);
        cc_gr__cat(&o, cc_gr_digits_expr_text(a, k, ({ CCArena* __cc_tpl_arena_35 = (a); CCString __cc_tpl_35 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_35, "o +=", 4, __cc_tpl_arena_35); __cc_tpl_35; })), a);
        cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_36 = (a); CCString __cc_tpl_36 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_36, "    }\n", 6, __cc_tpl_arena_36); __cc_tpl_36; }), a);
    } else {
        cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_37 = (a); CCString __cc_tpl_37 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_37, "    { long long x", 17, __cc_tpl_arena_37); cc__string_slot_push(&__cc_tpl_37, (k), __cc_tpl_arena_37); cc_string_push_buffer(&__cc_tpl_37, " = (long long)(", 15, __cc_tpl_arena_37); cc__string_slot_push(&__cc_tpl_37, (expr), __cc_tpl_arena_37); cc_string_push_buffer(&__cc_tpl_37, ");\n      unsigned long long u", 29, __cc_tpl_arena_37); cc__string_slot_push(&__cc_tpl_37, (k), __cc_tpl_arena_37); cc_string_push_buffer(&__cc_tpl_37, " = x", 4, __cc_tpl_arena_37); cc__string_slot_push(&__cc_tpl_37, (k), __cc_tpl_arena_37); cc_string_push_buffer(&__cc_tpl_37, " < 0 ? (unsigned long long)-x", 29, __cc_tpl_arena_37); cc__string_slot_push(&__cc_tpl_37, (k), __cc_tpl_arena_37); cc_string_push_buffer(&__cc_tpl_37, " : (unsigned long long)x", 24, __cc_tpl_arena_37); cc__string_slot_push(&__cc_tpl_37, (k), __cc_tpl_arena_37); cc_string_push_buffer(&__cc_tpl_37, ";\n      if (x", 13, __cc_tpl_arena_37); cc__string_slot_push(&__cc_tpl_37, (k), __cc_tpl_arena_37); cc_string_push_buffer(&__cc_tpl_37, " < 0) o++;\n", 11, __cc_tpl_arena_37); __cc_tpl_37; }), a);
        cc_gr__cat(&o, cc_gr_digits_expr_text(a, k, ({ CCArena* __cc_tpl_arena_38 = (a); CCString __cc_tpl_38 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_38, "o +=", 4, __cc_tpl_arena_38); __cc_tpl_38; })), a);
        cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_39 = (a); CCString __cc_tpl_39 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_39, "    }\n", 6, __cc_tpl_arena_39); __cc_tpl_39; }), a);
    }
    return o;
}

/* float writer body (flush already done by the caller): %.17g round-trips
 * doubles exactly; chk asks snprintf for the exact need first */
static inline CCString cc_gr_wfloat_text(CCArena* a, int k, int md, const char* field) {
    if (md == CC_GR_WPUT)
        return ({ CCArena* __cc_tpl_arena_40 = (a); CCString __cc_tpl_40 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_40, "    { char tb", 13, __cc_tpl_arena_40); cc__string_slot_push(&__cc_tpl_40, (k), __cc_tpl_arena_40); cc_string_push_buffer(&__cc_tpl_40, "[64]; int n", 11, __cc_tpl_arena_40); cc__string_slot_push(&__cc_tpl_40, (k), __cc_tpl_arena_40); cc_string_push_buffer(&__cc_tpl_40, " = snprintf(tb", 14, __cc_tpl_arena_40); cc__string_slot_push(&__cc_tpl_40, (k), __cc_tpl_arena_40); cc_string_push_buffer(&__cc_tpl_40, ", sizeof tb", 11, __cc_tpl_arena_40); cc__string_slot_push(&__cc_tpl_40, (k), __cc_tpl_arena_40); cc_string_push_buffer(&__cc_tpl_40, ", \"%.17g\", v->", 14, __cc_tpl_arena_40); cc__string_slot_push(&__cc_tpl_40, (field), __cc_tpl_arena_40); cc_string_push_buffer(&__cc_tpl_40, ");\n      memcpy(dst + o, tb", 27, __cc_tpl_arena_40); cc__string_slot_push(&__cc_tpl_40, (k), __cc_tpl_arena_40); cc_string_push_buffer(&__cc_tpl_40, ", (size_t)n", 11, __cc_tpl_arena_40); cc__string_slot_push(&__cc_tpl_40, (k), __cc_tpl_arena_40); cc_string_push_buffer(&__cc_tpl_40, "); o += (size_t)n", 17, __cc_tpl_arena_40); cc__string_slot_push(&__cc_tpl_40, (k), __cc_tpl_arena_40); cc_string_push_buffer(&__cc_tpl_40, "; }\n", 4, __cc_tpl_arena_40); __cc_tpl_40; });
    if (md == CC_GR_WCHK)
        return ({ CCArena* __cc_tpl_arena_41 = (a); CCString __cc_tpl_41 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_41, "    { char tb", 13, __cc_tpl_arena_41); cc__string_slot_push(&__cc_tpl_41, (k), __cc_tpl_arena_41); cc_string_push_buffer(&__cc_tpl_41, "[64]; int n", 11, __cc_tpl_arena_41); cc__string_slot_push(&__cc_tpl_41, (k), __cc_tpl_arena_41); cc_string_push_buffer(&__cc_tpl_41, " = snprintf(tb", 14, __cc_tpl_arena_41); cc__string_slot_push(&__cc_tpl_41, (k), __cc_tpl_arena_41); cc_string_push_buffer(&__cc_tpl_41, ", sizeof tb", 11, __cc_tpl_arena_41); cc__string_slot_push(&__cc_tpl_41, (k), __cc_tpl_arena_41); cc_string_push_buffer(&__cc_tpl_41, ", \"%.17g\", v->", 14, __cc_tpl_arena_41); cc__string_slot_push(&__cc_tpl_41, (field), __cc_tpl_arena_41); cc_string_push_buffer(&__cc_tpl_41, ");\n      if (n", 14, __cc_tpl_arena_41); cc__string_slot_push(&__cc_tpl_41, (k), __cc_tpl_arena_41); cc_string_push_buffer(&__cc_tpl_41, " < 0 || cap - o < (size_t)n", 27, __cc_tpl_arena_41); cc__string_slot_push(&__cc_tpl_41, (k), __cc_tpl_arena_41); cc_string_push_buffer(&__cc_tpl_41, ") return 0;\n      memcpy(dst + o, tb", 36, __cc_tpl_arena_41); cc__string_slot_push(&__cc_tpl_41, (k), __cc_tpl_arena_41); cc_string_push_buffer(&__cc_tpl_41, ", (size_t)n", 11, __cc_tpl_arena_41); cc__string_slot_push(&__cc_tpl_41, (k), __cc_tpl_arena_41); cc_string_push_buffer(&__cc_tpl_41, "); o += (size_t)n", 17, __cc_tpl_arena_41); cc__string_slot_push(&__cc_tpl_41, (k), __cc_tpl_arena_41); cc_string_push_buffer(&__cc_tpl_41, "; }\n", 4, __cc_tpl_arena_41); __cc_tpl_41; });
    return ({ CCArena* __cc_tpl_arena_42 = (a); CCString __cc_tpl_42 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_42, "    { char tb", 13, __cc_tpl_arena_42); cc__string_slot_push(&__cc_tpl_42, (k), __cc_tpl_arena_42); cc_string_push_buffer(&__cc_tpl_42, "[64]; o += (size_t)snprintf(tb", 30, __cc_tpl_arena_42); cc__string_slot_push(&__cc_tpl_42, (k), __cc_tpl_arena_42); cc_string_push_buffer(&__cc_tpl_42, ", sizeof tb", 11, __cc_tpl_arena_42); cc__string_slot_push(&__cc_tpl_42, (k), __cc_tpl_arena_42); cc_string_push_buffer(&__cc_tpl_42, ", \"%.17g\", v->", 14, __cc_tpl_arena_42); cc__string_slot_push(&__cc_tpl_42, (field), __cc_tpl_arena_42); cc_string_push_buffer(&__cc_tpl_42, "); }\n", 5, __cc_tpl_arena_42); __cc_tpl_42; });
}

/* slice-leaf payload body (pre/post literal runs and flush stay with the
 * caller): encoded when the rule's codec has an /encode inverse, memcpy
 * otherwise; k is meaningful only for the chk+encoder arm (the caller
 * allocates it only then, keeping the label sequence stable) */
static inline CCString cc_gr_wleaf_body_text(CCArena* a, int md, int k, const char* enc,
                                             const char* pexpr, const char* lexpr) {
    if (enc && md == CC_GR_WPUT)
        return ({ CCArena* __cc_tpl_arena_43 = (a); CCString __cc_tpl_43 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_43, "    o += ", 9, __cc_tpl_arena_43); cc__string_slot_push(&__cc_tpl_43, (enc), __cc_tpl_arena_43); cc_string_push_buffer(&__cc_tpl_43, "((const char*)", 14, __cc_tpl_arena_43); cc__string_slot_push(&__cc_tpl_43, (pexpr), __cc_tpl_arena_43); cc_string_push_buffer(&__cc_tpl_43, ", ", 2, __cc_tpl_arena_43); cc__string_slot_push(&__cc_tpl_43, (lexpr), __cc_tpl_arena_43); cc_string_push_buffer(&__cc_tpl_43, ", dst + o, (size_t)-1);\n", 24, __cc_tpl_arena_43); __cc_tpl_43; });
    if (enc && md == CC_GR_WCHK)
        return ({ CCArena* __cc_tpl_arena_44 = (a); CCString __cc_tpl_44 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_44, "    { size_t en", 15, __cc_tpl_arena_44); cc__string_slot_push(&__cc_tpl_44, (k), __cc_tpl_arena_44); cc_string_push_buffer(&__cc_tpl_44, " = ", 3, __cc_tpl_arena_44); cc__string_slot_push(&__cc_tpl_44, (enc), __cc_tpl_arena_44); cc_string_push_buffer(&__cc_tpl_44, "((const char*)", 14, __cc_tpl_arena_44); cc__string_slot_push(&__cc_tpl_44, (pexpr), __cc_tpl_arena_44); cc_string_push_buffer(&__cc_tpl_44, ", ", 2, __cc_tpl_arena_44); cc__string_slot_push(&__cc_tpl_44, (lexpr), __cc_tpl_arena_44); cc_string_push_buffer(&__cc_tpl_44, ", dst + o, cap - o);\n      if (en", 33, __cc_tpl_arena_44); cc__string_slot_push(&__cc_tpl_44, (k), __cc_tpl_arena_44); cc_string_push_buffer(&__cc_tpl_44, " > cap - o) return 0;\n      o += en", 35, __cc_tpl_arena_44); cc__string_slot_push(&__cc_tpl_44, (k), __cc_tpl_arena_44); cc_string_push_buffer(&__cc_tpl_44, "; }\n", 4, __cc_tpl_arena_44); __cc_tpl_44; });
    if (enc)
        return ({ CCArena* __cc_tpl_arena_45 = (a); CCString __cc_tpl_45 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_45, "    o += ", 9, __cc_tpl_arena_45); cc__string_slot_push(&__cc_tpl_45, (enc), __cc_tpl_arena_45); cc_string_push_buffer(&__cc_tpl_45, "((const char*)", 14, __cc_tpl_arena_45); cc__string_slot_push(&__cc_tpl_45, (pexpr), __cc_tpl_arena_45); cc_string_push_buffer(&__cc_tpl_45, ", ", 2, __cc_tpl_arena_45); cc__string_slot_push(&__cc_tpl_45, (lexpr), __cc_tpl_arena_45); cc_string_push_buffer(&__cc_tpl_45, ", 0, 0);\n", 9, __cc_tpl_arena_45); __cc_tpl_45; });
    if (md == CC_GR_WPUT)
        return ({ CCArena* __cc_tpl_arena_46 = (a); CCString __cc_tpl_46 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_46, "    memcpy(dst + o, ", 20, __cc_tpl_arena_46); cc__string_slot_push(&__cc_tpl_46, (pexpr), __cc_tpl_arena_46); cc_string_push_buffer(&__cc_tpl_46, ", ", 2, __cc_tpl_arena_46); cc__string_slot_push(&__cc_tpl_46, (lexpr), __cc_tpl_arena_46); cc_string_push_buffer(&__cc_tpl_46, "); o += ", 8, __cc_tpl_arena_46); cc__string_slot_push(&__cc_tpl_46, (lexpr), __cc_tpl_arena_46); cc_string_push_buffer(&__cc_tpl_46, ";\n", 2, __cc_tpl_arena_46); __cc_tpl_46; });
    if (md == CC_GR_WCHK)
        return ({ CCArena* __cc_tpl_arena_47 = (a); CCString __cc_tpl_47 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_47, "    if (cap - o < ", 18, __cc_tpl_arena_47); cc__string_slot_push(&__cc_tpl_47, (lexpr), __cc_tpl_arena_47); cc_string_push_buffer(&__cc_tpl_47, ") return 0;\n    memcpy(dst + o, ", 32, __cc_tpl_arena_47); cc__string_slot_push(&__cc_tpl_47, (pexpr), __cc_tpl_arena_47); cc_string_push_buffer(&__cc_tpl_47, ", ", 2, __cc_tpl_arena_47); cc__string_slot_push(&__cc_tpl_47, (lexpr), __cc_tpl_arena_47); cc_string_push_buffer(&__cc_tpl_47, "); o += ", 8, __cc_tpl_arena_47); cc__string_slot_push(&__cc_tpl_47, (lexpr), __cc_tpl_arena_47); cc_string_push_buffer(&__cc_tpl_47, ";\n", 2, __cc_tpl_arena_47); __cc_tpl_47; });
    return ({ CCArena* __cc_tpl_arena_48 = (a); CCString __cc_tpl_48 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_48, "    o += ", 9, __cc_tpl_arena_48); cc__string_slot_push(&__cc_tpl_48, (lexpr), __cc_tpl_arena_48); cc_string_push_buffer(&__cc_tpl_48, ";\n", 2, __cc_tpl_arena_48); __cc_tpl_48; });
}

/* raw bytes field: measure is just the length; put/chk memcpy (flush done
 * by the caller on the non-measure paths) */
static inline CCString cc_gr_wbytes_text(CCArena* a, int md, const char* field) {
    if (md == CC_GR_WMEASURE)
        return ({ CCArena* __cc_tpl_arena_49 = (a); CCString __cc_tpl_49 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_49, "    o += v->", 12, __cc_tpl_arena_49); cc__string_slot_push(&__cc_tpl_49, (field), __cc_tpl_arena_49); cc_string_push_buffer(&__cc_tpl_49, ".len;\n", 6, __cc_tpl_arena_49); __cc_tpl_49; });
    CCString o = cc_string_new();
    if (md == CC_GR_WCHK)
        cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_50 = (a); CCString __cc_tpl_50 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_50, "    if (cap - o < v->", 21, __cc_tpl_arena_50); cc__string_slot_push(&__cc_tpl_50, (field), __cc_tpl_arena_50); cc_string_push_buffer(&__cc_tpl_50, ".len) return 0;\n", 16, __cc_tpl_arena_50); __cc_tpl_50; }), a);
    cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_51 = (a); CCString __cc_tpl_51 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_51, "    memcpy(dst + o, v->", 23, __cc_tpl_arena_51); cc__string_slot_push(&__cc_tpl_51, (field), __cc_tpl_arena_51); cc_string_push_buffer(&__cc_tpl_51, ".ptr, v->", 9, __cc_tpl_arena_51); cc__string_slot_push(&__cc_tpl_51, (field), __cc_tpl_arena_51); cc_string_push_buffer(&__cc_tpl_51, ".len); o += v->", 15, __cc_tpl_arena_51); cc__string_slot_push(&__cc_tpl_51, (field), __cc_tpl_arena_51); cc_string_push_buffer(&__cc_tpl_51, ".len;\n", 6, __cc_tpl_arena_51); __cc_tpl_51; }), a);
    return o;
}

/* count-driven items: delegate each element to its own writer triplet */
static inline CCString cc_gr_witems_text(CCArena* a, int k, int md,
                                         const char* field, const char* etype) {
    CCString o = cc_string_new();
    cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_52 = (a); CCString __cc_tpl_52 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_52, "    { size_t i", 14, __cc_tpl_arena_52); cc__string_slot_push(&__cc_tpl_52, (k), __cc_tpl_arena_52); cc_string_push_buffer(&__cc_tpl_52, ";\n    for (i", 12, __cc_tpl_arena_52); cc__string_slot_push(&__cc_tpl_52, (k), __cc_tpl_arena_52); cc_string_push_buffer(&__cc_tpl_52, " = 0; i", 7, __cc_tpl_arena_52); cc__string_slot_push(&__cc_tpl_52, (k), __cc_tpl_arena_52); cc_string_push_buffer(&__cc_tpl_52, " < v->", 6, __cc_tpl_arena_52); cc__string_slot_push(&__cc_tpl_52, (field), __cc_tpl_arena_52); cc_string_push_buffer(&__cc_tpl_52, "_n; i", 5, __cc_tpl_arena_52); cc__string_slot_push(&__cc_tpl_52, (k), __cc_tpl_arena_52); cc_string_push_buffer(&__cc_tpl_52, "++)\n", 4, __cc_tpl_arena_52); __cc_tpl_52; }), a);
    if (md == CC_GR_WPUT)
        cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_53 = (a); CCString __cc_tpl_53 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_53, "        o += ", 13, __cc_tpl_arena_53); cc__string_slot_push(&__cc_tpl_53, (etype), __cc_tpl_arena_53); cc_string_push_buffer(&__cc_tpl_53, "__wput(&v->", 11, __cc_tpl_arena_53); cc__string_slot_push(&__cc_tpl_53, (field), __cc_tpl_arena_53); cc_string_push_buffer(&__cc_tpl_53, "[i", 2, __cc_tpl_arena_53); cc__string_slot_push(&__cc_tpl_53, (k), __cc_tpl_arena_53); cc_string_push_buffer(&__cc_tpl_53, "], dst + o);\n", 13, __cc_tpl_arena_53); __cc_tpl_53; }), a);
    else if (md == CC_GR_WCHK)
        cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_54 = (a); CCString __cc_tpl_54 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_54, "        if (!", 13, __cc_tpl_arena_54); cc__string_slot_push(&__cc_tpl_54, (etype), __cc_tpl_arena_54); cc_string_push_buffer(&__cc_tpl_54, "__wchk(&v->", 11, __cc_tpl_arena_54); cc__string_slot_push(&__cc_tpl_54, (field), __cc_tpl_arena_54); cc_string_push_buffer(&__cc_tpl_54, "[i", 2, __cc_tpl_arena_54); cc__string_slot_push(&__cc_tpl_54, (k), __cc_tpl_arena_54); cc_string_push_buffer(&__cc_tpl_54, "], dst, cap, &o)) return 0;\n", 28, __cc_tpl_arena_54); __cc_tpl_54; }), a);
    else
        cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_55 = (a); CCString __cc_tpl_55 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_55, "        o += ", 13, __cc_tpl_arena_55); cc__string_slot_push(&__cc_tpl_55, (etype), __cc_tpl_arena_55); cc_string_push_buffer(&__cc_tpl_55, "__wmeasure(&v->", 15, __cc_tpl_arena_55); cc__string_slot_push(&__cc_tpl_55, (field), __cc_tpl_arena_55); cc_string_push_buffer(&__cc_tpl_55, "[i", 2, __cc_tpl_arena_55); cc__string_slot_push(&__cc_tpl_55, (k), __cc_tpl_arena_55); cc_string_push_buffer(&__cc_tpl_55, "]);\n", 4, __cc_tpl_arena_55); __cc_tpl_55; }), a);
    cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_56 = (a); CCString __cc_tpl_56 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_56, "    }\n", 6, __cc_tpl_arena_56); __cc_tpl_56; }), a);
    return o;
}

/* narrowed delimited list: separator byte between elements comes from the
 * decomposed rule; open/close bytes ride the caller's constant-run WAcc */
static inline CCString cc_gr_wnarrow_list_text(CCArena* a, int k, int md,
                                               const char* field, const char* etype, int sep_b) {
    CCString o = cc_string_new();
    cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_57 = (a); CCString __cc_tpl_57 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_57, "    { size_t i", 14, __cc_tpl_arena_57); cc__string_slot_push(&__cc_tpl_57, (k), __cc_tpl_arena_57); cc_string_push_buffer(&__cc_tpl_57, ";\n    for (i", 12, __cc_tpl_arena_57); cc__string_slot_push(&__cc_tpl_57, (k), __cc_tpl_arena_57); cc_string_push_buffer(&__cc_tpl_57, " = 0; i", 7, __cc_tpl_arena_57); cc__string_slot_push(&__cc_tpl_57, (k), __cc_tpl_arena_57); cc_string_push_buffer(&__cc_tpl_57, " < v->", 6, __cc_tpl_arena_57); cc__string_slot_push(&__cc_tpl_57, (field), __cc_tpl_arena_57); cc_string_push_buffer(&__cc_tpl_57, "_n; i", 5, __cc_tpl_arena_57); cc__string_slot_push(&__cc_tpl_57, (k), __cc_tpl_arena_57); cc_string_push_buffer(&__cc_tpl_57, "++) {\n", 6, __cc_tpl_arena_57); __cc_tpl_57; }), a);
    if (md == CC_GR_WPUT)
        cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_58 = (a); CCString __cc_tpl_58 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_58, "        if (i", 13, __cc_tpl_arena_58); cc__string_slot_push(&__cc_tpl_58, (k), __cc_tpl_arena_58); cc_string_push_buffer(&__cc_tpl_58, ") dst[o++] = (char)", 19, __cc_tpl_arena_58); cc__string_slot_push(&__cc_tpl_58, (sep_b), __cc_tpl_arena_58); cc_string_push_buffer(&__cc_tpl_58, ";\n        o += ", 15, __cc_tpl_arena_58); cc__string_slot_push(&__cc_tpl_58, (etype), __cc_tpl_arena_58); cc_string_push_buffer(&__cc_tpl_58, "__wput(&v->", 11, __cc_tpl_arena_58); cc__string_slot_push(&__cc_tpl_58, (field), __cc_tpl_arena_58); cc_string_push_buffer(&__cc_tpl_58, "[i", 2, __cc_tpl_arena_58); cc__string_slot_push(&__cc_tpl_58, (k), __cc_tpl_arena_58); cc_string_push_buffer(&__cc_tpl_58, "], dst + o);\n", 13, __cc_tpl_arena_58); __cc_tpl_58; }), a);
    else if (md == CC_GR_WCHK)
        cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_59 = (a); CCString __cc_tpl_59 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_59, "        if (i", 13, __cc_tpl_arena_59); cc__string_slot_push(&__cc_tpl_59, (k), __cc_tpl_arena_59); cc_string_push_buffer(&__cc_tpl_59, ") { if (cap - o < 1) return 0; dst[o++] = (char)", 48, __cc_tpl_arena_59); cc__string_slot_push(&__cc_tpl_59, (sep_b), __cc_tpl_arena_59); cc_string_push_buffer(&__cc_tpl_59, "; }\n        if (!", 17, __cc_tpl_arena_59); cc__string_slot_push(&__cc_tpl_59, (etype), __cc_tpl_arena_59); cc_string_push_buffer(&__cc_tpl_59, "__wchk(&v->", 11, __cc_tpl_arena_59); cc__string_slot_push(&__cc_tpl_59, (field), __cc_tpl_arena_59); cc_string_push_buffer(&__cc_tpl_59, "[i", 2, __cc_tpl_arena_59); cc__string_slot_push(&__cc_tpl_59, (k), __cc_tpl_arena_59); cc_string_push_buffer(&__cc_tpl_59, "], dst, cap, &o)) return 0;\n", 28, __cc_tpl_arena_59); __cc_tpl_59; }), a);
    else
        cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_60 = (a); CCString __cc_tpl_60 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_60, "        if (i", 13, __cc_tpl_arena_60); cc__string_slot_push(&__cc_tpl_60, (k), __cc_tpl_arena_60); cc_string_push_buffer(&__cc_tpl_60, ") o++;\n        o += ", 20, __cc_tpl_arena_60); cc__string_slot_push(&__cc_tpl_60, (etype), __cc_tpl_arena_60); cc_string_push_buffer(&__cc_tpl_60, "__wmeasure(&v->", 15, __cc_tpl_arena_60); cc__string_slot_push(&__cc_tpl_60, (field), __cc_tpl_arena_60); cc_string_push_buffer(&__cc_tpl_60, "[i", 2, __cc_tpl_arena_60); cc__string_slot_push(&__cc_tpl_60, (k), __cc_tpl_arena_60); cc_string_push_buffer(&__cc_tpl_60, "]);\n", 4, __cc_tpl_arena_60); __cc_tpl_60; }), a);
    cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_61 = (a); CCString __cc_tpl_61 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_61, "    }\n    }\n", 12, __cc_tpl_arena_61); __cc_tpl_61; }), a);
    return o;
}

/* writer-triplet function heads/tails and the checked _write entry.
 * measure's tail folds the whole structural skeleton to ONE constant. */
static inline CCString cc_gr_write_head_text(CCArena* a, int md, const char* n) {
    if (md == CC_GR_WPUT)
        return ({ CCArena* __cc_tpl_arena_62 = (a); CCString __cc_tpl_62 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_62, "static __attribute__((unused)) size_t ", 38, __cc_tpl_arena_62); cc__string_slot_push(&__cc_tpl_62, (n), __cc_tpl_arena_62); cc_string_push_buffer(&__cc_tpl_62, "__wput(const ", 13, __cc_tpl_arena_62); cc__string_slot_push(&__cc_tpl_62, (n), __cc_tpl_arena_62); cc_string_push_buffer(&__cc_tpl_62, "* v, char* dst) {\n    size_t o = 0;\n    (void)v;\n", 49, __cc_tpl_arena_62); __cc_tpl_62; });
    if (md == CC_GR_WCHK)
        return ({ CCArena* __cc_tpl_arena_63 = (a); CCString __cc_tpl_63 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_63, "static __attribute__((unused)) int ", 35, __cc_tpl_arena_63); cc__string_slot_push(&__cc_tpl_63, (n), __cc_tpl_arena_63); cc_string_push_buffer(&__cc_tpl_63, "__wchk(const ", 13, __cc_tpl_arena_63); cc__string_slot_push(&__cc_tpl_63, (n), __cc_tpl_arena_63); cc_string_push_buffer(&__cc_tpl_63, "* v, char* dst, size_t cap, size_t* io) {\n    size_t o = *io;\n    (void)v;\n", 75, __cc_tpl_arena_63); __cc_tpl_63; });
    return ({ CCArena* __cc_tpl_arena_64 = (a); CCString __cc_tpl_64 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_64, "static __attribute__((unused)) size_t ", 38, __cc_tpl_arena_64); cc__string_slot_push(&__cc_tpl_64, (n), __cc_tpl_arena_64); cc_string_push_buffer(&__cc_tpl_64, "__wmeasure(const ", 17, __cc_tpl_arena_64); cc__string_slot_push(&__cc_tpl_64, (n), __cc_tpl_arena_64); cc_string_push_buffer(&__cc_tpl_64, "* v) {\n    size_t o = 0;\n    (void)v;\n", 38, __cc_tpl_arena_64); __cc_tpl_64; });
}

static inline CCString cc_gr_write_tail_text(CCArena* a, int md, long cfix) {
    CCString o = cc_string_new();
    if (md == CC_GR_WMEASURE && cfix)
        cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_65 = (a); CCString __cc_tpl_65 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_65, "    o += ", 9, __cc_tpl_arena_65); cc__string_slot_push(&__cc_tpl_65, (cfix), __cc_tpl_arena_65); cc_string_push_buffer(&__cc_tpl_65, ";   /* structural skeleton */\n", 30, __cc_tpl_arena_65); __cc_tpl_65; }), a);
    if (md == CC_GR_WCHK)
        cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_66 = (a); CCString __cc_tpl_66 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_66, "    *io = o;\n    return 1;\n}\n", 29, __cc_tpl_arena_66); __cc_tpl_66; }), a);
    else
        cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_67 = (a); CCString __cc_tpl_67 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_67, "    return o;\n}\n", 16, __cc_tpl_arena_67); __cc_tpl_67; }), a);
    return o;
}

static inline CCString cc_gr_write_entry_text(CCArena* a, const char* n) {
    return ({ CCArena* __cc_tpl_arena_68 = (a); CCString __cc_tpl_68 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_68, "static __attribute__((unused)) size_t ", 38, __cc_tpl_arena_68); cc__string_slot_push(&__cc_tpl_68, (n), __cc_tpl_arena_68); cc_string_push_buffer(&__cc_tpl_68, "_write(const ", 13, __cc_tpl_arena_68); cc__string_slot_push(&__cc_tpl_68, (n), __cc_tpl_arena_68); cc_string_push_buffer(&__cc_tpl_68, "* v, char* dst, size_t cap) {\n    size_t o = 0;\n    if (!", 57, __cc_tpl_arena_68); cc__string_slot_push(&__cc_tpl_68, (n), __cc_tpl_arena_68); cc_string_push_buffer(&__cc_tpl_68, "__wchk(v, dst, cap, &o)) return 0;\n    return o;\n}\n", 51, __cc_tpl_arena_68); __cc_tpl_68; });
}

/* ---- schema read tier: leaf term text generators ----
 * These carry only scalar/string parameters; recursion, key-dispatch
 * loops, and pad interleaving stay with the C drivers.  pfx is the
 * union-variant field prefix ("" or "u.<variant>."). */

/* extractor head/tail: G__x_R captures a span (xa,xb) + dirty flag */
static inline CCString cc_gr_x_head_text(CCArena* a, const char* g, const char* r) {
    return ({ CCArena* __cc_tpl_arena_69 = (a); CCString __cc_tpl_69 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_69, "static int ", 11, __cc_tpl_arena_69); cc__string_slot_push(&__cc_tpl_69, (g), __cc_tpl_arena_69); cc_string_push_buffer(&__cc_tpl_69, "__x_", 4, __cc_tpl_arena_69); cc__string_slot_push(&__cc_tpl_69, (r), __cc_tpl_arena_69); cc_string_push_buffer(&__cc_tpl_69, "(const unsigned char* s, size_t n, size_t* io,\n        size_t* xa, size_t* xb, int* xdr) {\n    size_t p = *io;\n    (void)s; (void)n;\n    *xdr = 0;\n", 147, __cc_tpl_arena_69); __cc_tpl_69; });
}

static inline CCString cc_gr_x_tail_text(CCArena* a, const char* fail) {
    return ({ CCArena* __cc_tpl_arena_70 = (a); CCString __cc_tpl_70 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_70, "    *io = p; return 1;\n", 23, __cc_tpl_arena_70); cc__string_slot_push(&__cc_tpl_70, (fail), __cc_tpl_arena_70); cc_string_push_buffer(&__cc_tpl_70, ":\n    return 0;\n}\n", 18, __cc_tpl_arena_70); __cc_tpl_70; });
}

/* matcher/pad call: G__<class>_<rule>(s, n, &p) or fail */
static inline CCString cc_gr_pad_call_text(CCArena* a, const char* g, const char* cls,
                                           const char* r, const char* fail) {
    return ({ CCArena* __cc_tpl_arena_71 = (a); CCString __cc_tpl_71 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_71, "    if (!", 9, __cc_tpl_arena_71); cc__string_slot_push(&__cc_tpl_71, (g), __cc_tpl_arena_71); cc_string_push_buffer(&__cc_tpl_71, "__", 2, __cc_tpl_arena_71); cc__string_slot_push(&__cc_tpl_71, (cls), __cc_tpl_arena_71); cc_string_push_buffer(&__cc_tpl_71, "_", 1, __cc_tpl_arena_71); cc__string_slot_push(&__cc_tpl_71, (r), __cc_tpl_arena_71); cc_string_push_buffer(&__cc_tpl_71, "(s, n, &p)) goto ", 17, __cc_tpl_arena_71); cc__string_slot_push(&__cc_tpl_71, (fail), __cc_tpl_arena_71); cc_string_push_buffer(&__cc_tpl_71, ";\n", 2, __cc_tpl_arena_71); __cc_tpl_71; });
}

/* fused int run: ONE accumulate-while-scan loop, no second conversion
 * pass, no charset loads — the hand-parser inner loop, derived */
static inline CCString cc_gr_bind_int_run_text(CCArena* a, int k, int neg,
                                               const char* pfx, const char* field,
                                               const char* fail) {
    CCString o = cc_string_new();
    cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_72 = (a); CCString __cc_tpl_72 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_72, "    { long long v", 17, __cc_tpl_arena_72); cc__string_slot_push(&__cc_tpl_72, (k), __cc_tpl_arena_72); cc_string_push_buffer(&__cc_tpl_72, " = 0; size_t d", 14, __cc_tpl_arena_72); cc__string_slot_push(&__cc_tpl_72, (k), __cc_tpl_arena_72); cc_string_push_buffer(&__cc_tpl_72, ";\n", 2, __cc_tpl_arena_72); __cc_tpl_72; }), a);
    if (neg)
        cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_73 = (a); CCString __cc_tpl_73 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_73, "      int ng", 12, __cc_tpl_arena_73); cc__string_slot_push(&__cc_tpl_73, (k), __cc_tpl_arena_73); cc_string_push_buffer(&__cc_tpl_73, " = 0;\n      if (p < n && s[p] == '-') { ng", 42, __cc_tpl_arena_73); cc__string_slot_push(&__cc_tpl_73, (k), __cc_tpl_arena_73); cc_string_push_buffer(&__cc_tpl_73, " = 1; p++; }\n", 13, __cc_tpl_arena_73); __cc_tpl_73; }), a);
    cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_74 = (a); CCString __cc_tpl_74 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_74, "      d", 7, __cc_tpl_arena_74); cc__string_slot_push(&__cc_tpl_74, (k), __cc_tpl_arena_74); cc_string_push_buffer(&__cc_tpl_74, " = p;\n      for (; p < n && s[p] >= '0' && s[p] <= '9'; p++)\n          v", 72, __cc_tpl_arena_74); cc__string_slot_push(&__cc_tpl_74, (k), __cc_tpl_arena_74); cc_string_push_buffer(&__cc_tpl_74, " = v", 4, __cc_tpl_arena_74); cc__string_slot_push(&__cc_tpl_74, (k), __cc_tpl_arena_74); cc_string_push_buffer(&__cc_tpl_74, " * 10 + (s[p] - '0');\n      if (p == d", 38, __cc_tpl_arena_74); cc__string_slot_push(&__cc_tpl_74, (k), __cc_tpl_arena_74); cc_string_push_buffer(&__cc_tpl_74, ") { if (p >= n) *cc_inc = 1; goto ", 34, __cc_tpl_arena_74); cc__string_slot_push(&__cc_tpl_74, (fail), __cc_tpl_arena_74); cc_string_push_buffer(&__cc_tpl_74, "; }\n", 4, __cc_tpl_arena_74); __cc_tpl_74; }), a);
    if (neg)
        cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_75 = (a); CCString __cc_tpl_75 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_75, "      out->", 11, __cc_tpl_arena_75); cc__string_slot_push(&__cc_tpl_75, (pfx), __cc_tpl_arena_75); cc__string_slot_push(&__cc_tpl_75, (field), __cc_tpl_arena_75); cc_string_push_buffer(&__cc_tpl_75, " = ng", 5, __cc_tpl_arena_75); cc__string_slot_push(&__cc_tpl_75, (k), __cc_tpl_arena_75); cc_string_push_buffer(&__cc_tpl_75, " ? -v", 5, __cc_tpl_arena_75); cc__string_slot_push(&__cc_tpl_75, (k), __cc_tpl_arena_75); cc_string_push_buffer(&__cc_tpl_75, " : v", 4, __cc_tpl_arena_75); cc__string_slot_push(&__cc_tpl_75, (k), __cc_tpl_arena_75); cc_string_push_buffer(&__cc_tpl_75, "; }\n", 4, __cc_tpl_arena_75); __cc_tpl_75; }), a);
    else
        cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_76 = (a); CCString __cc_tpl_76 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_76, "      out->", 11, __cc_tpl_arena_76); cc__string_slot_push(&__cc_tpl_76, (pfx), __cc_tpl_arena_76); cc__string_slot_push(&__cc_tpl_76, (field), __cc_tpl_arena_76); cc_string_push_buffer(&__cc_tpl_76, " = v", 4, __cc_tpl_arena_76); cc__string_slot_push(&__cc_tpl_76, (k), __cc_tpl_arena_76); cc_string_push_buffer(&__cc_tpl_76, "; }\n", 4, __cc_tpl_arena_76); __cc_tpl_76; }), a);
    return o;
}

/* span-bind openers: call the extractor, or set up locals for an inline
 * expansion (the C driver recurses rg_emit_node between open and close) */
static inline CCString cc_gr_bind_span_call_text(CCArena* a, int k, const char* g,
                                                 const char* r, const char* fail) {
    return ({ CCArena* __cc_tpl_arena_77 = (a); CCString __cc_tpl_77 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_77, "    { size_t xa", 15, __cc_tpl_arena_77); cc__string_slot_push(&__cc_tpl_77, (k), __cc_tpl_arena_77); cc_string_push_buffer(&__cc_tpl_77, ", xb", 4, __cc_tpl_arena_77); cc__string_slot_push(&__cc_tpl_77, (k), __cc_tpl_arena_77); cc_string_push_buffer(&__cc_tpl_77, "; int xd", 8, __cc_tpl_arena_77); cc__string_slot_push(&__cc_tpl_77, (k), __cc_tpl_arena_77); cc_string_push_buffer(&__cc_tpl_77, ";\n      if (!", 13, __cc_tpl_arena_77); cc__string_slot_push(&__cc_tpl_77, (g), __cc_tpl_arena_77); cc_string_push_buffer(&__cc_tpl_77, "__x_", 4, __cc_tpl_arena_77); cc__string_slot_push(&__cc_tpl_77, (r), __cc_tpl_arena_77); cc_string_push_buffer(&__cc_tpl_77, "(s, n, &p, &xa", 14, __cc_tpl_arena_77); cc__string_slot_push(&__cc_tpl_77, (k), __cc_tpl_arena_77); cc_string_push_buffer(&__cc_tpl_77, ", &xb", 5, __cc_tpl_arena_77); cc__string_slot_push(&__cc_tpl_77, (k), __cc_tpl_arena_77); cc_string_push_buffer(&__cc_tpl_77, ", &xd", 5, __cc_tpl_arena_77); cc__string_slot_push(&__cc_tpl_77, (k), __cc_tpl_arena_77); cc_string_push_buffer(&__cc_tpl_77, ")) goto ", 8, __cc_tpl_arena_77); cc__string_slot_push(&__cc_tpl_77, (fail), __cc_tpl_arena_77); cc_string_push_buffer(&__cc_tpl_77, ";\n", 2, __cc_tpl_arena_77); __cc_tpl_77; });
}

static inline CCString cc_gr_bind_span_inline_text(CCArena* a, int k) {
    return ({ CCArena* __cc_tpl_arena_78 = (a); CCString __cc_tpl_78 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_78, "    { size_t xa", 15, __cc_tpl_arena_78); cc__string_slot_push(&__cc_tpl_78, (k), __cc_tpl_arena_78); cc_string_push_buffer(&__cc_tpl_78, ", xb", 4, __cc_tpl_arena_78); cc__string_slot_push(&__cc_tpl_78, (k), __cc_tpl_arena_78); cc_string_push_buffer(&__cc_tpl_78, "; int xd", 8, __cc_tpl_arena_78); cc__string_slot_push(&__cc_tpl_78, (k), __cc_tpl_arena_78); cc_string_push_buffer(&__cc_tpl_78, " = 0;\n      { size_t* xa = &xa", 30, __cc_tpl_arena_78); cc__string_slot_push(&__cc_tpl_78, (k), __cc_tpl_arena_78); cc_string_push_buffer(&__cc_tpl_78, "; size_t* xb = &xb", 18, __cc_tpl_arena_78); cc__string_slot_push(&__cc_tpl_78, (k), __cc_tpl_arena_78); cc_string_push_buffer(&__cc_tpl_78, "; int* xdr = &xd", 16, __cc_tpl_arena_78); cc__string_slot_push(&__cc_tpl_78, (k), __cc_tpl_arena_78); cc_string_push_buffer(&__cc_tpl_78, ";\n        (void)xa; (void)xb; (void)xdr;\n", 41, __cc_tpl_arena_78); __cc_tpl_78; });
}

/* int over the captured span: matcher validated the digits already */
static inline CCString cc_gr_bind_int_span_text(CCArena* a, int k,
                                                const char* pfx, const char* field) {
    return ({ CCArena* __cc_tpl_arena_79 = (a); CCString __cc_tpl_79 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_79, "      (void)xd", 14, __cc_tpl_arena_79); cc__string_slot_push(&__cc_tpl_79, (k), __cc_tpl_arena_79); cc_string_push_buffer(&__cc_tpl_79, ";\n      { long long v", 21, __cc_tpl_arena_79); cc__string_slot_push(&__cc_tpl_79, (k), __cc_tpl_arena_79); cc_string_push_buffer(&__cc_tpl_79, " = 0; size_t q", 14, __cc_tpl_arena_79); cc__string_slot_push(&__cc_tpl_79, (k), __cc_tpl_arena_79); cc_string_push_buffer(&__cc_tpl_79, " = xa", 5, __cc_tpl_arena_79); cc__string_slot_push(&__cc_tpl_79, (k), __cc_tpl_arena_79); cc_string_push_buffer(&__cc_tpl_79, "; int ng", 8, __cc_tpl_arena_79); cc__string_slot_push(&__cc_tpl_79, (k), __cc_tpl_arena_79); cc_string_push_buffer(&__cc_tpl_79, " = 0;\n        if (q", 19, __cc_tpl_arena_79); cc__string_slot_push(&__cc_tpl_79, (k), __cc_tpl_arena_79); cc_string_push_buffer(&__cc_tpl_79, " < xb", 5, __cc_tpl_arena_79); cc__string_slot_push(&__cc_tpl_79, (k), __cc_tpl_arena_79); cc_string_push_buffer(&__cc_tpl_79, " && s[q", 7, __cc_tpl_arena_79); cc__string_slot_push(&__cc_tpl_79, (k), __cc_tpl_arena_79); cc_string_push_buffer(&__cc_tpl_79, "] == '-') { ng", 14, __cc_tpl_arena_79); cc__string_slot_push(&__cc_tpl_79, (k), __cc_tpl_arena_79); cc_string_push_buffer(&__cc_tpl_79, " = 1; q", 7, __cc_tpl_arena_79); cc__string_slot_push(&__cc_tpl_79, (k), __cc_tpl_arena_79); cc_string_push_buffer(&__cc_tpl_79, "++; }\n        for (; q", 22, __cc_tpl_arena_79); cc__string_slot_push(&__cc_tpl_79, (k), __cc_tpl_arena_79); cc_string_push_buffer(&__cc_tpl_79, " < xb", 5, __cc_tpl_arena_79); cc__string_slot_push(&__cc_tpl_79, (k), __cc_tpl_arena_79); cc_string_push_buffer(&__cc_tpl_79, " && s[q", 7, __cc_tpl_arena_79); cc__string_slot_push(&__cc_tpl_79, (k), __cc_tpl_arena_79); cc_string_push_buffer(&__cc_tpl_79, "] >= '0' && s[q", 15, __cc_tpl_arena_79); cc__string_slot_push(&__cc_tpl_79, (k), __cc_tpl_arena_79); cc_string_push_buffer(&__cc_tpl_79, "] <= '9'; q", 11, __cc_tpl_arena_79); cc__string_slot_push(&__cc_tpl_79, (k), __cc_tpl_arena_79); cc_string_push_buffer(&__cc_tpl_79, "++)\n            v", 17, __cc_tpl_arena_79); cc__string_slot_push(&__cc_tpl_79, (k), __cc_tpl_arena_79); cc_string_push_buffer(&__cc_tpl_79, " = v", 4, __cc_tpl_arena_79); cc__string_slot_push(&__cc_tpl_79, (k), __cc_tpl_arena_79); cc_string_push_buffer(&__cc_tpl_79, " * 10 + (s[q", 12, __cc_tpl_arena_79); cc__string_slot_push(&__cc_tpl_79, (k), __cc_tpl_arena_79); cc_string_push_buffer(&__cc_tpl_79, "] - '0');\n        out->", 23, __cc_tpl_arena_79); cc__string_slot_push(&__cc_tpl_79, (pfx), __cc_tpl_arena_79); cc__string_slot_push(&__cc_tpl_79, (field), __cc_tpl_arena_79); cc_string_push_buffer(&__cc_tpl_79, " = ng", 5, __cc_tpl_arena_79); cc__string_slot_push(&__cc_tpl_79, (k), __cc_tpl_arena_79); cc_string_push_buffer(&__cc_tpl_79, " ? -v", 5, __cc_tpl_arena_79); cc__string_slot_push(&__cc_tpl_79, (k), __cc_tpl_arena_79); cc_string_push_buffer(&__cc_tpl_79, " : v", 4, __cc_tpl_arena_79); cc__string_slot_push(&__cc_tpl_79, (k), __cc_tpl_arena_79); cc_string_push_buffer(&__cc_tpl_79, "; } }\n", 6, __cc_tpl_arena_79); __cc_tpl_79; });
}

/* float over the captured span: ffc parses JSON doubles */
static inline CCString cc_gr_bind_float_span_text(CCArena* a, int k, const char* pfx,
                                                  const char* field, const char* fail) {
    return ({ CCArena* __cc_tpl_arena_80 = (a); CCString __cc_tpl_80 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_80, "      (void)xd", 14, __cc_tpl_arena_80); cc__string_slot_push(&__cc_tpl_80, (k), __cc_tpl_arena_80); cc_string_push_buffer(&__cc_tpl_80, ";\n      { ffc_parse_options o", 29, __cc_tpl_arena_80); cc__string_slot_push(&__cc_tpl_80, (k), __cc_tpl_arena_80); cc_string_push_buffer(&__cc_tpl_80, "; o", 3, __cc_tpl_arena_80); cc__string_slot_push(&__cc_tpl_80, (k), __cc_tpl_arena_80); cc_string_push_buffer(&__cc_tpl_80, ".format = FFC_PRESET_JSON; o", 28, __cc_tpl_arena_80); cc__string_slot_push(&__cc_tpl_80, (k), __cc_tpl_arena_80); cc_string_push_buffer(&__cc_tpl_80, ".decimal_point = '.';\n        double v", 38, __cc_tpl_arena_80); cc__string_slot_push(&__cc_tpl_80, (k), __cc_tpl_arena_80); cc_string_push_buffer(&__cc_tpl_80, " = 0.0;\n        ffc_result r", 28, __cc_tpl_arena_80); cc__string_slot_push(&__cc_tpl_80, (k), __cc_tpl_arena_80); cc_string_push_buffer(&__cc_tpl_80, " = ffc_from_chars_double_options(\n            (const char*)(s + xa", 66, __cc_tpl_arena_80); cc__string_slot_push(&__cc_tpl_80, (k), __cc_tpl_arena_80); cc_string_push_buffer(&__cc_tpl_80, "), (const char*)(s + xb", 23, __cc_tpl_arena_80); cc__string_slot_push(&__cc_tpl_80, (k), __cc_tpl_arena_80); cc_string_push_buffer(&__cc_tpl_80, "), &v", 5, __cc_tpl_arena_80); cc__string_slot_push(&__cc_tpl_80, (k), __cc_tpl_arena_80); cc_string_push_buffer(&__cc_tpl_80, ", o", 3, __cc_tpl_arena_80); cc__string_slot_push(&__cc_tpl_80, (k), __cc_tpl_arena_80); cc_string_push_buffer(&__cc_tpl_80, ");\n        if (r", 16, __cc_tpl_arena_80); cc__string_slot_push(&__cc_tpl_80, (k), __cc_tpl_arena_80); cc_string_push_buffer(&__cc_tpl_80, ".outcome != FFC_OUTCOME_OK) goto ", 33, __cc_tpl_arena_80); cc__string_slot_push(&__cc_tpl_80, (fail), __cc_tpl_arena_80); cc_string_push_buffer(&__cc_tpl_80, ";\n        out->", 15, __cc_tpl_arena_80); cc__string_slot_push(&__cc_tpl_80, (pfx), __cc_tpl_arena_80); cc__string_slot_push(&__cc_tpl_80, (field), __cc_tpl_arena_80); cc_string_push_buffer(&__cc_tpl_80, " = v", 4, __cc_tpl_arena_80); cc__string_slot_push(&__cc_tpl_80, (k), __cc_tpl_arena_80); cc_string_push_buffer(&__cc_tpl_80, "; } }\n", 6, __cc_tpl_arena_80); __cc_tpl_80; });
}

/* slice bind closers: clean spans borrow raw, dirty spans decode through
 * the rule's codec (same provenance contract as the collect/DOM tiers) */
static inline CCString cc_gr_bind_slice_codec_text(CCArena* a, int k, const char* pfx,
                                                   const char* field, const char* codec,
                                                   const char* fail) {
    return ({ CCArena* __cc_tpl_arena_81 = (a); CCString __cc_tpl_81 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_81, "      if (!xd", 13, __cc_tpl_arena_81); cc__string_slot_push(&__cc_tpl_81, (k), __cc_tpl_arena_81); cc_string_push_buffer(&__cc_tpl_81, ") out->", 7, __cc_tpl_arena_81); cc__string_slot_push(&__cc_tpl_81, (pfx), __cc_tpl_arena_81); cc__string_slot_push(&__cc_tpl_81, (field), __cc_tpl_arena_81); cc_string_push_buffer(&__cc_tpl_81, " = cc_slice_from_buffer((void*)(s + xa", 38, __cc_tpl_arena_81); cc__string_slot_push(&__cc_tpl_81, (k), __cc_tpl_arena_81); cc_string_push_buffer(&__cc_tpl_81, "), xb", 5, __cc_tpl_arena_81); cc__string_slot_push(&__cc_tpl_81, (k), __cc_tpl_arena_81); cc_string_push_buffer(&__cc_tpl_81, " - xa", 5, __cc_tpl_arena_81); cc__string_slot_push(&__cc_tpl_81, (k), __cc_tpl_arena_81); cc_string_push_buffer(&__cc_tpl_81, ");\n      else if (!", 19, __cc_tpl_arena_81); cc__string_slot_push(&__cc_tpl_81, (codec), __cc_tpl_arena_81); cc_string_push_buffer(&__cc_tpl_81, "((const char*)(s + xa", 21, __cc_tpl_arena_81); cc__string_slot_push(&__cc_tpl_81, (k), __cc_tpl_arena_81); cc_string_push_buffer(&__cc_tpl_81, "), xb", 5, __cc_tpl_arena_81); cc__string_slot_push(&__cc_tpl_81, (k), __cc_tpl_arena_81); cc_string_push_buffer(&__cc_tpl_81, " - xa", 5, __cc_tpl_arena_81); cc__string_slot_push(&__cc_tpl_81, (k), __cc_tpl_arena_81); cc_string_push_buffer(&__cc_tpl_81, ", &out->", 8, __cc_tpl_arena_81); cc__string_slot_push(&__cc_tpl_81, (pfx), __cc_tpl_arena_81); cc__string_slot_push(&__cc_tpl_81, (field), __cc_tpl_arena_81); cc_string_push_buffer(&__cc_tpl_81, ", arena)) goto ", 15, __cc_tpl_arena_81); cc__string_slot_push(&__cc_tpl_81, (fail), __cc_tpl_arena_81); cc_string_push_buffer(&__cc_tpl_81, ";\n    }\n", 8, __cc_tpl_arena_81); __cc_tpl_81; });
}

static inline CCString cc_gr_bind_slice_borrow_text(CCArena* a, int k,
                                                    const char* pfx, const char* field) {
    return ({ CCArena* __cc_tpl_arena_82 = (a); CCString __cc_tpl_82 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_82, "      (void)xd", 14, __cc_tpl_arena_82); cc__string_slot_push(&__cc_tpl_82, (k), __cc_tpl_arena_82); cc_string_push_buffer(&__cc_tpl_82, ";\n      out->", 13, __cc_tpl_arena_82); cc__string_slot_push(&__cc_tpl_82, (pfx), __cc_tpl_arena_82); cc__string_slot_push(&__cc_tpl_82, (field), __cc_tpl_arena_82); cc_string_push_buffer(&__cc_tpl_82, " = cc_slice_from_buffer((void*)(s + xa", 38, __cc_tpl_arena_82); cc__string_slot_push(&__cc_tpl_82, (k), __cc_tpl_arena_82); cc_string_push_buffer(&__cc_tpl_82, "), xb", 5, __cc_tpl_arena_82); cc__string_slot_push(&__cc_tpl_82, (k), __cc_tpl_arena_82); cc_string_push_buffer(&__cc_tpl_82, " - xa", 5, __cc_tpl_arena_82); cc__string_slot_push(&__cc_tpl_82, (k), __cc_tpl_arena_82); cc_string_push_buffer(&__cc_tpl_82, ");\n    }\n", 9, __cc_tpl_arena_82); __cc_tpl_82; });
}

/* counted opaque payload: exactly out->cfield bytes, borrowed */
static inline CCString cc_gr_bytes_term_text(CCArena* a, int k, const char* pfx,
                                             const char* cfield, const char* field,
                                             const char* fail) {
    return ({ CCArena* __cc_tpl_arena_83 = (a); CCString __cc_tpl_83 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_83, "    { long long L", 17, __cc_tpl_arena_83); cc__string_slot_push(&__cc_tpl_83, (k), __cc_tpl_arena_83); cc_string_push_buffer(&__cc_tpl_83, " = out->", 8, __cc_tpl_arena_83); cc__string_slot_push(&__cc_tpl_83, (pfx), __cc_tpl_arena_83); cc__string_slot_push(&__cc_tpl_83, (cfield), __cc_tpl_arena_83); cc_string_push_buffer(&__cc_tpl_83, ";\n      if (L", 13, __cc_tpl_arena_83); cc__string_slot_push(&__cc_tpl_83, (k), __cc_tpl_arena_83); cc_string_push_buffer(&__cc_tpl_83, " < 0) goto ", 11, __cc_tpl_arena_83); cc__string_slot_push(&__cc_tpl_83, (fail), __cc_tpl_arena_83); cc_string_push_buffer(&__cc_tpl_83, ";\n      if ((unsigned long long)L", 33, __cc_tpl_arena_83); cc__string_slot_push(&__cc_tpl_83, (k), __cc_tpl_arena_83); cc_string_push_buffer(&__cc_tpl_83, " > (unsigned long long)(n - p)) { *cc_inc = 1; goto ", 52, __cc_tpl_arena_83); cc__string_slot_push(&__cc_tpl_83, (fail), __cc_tpl_arena_83); cc_string_push_buffer(&__cc_tpl_83, "; }\n      out->", 15, __cc_tpl_arena_83); cc__string_slot_push(&__cc_tpl_83, (pfx), __cc_tpl_arena_83); cc__string_slot_push(&__cc_tpl_83, (field), __cc_tpl_arena_83); cc_string_push_buffer(&__cc_tpl_83, " = cc_slice_from_buffer((void*)(s + p), (size_t)L", 49, __cc_tpl_arena_83); cc__string_slot_push(&__cc_tpl_83, (k), __cc_tpl_arena_83); cc_string_push_buffer(&__cc_tpl_83, ");\n      p += (size_t)L", 23, __cc_tpl_arena_83); cc__string_slot_push(&__cc_tpl_83, (k), __cc_tpl_arena_83); cc_string_push_buffer(&__cc_tpl_83, "; }\n", 4, __cc_tpl_arena_83); __cc_tpl_83; });
}

/* count-driven items: cap > 0 fills the struct's INLINE array (zero
 * alloc; larger count is a protocol error), else exact-size allocation */
static inline CCString cc_gr_items_counted_text(CCArena* a, int k, const char* T,
                                                const char* pfx, const char* field,
                                                const char* cfield, int cap,
                                                const char* fail) {
    if (cap > 0)
        return ({ CCArena* __cc_tpl_arena_84 = (a); CCString __cc_tpl_84 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_84, "    { long long C", 17, __cc_tpl_arena_84); cc__string_slot_push(&__cc_tpl_84, (k), __cc_tpl_arena_84); cc_string_push_buffer(&__cc_tpl_84, " = out->", 8, __cc_tpl_arena_84); cc__string_slot_push(&__cc_tpl_84, (pfx), __cc_tpl_arena_84); cc__string_slot_push(&__cc_tpl_84, (cfield), __cc_tpl_arena_84); cc_string_push_buffer(&__cc_tpl_84, ";\n      if (C", 13, __cc_tpl_arena_84); cc__string_slot_push(&__cc_tpl_84, (k), __cc_tpl_arena_84); cc_string_push_buffer(&__cc_tpl_84, " < 0 || C", 9, __cc_tpl_arena_84); cc__string_slot_push(&__cc_tpl_84, (k), __cc_tpl_arena_84); cc_string_push_buffer(&__cc_tpl_84, " > ", 3, __cc_tpl_arena_84); cc__string_slot_push(&__cc_tpl_84, (cap), __cc_tpl_arena_84); cc_string_push_buffer(&__cc_tpl_84, ") goto ", 7, __cc_tpl_arena_84); cc__string_slot_push(&__cc_tpl_84, (fail), __cc_tpl_arena_84); cc_string_push_buffer(&__cc_tpl_84, ";\n      if ((unsigned long long)C", 33, __cc_tpl_arena_84); cc__string_slot_push(&__cc_tpl_84, (k), __cc_tpl_arena_84); cc_string_push_buffer(&__cc_tpl_84, " > n - p) { *cc_inc = 1; goto ", 30, __cc_tpl_arena_84); cc__string_slot_push(&__cc_tpl_84, (fail), __cc_tpl_arena_84); cc_string_push_buffer(&__cc_tpl_84, "; }\n      for (long long i", 26, __cc_tpl_arena_84); cc__string_slot_push(&__cc_tpl_84, (k), __cc_tpl_arena_84); cc_string_push_buffer(&__cc_tpl_84, " = 0; i", 7, __cc_tpl_arena_84); cc__string_slot_push(&__cc_tpl_84, (k), __cc_tpl_arena_84); cc_string_push_buffer(&__cc_tpl_84, " < C", 4, __cc_tpl_arena_84); cc__string_slot_push(&__cc_tpl_84, (k), __cc_tpl_arena_84); cc_string_push_buffer(&__cc_tpl_84, "; i", 3, __cc_tpl_arena_84); cc__string_slot_push(&__cc_tpl_84, (k), __cc_tpl_arena_84); cc_string_push_buffer(&__cc_tpl_84, "++)\n          if (!", 19, __cc_tpl_arena_84); cc__string_slot_push(&__cc_tpl_84, (T), __cc_tpl_arena_84); cc_string_push_buffer(&__cc_tpl_84, "__fill(s, n, &p, arena, &out->", 30, __cc_tpl_arena_84); cc__string_slot_push(&__cc_tpl_84, (pfx), __cc_tpl_arena_84); cc__string_slot_push(&__cc_tpl_84, (field), __cc_tpl_arena_84); cc_string_push_buffer(&__cc_tpl_84, "[i", 2, __cc_tpl_arena_84); cc__string_slot_push(&__cc_tpl_84, (k), __cc_tpl_arena_84); cc_string_push_buffer(&__cc_tpl_84, "], cc_inc, cc_depth + 1)) goto ", 31, __cc_tpl_arena_84); cc__string_slot_push(&__cc_tpl_84, (fail), __cc_tpl_arena_84); cc_string_push_buffer(&__cc_tpl_84, ";\n      out->", 13, __cc_tpl_arena_84); cc__string_slot_push(&__cc_tpl_84, (pfx), __cc_tpl_arena_84); cc__string_slot_push(&__cc_tpl_84, (field), __cc_tpl_arena_84); cc_string_push_buffer(&__cc_tpl_84, "_n = (size_t)C", 14, __cc_tpl_arena_84); cc__string_slot_push(&__cc_tpl_84, (k), __cc_tpl_arena_84); cc_string_push_buffer(&__cc_tpl_84, "; }\n", 4, __cc_tpl_arena_84); __cc_tpl_84; });
    return ({ CCArena* __cc_tpl_arena_85 = (a); CCString __cc_tpl_85 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_85, "    { long long C", 17, __cc_tpl_arena_85); cc__string_slot_push(&__cc_tpl_85, (k), __cc_tpl_arena_85); cc_string_push_buffer(&__cc_tpl_85, " = out->", 8, __cc_tpl_arena_85); cc__string_slot_push(&__cc_tpl_85, (pfx), __cc_tpl_arena_85); cc__string_slot_push(&__cc_tpl_85, (cfield), __cc_tpl_arena_85); cc_string_push_buffer(&__cc_tpl_85, ";\n      if (C", 13, __cc_tpl_arena_85); cc__string_slot_push(&__cc_tpl_85, (k), __cc_tpl_arena_85); cc_string_push_buffer(&__cc_tpl_85, " < 0) goto ", 11, __cc_tpl_arena_85); cc__string_slot_push(&__cc_tpl_85, (fail), __cc_tpl_arena_85); cc_string_push_buffer(&__cc_tpl_85, ";\n      if ((unsigned long long)C", 33, __cc_tpl_arena_85); cc__string_slot_push(&__cc_tpl_85, (k), __cc_tpl_arena_85); cc_string_push_buffer(&__cc_tpl_85, " > n - p) { *cc_inc = 1; goto ", 30, __cc_tpl_arena_85); cc__string_slot_push(&__cc_tpl_85, (fail), __cc_tpl_arena_85); cc_string_push_buffer(&__cc_tpl_85, "; }\n      size_t cap", 20, __cc_tpl_arena_85); cc__string_slot_push(&__cc_tpl_85, (k), __cc_tpl_arena_85); cc_string_push_buffer(&__cc_tpl_85, " = C", 4, __cc_tpl_arena_85); cc__string_slot_push(&__cc_tpl_85, (k), __cc_tpl_arena_85); cc_string_push_buffer(&__cc_tpl_85, " > 0 ? (size_t)C", 16, __cc_tpl_arena_85); cc__string_slot_push(&__cc_tpl_85, (k), __cc_tpl_arena_85); cc_string_push_buffer(&__cc_tpl_85, " : 1;\n      ", 12, __cc_tpl_arena_85); cc__string_slot_push(&__cc_tpl_85, (T), __cc_tpl_arena_85); cc_string_push_buffer(&__cc_tpl_85, "* v", 3, __cc_tpl_arena_85); cc__string_slot_push(&__cc_tpl_85, (k), __cc_tpl_arena_85); cc_string_push_buffer(&__cc_tpl_85, " = (", 4, __cc_tpl_arena_85); cc__string_slot_push(&__cc_tpl_85, (T), __cc_tpl_arena_85); cc_string_push_buffer(&__cc_tpl_85, "*)cc_arena_alloc_local(arena, cap", 33, __cc_tpl_arena_85); cc__string_slot_push(&__cc_tpl_85, (k), __cc_tpl_arena_85); cc_string_push_buffer(&__cc_tpl_85, " * sizeof(", 10, __cc_tpl_arena_85); cc__string_slot_push(&__cc_tpl_85, (T), __cc_tpl_arena_85); cc_string_push_buffer(&__cc_tpl_85, "), _Alignof(", 12, __cc_tpl_arena_85); cc__string_slot_push(&__cc_tpl_85, (T), __cc_tpl_arena_85); cc_string_push_buffer(&__cc_tpl_85, "));\n      if (!v", 16, __cc_tpl_arena_85); cc__string_slot_push(&__cc_tpl_85, (k), __cc_tpl_arena_85); cc_string_push_buffer(&__cc_tpl_85, ") goto ", 7, __cc_tpl_arena_85); cc__string_slot_push(&__cc_tpl_85, (fail), __cc_tpl_arena_85); cc_string_push_buffer(&__cc_tpl_85, ";\n      for (long long i", 24, __cc_tpl_arena_85); cc__string_slot_push(&__cc_tpl_85, (k), __cc_tpl_arena_85); cc_string_push_buffer(&__cc_tpl_85, " = 0; i", 7, __cc_tpl_arena_85); cc__string_slot_push(&__cc_tpl_85, (k), __cc_tpl_arena_85); cc_string_push_buffer(&__cc_tpl_85, " < C", 4, __cc_tpl_arena_85); cc__string_slot_push(&__cc_tpl_85, (k), __cc_tpl_arena_85); cc_string_push_buffer(&__cc_tpl_85, "; i", 3, __cc_tpl_arena_85); cc__string_slot_push(&__cc_tpl_85, (k), __cc_tpl_arena_85); cc_string_push_buffer(&__cc_tpl_85, "++)\n          if (!", 19, __cc_tpl_arena_85); cc__string_slot_push(&__cc_tpl_85, (T), __cc_tpl_arena_85); cc_string_push_buffer(&__cc_tpl_85, "__fill(s, n, &p, arena, &v", 26, __cc_tpl_arena_85); cc__string_slot_push(&__cc_tpl_85, (k), __cc_tpl_arena_85); cc_string_push_buffer(&__cc_tpl_85, "[i", 2, __cc_tpl_arena_85); cc__string_slot_push(&__cc_tpl_85, (k), __cc_tpl_arena_85); cc_string_push_buffer(&__cc_tpl_85, "], cc_inc, cc_depth + 1)) goto ", 31, __cc_tpl_arena_85); cc__string_slot_push(&__cc_tpl_85, (fail), __cc_tpl_arena_85); cc_string_push_buffer(&__cc_tpl_85, ";\n      out->", 13, __cc_tpl_arena_85); cc__string_slot_push(&__cc_tpl_85, (pfx), __cc_tpl_arena_85); cc__string_slot_push(&__cc_tpl_85, (field), __cc_tpl_arena_85); cc_string_push_buffer(&__cc_tpl_85, " = v", 4, __cc_tpl_arena_85); cc__string_slot_push(&__cc_tpl_85, (k), __cc_tpl_arena_85); cc_string_push_buffer(&__cc_tpl_85, "; out->", 7, __cc_tpl_arena_85); cc__string_slot_push(&__cc_tpl_85, (pfx), __cc_tpl_arena_85); cc__string_slot_push(&__cc_tpl_85, (field), __cc_tpl_arena_85); cc_string_push_buffer(&__cc_tpl_85, "_n = (size_t)C", 14, __cc_tpl_arena_85); cc__string_slot_push(&__cc_tpl_85, (k), __cc_tpl_arena_85); cc_string_push_buffer(&__cc_tpl_85, "; }\n", 4, __cc_tpl_arena_85); __cc_tpl_85; });
}

/* growable element-vector pieces, shared by the delimited items form and
 * the narrowed list form (the drivers interleave pads between them) */
static inline CCString cc_gr_lvec_head_text(CCArena* a, int k, const char* T, const char* fail) {
    return ({ CCArena* __cc_tpl_arena_86 = (a); CCString __cc_tpl_86 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_86, "    { size_t cap", 16, __cc_tpl_arena_86); cc__string_slot_push(&__cc_tpl_86, (k), __cc_tpl_arena_86); cc_string_push_buffer(&__cc_tpl_86, " = 8, cnt", 9, __cc_tpl_arena_86); cc__string_slot_push(&__cc_tpl_86, (k), __cc_tpl_arena_86); cc_string_push_buffer(&__cc_tpl_86, " = 0;\n    ", 10, __cc_tpl_arena_86); cc__string_slot_push(&__cc_tpl_86, (T), __cc_tpl_arena_86); cc_string_push_buffer(&__cc_tpl_86, "* v", 3, __cc_tpl_arena_86); cc__string_slot_push(&__cc_tpl_86, (k), __cc_tpl_arena_86); cc_string_push_buffer(&__cc_tpl_86, " = (", 4, __cc_tpl_arena_86); cc__string_slot_push(&__cc_tpl_86, (T), __cc_tpl_arena_86); cc_string_push_buffer(&__cc_tpl_86, "*)cc_arena_alloc_local(arena, cap", 33, __cc_tpl_arena_86); cc__string_slot_push(&__cc_tpl_86, (k), __cc_tpl_arena_86); cc_string_push_buffer(&__cc_tpl_86, " * sizeof(", 10, __cc_tpl_arena_86); cc__string_slot_push(&__cc_tpl_86, (T), __cc_tpl_arena_86); cc_string_push_buffer(&__cc_tpl_86, "), _Alignof(", 12, __cc_tpl_arena_86); cc__string_slot_push(&__cc_tpl_86, (T), __cc_tpl_arena_86); cc_string_push_buffer(&__cc_tpl_86, "));\n    if (!v", 14, __cc_tpl_arena_86); cc__string_slot_push(&__cc_tpl_86, (k), __cc_tpl_arena_86); cc_string_push_buffer(&__cc_tpl_86, ") goto ", 7, __cc_tpl_arena_86); cc__string_slot_push(&__cc_tpl_86, (fail), __cc_tpl_arena_86); cc_string_push_buffer(&__cc_tpl_86, ";\n", 2, __cc_tpl_arena_86); __cc_tpl_86; });
}

static inline CCString cc_gr_byte_check_text(CCArena* a, int b, const char* fail) {
    return ({ CCArena* __cc_tpl_arena_87 = (a); CCString __cc_tpl_87 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_87, "    if (!(p < n && s[p] == ", 27, __cc_tpl_arena_87); cc__string_slot_push(&__cc_tpl_87, (b), __cc_tpl_arena_87); cc_string_push_buffer(&__cc_tpl_87, ")) goto ", 8, __cc_tpl_arena_87); cc__string_slot_push(&__cc_tpl_87, (fail), __cc_tpl_arena_87); cc_string_push_buffer(&__cc_tpl_87, ";\n    p++;\n", 11, __cc_tpl_arena_87); __cc_tpl_87; });
}

static inline CCString cc_gr_loop_open_text(CCArena* a, int b) {
    return ({ CCArena* __cc_tpl_arena_88 = (a); CCString __cc_tpl_88 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_88, "    if (p < n && s[p] != ", 25, __cc_tpl_arena_88); cc__string_slot_push(&__cc_tpl_88, (b), __cc_tpl_arena_88); cc_string_push_buffer(&__cc_tpl_88, ") {\n    for (;;) {\n", 19, __cc_tpl_arena_88); __cc_tpl_88; });
}

static inline CCString cc_gr_lvec_grow_text(CCArena* a, int k, const char* T, const char* fail) {
    return ({ CCArena* __cc_tpl_arena_89 = (a); CCString __cc_tpl_89 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_89, "    if (cnt", 11, __cc_tpl_arena_89); cc__string_slot_push(&__cc_tpl_89, (k), __cc_tpl_arena_89); cc_string_push_buffer(&__cc_tpl_89, " == cap", 7, __cc_tpl_arena_89); cc__string_slot_push(&__cc_tpl_89, (k), __cc_tpl_arena_89); cc_string_push_buffer(&__cc_tpl_89, ") {\n        ", 12, __cc_tpl_arena_89); cc__string_slot_push(&__cc_tpl_89, (T), __cc_tpl_arena_89); cc_string_push_buffer(&__cc_tpl_89, "* nv", 4, __cc_tpl_arena_89); cc__string_slot_push(&__cc_tpl_89, (k), __cc_tpl_arena_89); cc_string_push_buffer(&__cc_tpl_89, " = (", 4, __cc_tpl_arena_89); cc__string_slot_push(&__cc_tpl_89, (T), __cc_tpl_arena_89); cc_string_push_buffer(&__cc_tpl_89, "*)cc_arena_realloc(arena, arena, v", 34, __cc_tpl_arena_89); cc__string_slot_push(&__cc_tpl_89, (k), __cc_tpl_arena_89); cc_string_push_buffer(&__cc_tpl_89, ",\n            cap", 17, __cc_tpl_arena_89); cc__string_slot_push(&__cc_tpl_89, (k), __cc_tpl_arena_89); cc_string_push_buffer(&__cc_tpl_89, " * sizeof(", 10, __cc_tpl_arena_89); cc__string_slot_push(&__cc_tpl_89, (T), __cc_tpl_arena_89); cc_string_push_buffer(&__cc_tpl_89, "), cap", 6, __cc_tpl_arena_89); cc__string_slot_push(&__cc_tpl_89, (k), __cc_tpl_arena_89); cc_string_push_buffer(&__cc_tpl_89, " * 2 * sizeof(", 14, __cc_tpl_arena_89); cc__string_slot_push(&__cc_tpl_89, (T), __cc_tpl_arena_89); cc_string_push_buffer(&__cc_tpl_89, "), _Alignof(", 12, __cc_tpl_arena_89); cc__string_slot_push(&__cc_tpl_89, (T), __cc_tpl_arena_89); cc_string_push_buffer(&__cc_tpl_89, "));\n        if (!nv", 19, __cc_tpl_arena_89); cc__string_slot_push(&__cc_tpl_89, (k), __cc_tpl_arena_89); cc_string_push_buffer(&__cc_tpl_89, ") goto ", 7, __cc_tpl_arena_89); cc__string_slot_push(&__cc_tpl_89, (fail), __cc_tpl_arena_89); cc_string_push_buffer(&__cc_tpl_89, ";\n        v", 11, __cc_tpl_arena_89); cc__string_slot_push(&__cc_tpl_89, (k), __cc_tpl_arena_89); cc_string_push_buffer(&__cc_tpl_89, " = nv", 5, __cc_tpl_arena_89); cc__string_slot_push(&__cc_tpl_89, (k), __cc_tpl_arena_89); cc_string_push_buffer(&__cc_tpl_89, "; cap", 5, __cc_tpl_arena_89); cc__string_slot_push(&__cc_tpl_89, (k), __cc_tpl_arena_89); cc_string_push_buffer(&__cc_tpl_89, " *= 2;\n    }\n", 13, __cc_tpl_arena_89); __cc_tpl_89; });
}

static inline CCString cc_gr_lvec_fill_text(CCArena* a, int k, const char* T, const char* fail) {
    return ({ CCArena* __cc_tpl_arena_90 = (a); CCString __cc_tpl_90 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_90, "    if (!", 9, __cc_tpl_arena_90); cc__string_slot_push(&__cc_tpl_90, (T), __cc_tpl_arena_90); cc_string_push_buffer(&__cc_tpl_90, "__fill(s, n, &p, arena, &v", 26, __cc_tpl_arena_90); cc__string_slot_push(&__cc_tpl_90, (k), __cc_tpl_arena_90); cc_string_push_buffer(&__cc_tpl_90, "[cnt", 4, __cc_tpl_arena_90); cc__string_slot_push(&__cc_tpl_90, (k), __cc_tpl_arena_90); cc_string_push_buffer(&__cc_tpl_90, "], cc_inc, cc_depth + 1)) goto ", 31, __cc_tpl_arena_90); cc__string_slot_push(&__cc_tpl_90, (fail), __cc_tpl_arena_90); cc_string_push_buffer(&__cc_tpl_90, ";\n    cnt", 9, __cc_tpl_arena_90); cc__string_slot_push(&__cc_tpl_90, (k), __cc_tpl_arena_90); cc_string_push_buffer(&__cc_tpl_90, "++;\n", 4, __cc_tpl_arena_90); __cc_tpl_90; });
}

/* separator handling: the narrowed form has no pads inside the sep arm
 * (one piece); the delimited items form pads after the sep (split) */
static inline CCString cc_gr_lvec_sep_close_text(CCArena* a, int b) {
    return ({ CCArena* __cc_tpl_arena_91 = (a); CCString __cc_tpl_91 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_91, "    if (p < n && s[p] == ", 25, __cc_tpl_arena_91); cc__string_slot_push(&__cc_tpl_91, (b), __cc_tpl_arena_91); cc_string_push_buffer(&__cc_tpl_91, ") { p++; continue; }\n    break;\n    }\n    }\n", 44, __cc_tpl_arena_91); __cc_tpl_91; });
}

static inline CCString cc_gr_lvec_sep_open_text(CCArena* a, int b) {
    return ({ CCArena* __cc_tpl_arena_92 = (a); CCString __cc_tpl_92 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_92, "    if (p < n && s[p] == ", 25, __cc_tpl_arena_92); cc__string_slot_push(&__cc_tpl_92, (b), __cc_tpl_arena_92); cc_string_push_buffer(&__cc_tpl_92, ") { p++;\n", 9, __cc_tpl_arena_92); __cc_tpl_92; });
}

static inline CCString cc_gr_lvec_loop_close_text(CCArena* a) {
    return ({ CCArena* __cc_tpl_arena_93 = (a); CCString __cc_tpl_93 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_93, "    continue; }\n    break;\n    }\n    }\n", 39, __cc_tpl_arena_93); __cc_tpl_93; });
}

static inline CCString cc_gr_lvec_store_text(CCArena* a, int k,
                                             const char* pfx, const char* field) {
    return ({ CCArena* __cc_tpl_arena_94 = (a); CCString __cc_tpl_94 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_94, "    out->", 9, __cc_tpl_arena_94); cc__string_slot_push(&__cc_tpl_94, (pfx), __cc_tpl_arena_94); cc__string_slot_push(&__cc_tpl_94, (field), __cc_tpl_arena_94); cc_string_push_buffer(&__cc_tpl_94, " = v", 4, __cc_tpl_arena_94); cc__string_slot_push(&__cc_tpl_94, (k), __cc_tpl_arena_94); cc_string_push_buffer(&__cc_tpl_94, "; out->", 7, __cc_tpl_arena_94); cc__string_slot_push(&__cc_tpl_94, (pfx), __cc_tpl_arena_94); cc__string_slot_push(&__cc_tpl_94, (field), __cc_tpl_arena_94); cc_string_push_buffer(&__cc_tpl_94, "_n = cnt", 8, __cc_tpl_arena_94); cc__string_slot_push(&__cc_tpl_94, (k), __cc_tpl_arena_94); cc_string_push_buffer(&__cc_tpl_94, "; }\n", 4, __cc_tpl_arena_94); __cc_tpl_94; });
}

static inline CCString cc_gr_presence_text(CCArena* a, int bit) {
    return ({ CCArena* __cc_tpl_arena_95 = (a); CCString __cc_tpl_95 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_95, "    out->cc__set |= 1ULL << ", 28, __cc_tpl_arena_95); cc__string_slot_push(&__cc_tpl_95, (bit), __cc_tpl_arena_95); cc_string_push_buffer(&__cc_tpl_95, ";\n", 2, __cc_tpl_arena_95); __cc_tpl_95; });
}

/* literal terms: bounds-driven failure = INCOMPLETE, content = malformed */
static inline CCString cc_gr_lit1_term_text(CCArena* a, int b, const char* fail) {
    CCString c = cc_gr_chr_text(a, b);
    return ({ CCArena* __cc_tpl_arena_96 = (a); CCString __cc_tpl_96 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_96, "    if (p >= n) { *cc_inc = 1; goto ", 36, __cc_tpl_arena_96); cc__string_slot_push(&__cc_tpl_96, (fail), __cc_tpl_arena_96); cc_string_push_buffer(&__cc_tpl_96, "; }\n    if (s[p] != ", 20, __cc_tpl_arena_96); cc__string_slot_push(&__cc_tpl_96, (b), __cc_tpl_arena_96); cc_string_push_buffer(&__cc_tpl_96, " /*", 3, __cc_tpl_arena_96); cc__string_slot_push(&__cc_tpl_96, (c), __cc_tpl_arena_96); cc_string_push_buffer(&__cc_tpl_96, "*/) goto ", 9, __cc_tpl_arena_96); cc__string_slot_push(&__cc_tpl_96, (fail), __cc_tpl_arena_96); cc_string_push_buffer(&__cc_tpl_96, ";\n    p++;\n", 11, __cc_tpl_arena_96); __cc_tpl_96; });
}

static inline CCString cc_gr_litn_term_text(CCArena* a, const unsigned char* lit, int len,
                                            const char* fail) {
    CCString esc = cc_gr_esc_text(a, lit, len);
    return ({ CCArena* __cc_tpl_arena_97 = (a); CCString __cc_tpl_97 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_97, "    if (p + ", 12, __cc_tpl_arena_97); cc__string_slot_push(&__cc_tpl_97, (len), __cc_tpl_arena_97); cc_string_push_buffer(&__cc_tpl_97, " > n) { *cc_inc = 1; goto ", 26, __cc_tpl_arena_97); cc__string_slot_push(&__cc_tpl_97, (fail), __cc_tpl_arena_97); cc_string_push_buffer(&__cc_tpl_97, "; }\n    if (memcmp(s + p, \"", 27, __cc_tpl_arena_97); cc__string_slot_push(&__cc_tpl_97, (esc), __cc_tpl_arena_97); cc_string_push_buffer(&__cc_tpl_97, "\", ", 3, __cc_tpl_arena_97); cc__string_slot_push(&__cc_tpl_97, (len), __cc_tpl_arena_97); cc_string_push_buffer(&__cc_tpl_97, ") != 0) goto ", 13, __cc_tpl_arena_97); cc__string_slot_push(&__cc_tpl_97, (fail), __cc_tpl_arena_97); cc_string_push_buffer(&__cc_tpl_97, ";\n    p += ", 11, __cc_tpl_arena_97); cc__string_slot_push(&__cc_tpl_97, (len), __cc_tpl_arena_97); cc_string_push_buffer(&__cc_tpl_97, ";\n", 2, __cc_tpl_arena_97); __cc_tpl_97; });
}

/* ---- key-dispatch pieces, shared by the delimited fields form and the
 * narrowed member-list form: capture the key span, switch on length,
 * memcmp chain within a length class, unknown keys to the Ld arm ---- */
static inline CCString cc_gr_key_span_text(CCArena* a, int k, const char* g,
                                           const char* r, const char* fail) {
    return ({ CCArena* __cc_tpl_arena_98 = (a); CCString __cc_tpl_98 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_98, "    { size_t xa", 15, __cc_tpl_arena_98); cc__string_slot_push(&__cc_tpl_98, (k), __cc_tpl_arena_98); cc_string_push_buffer(&__cc_tpl_98, ", xb", 4, __cc_tpl_arena_98); cc__string_slot_push(&__cc_tpl_98, (k), __cc_tpl_arena_98); cc_string_push_buffer(&__cc_tpl_98, "; int xd", 8, __cc_tpl_arena_98); cc__string_slot_push(&__cc_tpl_98, (k), __cc_tpl_arena_98); cc_string_push_buffer(&__cc_tpl_98, ";\n      if (!", 13, __cc_tpl_arena_98); cc__string_slot_push(&__cc_tpl_98, (g), __cc_tpl_arena_98); cc_string_push_buffer(&__cc_tpl_98, "__x_", 4, __cc_tpl_arena_98); cc__string_slot_push(&__cc_tpl_98, (r), __cc_tpl_arena_98); cc_string_push_buffer(&__cc_tpl_98, "(s, n, &p, &xa", 14, __cc_tpl_arena_98); cc__string_slot_push(&__cc_tpl_98, (k), __cc_tpl_arena_98); cc_string_push_buffer(&__cc_tpl_98, ", &xb", 5, __cc_tpl_arena_98); cc__string_slot_push(&__cc_tpl_98, (k), __cc_tpl_arena_98); cc_string_push_buffer(&__cc_tpl_98, ", &xd", 5, __cc_tpl_arena_98); cc__string_slot_push(&__cc_tpl_98, (k), __cc_tpl_arena_98); cc_string_push_buffer(&__cc_tpl_98, ")) goto ", 8, __cc_tpl_arena_98); cc__string_slot_push(&__cc_tpl_98, (fail), __cc_tpl_arena_98); cc_string_push_buffer(&__cc_tpl_98, ";\n      (void)xd", 16, __cc_tpl_arena_98); cc__string_slot_push(&__cc_tpl_98, (k), __cc_tpl_arena_98); cc_string_push_buffer(&__cc_tpl_98, ";\n", 2, __cc_tpl_arena_98); __cc_tpl_98; });
}

/* member-scope byte check (6-space indent, inside the key block) */
static inline CCString cc_gr_kv_check_text(CCArena* a, int b, const char* fail) {
    return ({ CCArena* __cc_tpl_arena_99 = (a); CCString __cc_tpl_99 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_99, "      if (!(p < n && s[p] == ", 29, __cc_tpl_arena_99); cc__string_slot_push(&__cc_tpl_99, (b), __cc_tpl_arena_99); cc_string_push_buffer(&__cc_tpl_99, ")) goto ", 8, __cc_tpl_arena_99); cc__string_slot_push(&__cc_tpl_99, (fail), __cc_tpl_arena_99); cc_string_push_buffer(&__cc_tpl_99, ";\n      p++;\n", 13, __cc_tpl_arena_99); __cc_tpl_99; });
}

static inline CCString cc_gr_key_switch_open_text(CCArena* a, int k) {
    return ({ CCArena* __cc_tpl_arena_100 = (a); CCString __cc_tpl_100 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_100, "      switch (xb", 16, __cc_tpl_arena_100); cc__string_slot_push(&__cc_tpl_100, (k), __cc_tpl_arena_100); cc_string_push_buffer(&__cc_tpl_100, " - xa", 5, __cc_tpl_arena_100); cc__string_slot_push(&__cc_tpl_100, (k), __cc_tpl_arena_100); cc_string_push_buffer(&__cc_tpl_100, ") {\n", 4, __cc_tpl_arena_100); __cc_tpl_100; });
}

static inline CCString cc_gr_key_case_text(CCArena* a, int len) {
    return ({ CCArena* __cc_tpl_arena_101 = (a); CCString __cc_tpl_101 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_101, "      case ", 11, __cc_tpl_arena_101); cc__string_slot_push(&__cc_tpl_101, (len), __cc_tpl_arena_101); cc_string_push_buffer(&__cc_tpl_101, ":\n", 2, __cc_tpl_arena_101); __cc_tpl_101; });
}

static inline CCString cc_gr_key_memcmp_open_text(CCArena* a, int k,
                                                  const unsigned char* key, int len) {
    CCString esc = cc_gr_esc_text(a, key, len);
    return ({ CCArena* __cc_tpl_arena_102 = (a); CCString __cc_tpl_102 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_102, "        if (memcmp(s + xa", 25, __cc_tpl_arena_102); cc__string_slot_push(&__cc_tpl_102, (k), __cc_tpl_arena_102); cc_string_push_buffer(&__cc_tpl_102, ", \"", 3, __cc_tpl_arena_102); cc__string_slot_push(&__cc_tpl_102, (esc), __cc_tpl_arena_102); cc_string_push_buffer(&__cc_tpl_102, "\", ", 3, __cc_tpl_arena_102); cc__string_slot_push(&__cc_tpl_102, (len), __cc_tpl_arena_102); cc_string_push_buffer(&__cc_tpl_102, ") == 0) {\n", 10, __cc_tpl_arena_102); __cc_tpl_102; });
}

static inline CCString cc_gr_key_break_text(CCArena* a) {
    return ({ CCArena* __cc_tpl_arena_103 = (a); CCString __cc_tpl_103 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_103, "          break; }\n", 19, __cc_tpl_arena_103); __cc_tpl_103; });
}

static inline CCString cc_gr_key_goto_dflt_text(CCArena* a, int k) {
    return ({ CCArena* __cc_tpl_arena_104 = (a); CCString __cc_tpl_104 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_104, "        goto Ld", 15, __cc_tpl_arena_104); cc__string_slot_push(&__cc_tpl_104, (k), __cc_tpl_arena_104); cc_string_push_buffer(&__cc_tpl_104, ";\n", 2, __cc_tpl_arena_104); __cc_tpl_104; });
}

static inline CCString cc_gr_key_default_text(CCArena* a, int k) {
    return ({ CCArena* __cc_tpl_arena_105 = (a); CCString __cc_tpl_105 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_105, "      default: goto Ld", 22, __cc_tpl_arena_105); cc__string_slot_push(&__cc_tpl_105, (k), __cc_tpl_arena_105); cc_string_push_buffer(&__cc_tpl_105, ";\n      }\n      goto Ln", 23, __cc_tpl_arena_105); cc__string_slot_push(&__cc_tpl_105, (k), __cc_tpl_arena_105); cc_string_push_buffer(&__cc_tpl_105, ";\nLd", 4, __cc_tpl_arena_105); cc__string_slot_push(&__cc_tpl_105, (k), __cc_tpl_arena_105); cc_string_push_buffer(&__cc_tpl_105, ":\n", 2, __cc_tpl_arena_105); __cc_tpl_105; });
}

/* the Ld arm: skip through the grammar's value/else rule, or hard-fail */
static inline CCString cc_gr_pad_call6_text(CCArena* a, const char* g, const char* cls,
                                            const char* r, const char* fail) {
    return ({ CCArena* __cc_tpl_arena_106 = (a); CCString __cc_tpl_106 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_106, "      if (!", 11, __cc_tpl_arena_106); cc__string_slot_push(&__cc_tpl_106, (g), __cc_tpl_arena_106); cc_string_push_buffer(&__cc_tpl_106, "__", 2, __cc_tpl_arena_106); cc__string_slot_push(&__cc_tpl_106, (cls), __cc_tpl_arena_106); cc_string_push_buffer(&__cc_tpl_106, "_", 1, __cc_tpl_arena_106); cc__string_slot_push(&__cc_tpl_106, (r), __cc_tpl_arena_106); cc_string_push_buffer(&__cc_tpl_106, "(s, n, &p)) goto ", 17, __cc_tpl_arena_106); cc__string_slot_push(&__cc_tpl_106, (fail), __cc_tpl_arena_106); cc_string_push_buffer(&__cc_tpl_106, ";\n", 2, __cc_tpl_arena_106); __cc_tpl_106; });
}

static inline CCString cc_gr_unknown_member_text(CCArena* a, const char* fail) {
    return ({ CCArena* __cc_tpl_arena_107 = (a); CCString __cc_tpl_107 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_107, "      goto ", 11, __cc_tpl_arena_107); cc__string_slot_push(&__cc_tpl_107, (fail), __cc_tpl_arena_107); cc_string_push_buffer(&__cc_tpl_107, ";   /* unknown member and no else rule */\n", 42, __cc_tpl_arena_107); __cc_tpl_107; });
}

static inline CCString cc_gr_key_loop_end_text(CCArena* a, int k) {
    return ({ CCArena* __cc_tpl_arena_108 = (a); CCString __cc_tpl_108 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_108, "Ln", 2, __cc_tpl_arena_108); cc__string_slot_push(&__cc_tpl_108, (k), __cc_tpl_arena_108); cc_string_push_buffer(&__cc_tpl_108, ": ;\n    }\n", 10, __cc_tpl_arena_108); __cc_tpl_108; });
}

/* ---- reflective face: field table, name lookup, compiled get ---- */
static inline CCString cc_gr_ftable_head_text(CCArena* a, const char* n, int cnt) {
    return ({ CCArena* __cc_tpl_arena_109 = (a); CCString __cc_tpl_109 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_109, "static const CCGramField ", 25, __cc_tpl_arena_109); cc__string_slot_push(&__cc_tpl_109, (n), __cc_tpl_arena_109); cc_string_push_buffer(&__cc_tpl_109, "__fields[", 9, __cc_tpl_arena_109); cc__string_slot_push(&__cc_tpl_109, (cnt), __cc_tpl_arena_109); cc_string_push_buffer(&__cc_tpl_109, "] = {\n", 6, __cc_tpl_arena_109); __cc_tpl_109; });
}

static inline CCString cc_gr_ftable_row_items_text(CCArena* a, const char* field,
                                                   const char* gk, const char* n,
                                                   const char* etype) {
    return ({ CCArena* __cc_tpl_arena_110 = (a); CCString __cc_tpl_110 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_110, "    { \"", 7, __cc_tpl_arena_110); cc__string_slot_push(&__cc_tpl_110, (field), __cc_tpl_arena_110); cc_string_push_buffer(&__cc_tpl_110, "\", ", 3, __cc_tpl_arena_110); cc__string_slot_push(&__cc_tpl_110, (gk), __cc_tpl_arena_110); cc_string_push_buffer(&__cc_tpl_110, ", (unsigned)offsetof(", 21, __cc_tpl_arena_110); cc__string_slot_push(&__cc_tpl_110, (n), __cc_tpl_arena_110); cc_string_push_buffer(&__cc_tpl_110, ", ", 2, __cc_tpl_arena_110); cc__string_slot_push(&__cc_tpl_110, (field), __cc_tpl_arena_110); cc_string_push_buffer(&__cc_tpl_110, "), (unsigned)offsetof(", 22, __cc_tpl_arena_110); cc__string_slot_push(&__cc_tpl_110, (n), __cc_tpl_arena_110); cc_string_push_buffer(&__cc_tpl_110, ", ", 2, __cc_tpl_arena_110); cc__string_slot_push(&__cc_tpl_110, (field), __cc_tpl_arena_110); cc_string_push_buffer(&__cc_tpl_110, "_n), \"", 6, __cc_tpl_arena_110); cc__string_slot_push(&__cc_tpl_110, (etype), __cc_tpl_arena_110); cc_string_push_buffer(&__cc_tpl_110, "\" },\n", 5, __cc_tpl_arena_110); __cc_tpl_110; });
}

static inline CCString cc_gr_ftable_row_text(CCArena* a, const char* field,
                                             const char* gk, const char* n) {
    return ({ CCArena* __cc_tpl_arena_111 = (a); CCString __cc_tpl_111 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_111, "    { \"", 7, __cc_tpl_arena_111); cc__string_slot_push(&__cc_tpl_111, (field), __cc_tpl_arena_111); cc_string_push_buffer(&__cc_tpl_111, "\", ", 3, __cc_tpl_arena_111); cc__string_slot_push(&__cc_tpl_111, (gk), __cc_tpl_arena_111); cc_string_push_buffer(&__cc_tpl_111, ", (unsigned)offsetof(", 21, __cc_tpl_arena_111); cc__string_slot_push(&__cc_tpl_111, (n), __cc_tpl_arena_111); cc_string_push_buffer(&__cc_tpl_111, ", ", 2, __cc_tpl_arena_111); cc__string_slot_push(&__cc_tpl_111, (field), __cc_tpl_arena_111); cc_string_push_buffer(&__cc_tpl_111, "), 0, 0 },\n", 11, __cc_tpl_arena_111); __cc_tpl_111; });
}

static inline CCString cc_gr_ftable_empty_row_text(CCArena* a) {
    return ({ CCArena* __cc_tpl_arena_112 = (a); CCString __cc_tpl_112 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_112, "    { 0, 0, 0, 0, 0 },\n", 23, __cc_tpl_arena_112); __cc_tpl_112; });
}

static inline CCString cc_gr_ftable_close_text(CCArena* a) {
    return ({ CCArena* __cc_tpl_arena_113 = (a); CCString __cc_tpl_113 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_113, "};\n", 3, __cc_tpl_arena_113); __cc_tpl_113; });
}

static inline CCString cc_gr_field_fn_head_text(CCArena* a, const char* n) {
    return ({ CCArena* __cc_tpl_arena_114 = (a); CCString __cc_tpl_114 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_114, "static __attribute__((unused)) const CCGramField* ", 50, __cc_tpl_arena_114); cc__string_slot_push(&__cc_tpl_114, (n), __cc_tpl_arena_114); cc_string_push_buffer(&__cc_tpl_114, "_field(const char* k, size_t kl) {\n    switch (kl) {\n", 53, __cc_tpl_arena_114); __cc_tpl_114; });
}

static inline CCString cc_gr_get_case_text(CCArena* a, int len) {
    return ({ CCArena* __cc_tpl_arena_115 = (a); CCString __cc_tpl_115 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_115, "    case ", 9, __cc_tpl_arena_115); cc__string_slot_push(&__cc_tpl_115, (len), __cc_tpl_arena_115); cc_string_push_buffer(&__cc_tpl_115, ":\n", 2, __cc_tpl_arena_115); __cc_tpl_115; });
}

static inline CCString cc_gr_field_hit_text(CCArena* a, const char* field, int len,
                                            const char* n, int j) {
    return ({ CCArena* __cc_tpl_arena_116 = (a); CCString __cc_tpl_116 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_116, "        if (memcmp(k, \"", 23, __cc_tpl_arena_116); cc__string_slot_push(&__cc_tpl_116, (field), __cc_tpl_arena_116); cc_string_push_buffer(&__cc_tpl_116, "\", ", 3, __cc_tpl_arena_116); cc__string_slot_push(&__cc_tpl_116, (len), __cc_tpl_arena_116); cc_string_push_buffer(&__cc_tpl_116, ") == 0) return &", 16, __cc_tpl_arena_116); cc__string_slot_push(&__cc_tpl_116, (n), __cc_tpl_arena_116); cc_string_push_buffer(&__cc_tpl_116, "__fields[", 9, __cc_tpl_arena_116); cc__string_slot_push(&__cc_tpl_116, (j), __cc_tpl_arena_116); cc_string_push_buffer(&__cc_tpl_116, "];\n", 3, __cc_tpl_arena_116); __cc_tpl_116; });
}

static inline CCString cc_gr_get_break_text(CCArena* a) {
    return ({ CCArena* __cc_tpl_arena_117 = (a); CCString __cc_tpl_117 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_117, "        break;\n", 15, __cc_tpl_arena_117); __cc_tpl_117; });
}

static inline CCString cc_gr_switch_end_ret0_text(CCArena* a) {
    return ({ CCArena* __cc_tpl_arena_118 = (a); CCString __cc_tpl_118 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_118, "    }\n    return 0;\n}\n", 22, __cc_tpl_arena_118); __cc_tpl_118; });
}

/* compiled get: dispatch straight to member reads — the field table is
 * the reflective face, this is the fast one */
static inline CCString cc_gr_get_fn_head_text(CCArena* a, const char* n) {
    return ({ CCArena* __cc_tpl_arena_119 = (a); CCString __cc_tpl_119 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_119, "static __attribute__((unused)) int ", 35, __cc_tpl_arena_119); cc__string_slot_push(&__cc_tpl_119, (n), __cc_tpl_arena_119); cc_string_push_buffer(&__cc_tpl_119, "_get(const ", 11, __cc_tpl_arena_119); cc__string_slot_push(&__cc_tpl_119, (n), __cc_tpl_arena_119); cc_string_push_buffer(&__cc_tpl_119, "* v, const char* k, CCGramValue* out) {\n    size_t kl = strlen(k);\n    (void)v;\n    switch (kl) {\n", 98, __cc_tpl_arena_119); __cc_tpl_119; });
}

static inline CCString cc_gr_get_hit_open_text(CCArena* a, const char* field, int len,
                                               const char* gk, const char* n, int j) {
    return ({ CCArena* __cc_tpl_arena_120 = (a); CCString __cc_tpl_120 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_120, "        if (memcmp(k, \"", 23, __cc_tpl_arena_120); cc__string_slot_push(&__cc_tpl_120, (field), __cc_tpl_arena_120); cc_string_push_buffer(&__cc_tpl_120, "\", ", 3, __cc_tpl_arena_120); cc__string_slot_push(&__cc_tpl_120, (len), __cc_tpl_arena_120); cc_string_push_buffer(&__cc_tpl_120, ") == 0) {\n            out->kind = ", 34, __cc_tpl_arena_120); cc__string_slot_push(&__cc_tpl_120, (gk), __cc_tpl_arena_120); cc_string_push_buffer(&__cc_tpl_120, ";\n            out->field = &", 28, __cc_tpl_arena_120); cc__string_slot_push(&__cc_tpl_120, (n), __cc_tpl_arena_120); cc_string_push_buffer(&__cc_tpl_120, "__fields[", 9, __cc_tpl_arena_120); cc__string_slot_push(&__cc_tpl_120, (j), __cc_tpl_arena_120); cc_string_push_buffer(&__cc_tpl_120, "];\n", 3, __cc_tpl_arena_120); __cc_tpl_120; });
}

static inline CCString cc_gr_get_present_text(CCArena* a, int presence, int j) {
    if (presence)
        return ({ CCArena* __cc_tpl_arena_121 = (a); CCString __cc_tpl_121 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_121, "            out->present = (int)((v->cc__set >> ", 48, __cc_tpl_arena_121); cc__string_slot_push(&__cc_tpl_121, (j), __cc_tpl_arena_121); cc_string_push_buffer(&__cc_tpl_121, ") & 1);\n", 8, __cc_tpl_arena_121); __cc_tpl_121; });
    return ({ CCArena* __cc_tpl_arena_122 = (a); CCString __cc_tpl_122 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_122, "            out->present = 1;\n", 30, __cc_tpl_arena_122); __cc_tpl_122; });
}

/* vk: 0 int, 1 float, 2 items, 3 slice */
static inline CCString cc_gr_get_value_text(CCArena* a, int vk, const char* field) {
    if (vk == 0)
        return ({ CCArena* __cc_tpl_arena_123 = (a); CCString __cc_tpl_123 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_123, "            out->i = v->", 24, __cc_tpl_arena_123); cc__string_slot_push(&__cc_tpl_123, (field), __cc_tpl_arena_123); cc_string_push_buffer(&__cc_tpl_123, ";\n", 2, __cc_tpl_arena_123); __cc_tpl_123; });
    if (vk == 1)
        return ({ CCArena* __cc_tpl_arena_124 = (a); CCString __cc_tpl_124 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_124, "            out->f = v->", 24, __cc_tpl_arena_124); cc__string_slot_push(&__cc_tpl_124, (field), __cc_tpl_arena_124); cc_string_push_buffer(&__cc_tpl_124, ";\n", 2, __cc_tpl_arena_124); __cc_tpl_124; });
    if (vk == 2)
        return ({ CCArena* __cc_tpl_arena_125 = (a); CCString __cc_tpl_125 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_125, "            out->items = v->", 28, __cc_tpl_arena_125); cc__string_slot_push(&__cc_tpl_125, (field), __cc_tpl_arena_125); cc_string_push_buffer(&__cc_tpl_125, "; out->items_n = v->", 20, __cc_tpl_arena_125); cc__string_slot_push(&__cc_tpl_125, (field), __cc_tpl_arena_125); cc_string_push_buffer(&__cc_tpl_125, "_n;\n", 4, __cc_tpl_arena_125); __cc_tpl_125; });
    return ({ CCArena* __cc_tpl_arena_126 = (a); CCString __cc_tpl_126 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_126, "            out->s = v->", 24, __cc_tpl_arena_126); cc__string_slot_push(&__cc_tpl_126, (field), __cc_tpl_arena_126); cc_string_push_buffer(&__cc_tpl_126, ";\n", 2, __cc_tpl_arena_126); __cc_tpl_126; });
}

static inline CCString cc_gr_get_hit_close_text(CCArena* a) {
    return ({ CCArena* __cc_tpl_arena_127 = (a); CCString __cc_tpl_127 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_127, "            return 1;\n        }\n", 32, __cc_tpl_arena_127); __cc_tpl_127; });
}

/* ---- tagged-union first-byte dispatch (`one of`) ---- */
static inline CCString cc_gr_union_head_text(CCArena* a, const char* fail) {
    return ({ CCArena* __cc_tpl_arena_128 = (a); CCString __cc_tpl_128 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_128, "    if (p >= n) { *cc_inc = 1; goto ", 36, __cc_tpl_arena_128); cc__string_slot_push(&__cc_tpl_128, (fail), __cc_tpl_arena_128); cc_string_push_buffer(&__cc_tpl_128, "; }\n    if (cc_depth > 128) goto ", 33, __cc_tpl_arena_128); cc__string_slot_push(&__cc_tpl_128, (fail), __cc_tpl_arena_128); cc_string_push_buffer(&__cc_tpl_128, ";\n    switch (s[p]) {\n", 22, __cc_tpl_arena_128); __cc_tpl_128; });
}

static inline CCString cc_gr_union_case_text(CCArena* a, int db,
                                             const char* sname, const char* vname) {
    CCString c = cc_gr_chr_text(a, db);
    return ({ CCArena* __cc_tpl_arena_129 = (a); CCString __cc_tpl_129 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_129, "    case ", 9, __cc_tpl_arena_129); cc__string_slot_push(&__cc_tpl_129, (db), __cc_tpl_arena_129); cc_string_push_buffer(&__cc_tpl_129, ": /*", 4, __cc_tpl_arena_129); cc__string_slot_push(&__cc_tpl_129, (c), __cc_tpl_arena_129); cc_string_push_buffer(&__cc_tpl_129, "*/ {\n        out->kind = ", 25, __cc_tpl_arena_129); cc__string_slot_push(&__cc_tpl_129, (sname), __cc_tpl_arena_129); cc_string_push_buffer(&__cc_tpl_129, "_", 1, __cc_tpl_arena_129); cc__string_slot_push(&__cc_tpl_129, (vname), __cc_tpl_arena_129); cc_string_push_buffer(&__cc_tpl_129, ";\n", 2, __cc_tpl_arena_129); __cc_tpl_129; });
}

static inline CCString cc_gr_union_break_text(CCArena* a) {
    return ({ CCArena* __cc_tpl_arena_130 = (a); CCString __cc_tpl_130 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_130, "        break; }\n", 17, __cc_tpl_arena_130); __cc_tpl_130; });
}

static inline CCString cc_gr_union_default_text(CCArena* a,
                                                const char* sname, const char* vname) {
    return ({ CCArena* __cc_tpl_arena_131 = (a); CCString __cc_tpl_131 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_131, "    default: {\n        out->kind = ", 35, __cc_tpl_arena_131); cc__string_slot_push(&__cc_tpl_131, (sname), __cc_tpl_arena_131); cc_string_push_buffer(&__cc_tpl_131, "_", 1, __cc_tpl_arena_131); cc__string_slot_push(&__cc_tpl_131, (vname), __cc_tpl_arena_131); cc_string_push_buffer(&__cc_tpl_131, ";\n", 2, __cc_tpl_arena_131); __cc_tpl_131; });
}

static inline CCString cc_gr_union_no_default_text(CCArena* a, const char* fail) {
    return ({ CCArena* __cc_tpl_arena_132 = (a); CCString __cc_tpl_132 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_132, "    default: goto ", 18, __cc_tpl_arena_132); cc__string_slot_push(&__cc_tpl_132, (fail), __cc_tpl_arena_132); cc_string_push_buffer(&__cc_tpl_132, ";\n", 2, __cc_tpl_arena_132); __cc_tpl_132; });
}

static inline CCString cc_gr_union_close_text(CCArena* a) {
    return ({ CCArena* __cc_tpl_arena_133 = (a); CCString __cc_tpl_133 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_133, "    }\n", 6, __cc_tpl_arena_133); __cc_tpl_133; });
}

/* ---- matcher-tier substrate: pool/charset tables, prototypes, frames ---- */
static inline CCString cc_gr_pool_table_text(CCArena* a, const char* n,
                                             const unsigned char* pool, int npool) {
    CCString o = cc_string_new();
    cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_134 = (a); CCString __cc_tpl_134 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_134, "static const unsigned char ", 27, __cc_tpl_arena_134); cc__string_slot_push(&__cc_tpl_134, (n), __cc_tpl_arena_134); cc_string_push_buffer(&__cc_tpl_134, "__pool[", 7, __cc_tpl_arena_134); cc__string_slot_push(&__cc_tpl_134, (npool), __cc_tpl_arena_134); cc_string_push_buffer(&__cc_tpl_134, "] = {", 5, __cc_tpl_arena_134); __cc_tpl_134; }), a);
    for (int i = 0; i < npool; i++) {
        int b = (int)pool[i];
        cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_135 = (a); CCString __cc_tpl_135 = cc_string_new(); cc__string_slot_push(&__cc_tpl_135, (b), __cc_tpl_arena_135); cc_string_push_buffer(&__cc_tpl_135, ",", 1, __cc_tpl_arena_135); __cc_tpl_135; }), a);
    }
    cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_136 = (a); CCString __cc_tpl_136 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_136, "};\n", 3, __cc_tpl_arena_136); __cc_tpl_136; }), a);
    return o;
}

/* compact human description of a charset row, for the emitted table.
 * The 128-byte/16-slack truncation guard is part of the output contract. */
static inline CCString cc_gr_cs_desc_text(CCArena* a, const unsigned char* set) {
    CCString o = cc_string_new();
    int c = 0;
    while (c < 256 && o.len + 16 < 128) {
        if (!((set[c >> 3] >> (c & 7)) & 1)) { c++; continue; }
        int d = c;
        while (d + 1 < 256 && ((set[(d + 1) >> 3] >> ((d + 1) & 7)) & 1)) d++;
        if (o.len > 0)
            cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_137 = (a); CCString __cc_tpl_137 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_137, " ", 1, __cc_tpl_arena_137); __cc_tpl_137; }), a);
        cc_gr__cat(&o, cc_gr_chr_text(a, c), a);
        if (d > c) {
            cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_138 = (a); CCString __cc_tpl_138 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_138, "-", 1, __cc_tpl_arena_138); __cc_tpl_138; }), a);
            cc_gr__cat(&o, cc_gr_chr_text(a, d), a);
        }
        c = d + 1;
    }
    return o;
}

static inline CCString cc_gr_cs_head_text(CCArena* a, const char* n, int nsets) {
    return ({ CCArena* __cc_tpl_arena_139 = (a); CCString __cc_tpl_139 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_139, "#if defined(__SSE2__)\n#include <emmintrin.h>\n#endif\nstatic const unsigned char ", 79, __cc_tpl_arena_139); cc__string_slot_push(&__cc_tpl_139, (n), __cc_tpl_arena_139); cc_string_push_buffer(&__cc_tpl_139, "__cs[", 5, __cc_tpl_arena_139); cc__string_slot_push(&__cc_tpl_139, (nsets), __cc_tpl_arena_139); cc_string_push_buffer(&__cc_tpl_139, "][32] = {\n", 10, __cc_tpl_arena_139); __cc_tpl_139; });
}

static inline CCString cc_gr_cs_row_text(CCArena* a, int s, const unsigned char* set) {
    CCString o = cc_string_new();
    CCString desc = cc_gr_cs_desc_text(a, set);
    cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_140 = (a); CCString __cc_tpl_140 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_140, "  /* cs", 7, __cc_tpl_arena_140); cc__string_slot_push(&__cc_tpl_140, (s), __cc_tpl_arena_140); cc_string_push_buffer(&__cc_tpl_140, ": ", 2, __cc_tpl_arena_140); cc__string_slot_push(&__cc_tpl_140, (desc), __cc_tpl_arena_140); cc_string_push_buffer(&__cc_tpl_140, " */\n  {", 7, __cc_tpl_arena_140); __cc_tpl_140; }), a);
    for (int i = 0; i < 32; i++) {
        unsigned int v = set[i];
        cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_141 = (a); CCString __cc_tpl_141 = cc_string_new(); cc__string_slot_push(&__cc_tpl_141, (v), __cc_tpl_arena_141); cc_string_push_buffer(&__cc_tpl_141, ",", 1, __cc_tpl_arena_141); __cc_tpl_141; }), a);
    }
    cc_gr__cat(&o, ({ CCArena* __cc_tpl_arena_142 = (a); CCString __cc_tpl_142 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_142, "},\n", 3, __cc_tpl_arena_142); __cc_tpl_142; }), a);
    return o;
}

static inline CCString cc_gr_matcher_proto_text(CCArena* a, const char* attr, const char* n,
                                                const char* cls, const char* r, const char* suf) {
    return ({ CCArena* __cc_tpl_arena_143 = (a); CCString __cc_tpl_143 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_143, "static ", 7, __cc_tpl_arena_143); cc__string_slot_push(&__cc_tpl_143, (attr), __cc_tpl_arena_143); cc_string_push_buffer(&__cc_tpl_143, "int ", 4, __cc_tpl_arena_143); cc__string_slot_push(&__cc_tpl_143, (n), __cc_tpl_arena_143); cc_string_push_buffer(&__cc_tpl_143, "__", 2, __cc_tpl_arena_143); cc__string_slot_push(&__cc_tpl_143, (cls), __cc_tpl_arena_143); cc_string_push_buffer(&__cc_tpl_143, "_", 1, __cc_tpl_arena_143); cc__string_slot_push(&__cc_tpl_143, (r), __cc_tpl_arena_143); cc__string_slot_push(&__cc_tpl_143, (suf), __cc_tpl_arena_143); cc_string_push_buffer(&__cc_tpl_143, "(const unsigned char* s, size_t n, size_t* io);\n", 48, __cc_tpl_arena_143); __cc_tpl_143; });
}

static inline CCString cc_gr_matcher_head_text(CCArena* a, const char* attr, const char* n,
                                               const char* cls, const char* r, const char* suf) {
    return ({ CCArena* __cc_tpl_arena_144 = (a); CCString __cc_tpl_144 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_144, "static ", 7, __cc_tpl_arena_144); cc__string_slot_push(&__cc_tpl_144, (attr), __cc_tpl_arena_144); cc_string_push_buffer(&__cc_tpl_144, "int ", 4, __cc_tpl_arena_144); cc__string_slot_push(&__cc_tpl_144, (n), __cc_tpl_arena_144); cc_string_push_buffer(&__cc_tpl_144, "__", 2, __cc_tpl_arena_144); cc__string_slot_push(&__cc_tpl_144, (cls), __cc_tpl_arena_144); cc_string_push_buffer(&__cc_tpl_144, "_", 1, __cc_tpl_arena_144); cc__string_slot_push(&__cc_tpl_144, (r), __cc_tpl_arena_144); cc__string_slot_push(&__cc_tpl_144, (suf), __cc_tpl_arena_144); cc_string_push_buffer(&__cc_tpl_144, "(const unsigned char* s, size_t n, size_t* io) {\n    size_t p = *io;\n    (void)s; (void)n;\n", 91, __cc_tpl_arena_144); __cc_tpl_144; });
}

/* padded-form trampoline: lead pad inline, then the __np body */
static inline CCString cc_gr_np_call_text(CCArena* a, const char* n, const char* cls,
                                          const char* r, const char* fail) {
    return ({ CCArena* __cc_tpl_arena_145 = (a); CCString __cc_tpl_145 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_145, "    if (!", 9, __cc_tpl_arena_145); cc__string_slot_push(&__cc_tpl_145, (n), __cc_tpl_arena_145); cc_string_push_buffer(&__cc_tpl_145, "__", 2, __cc_tpl_arena_145); cc__string_slot_push(&__cc_tpl_145, (cls), __cc_tpl_arena_145); cc_string_push_buffer(&__cc_tpl_145, "_", 1, __cc_tpl_arena_145); cc__string_slot_push(&__cc_tpl_145, (r), __cc_tpl_arena_145); cc_string_push_buffer(&__cc_tpl_145, "__np(s, n, &p)) goto ", 21, __cc_tpl_arena_145); cc__string_slot_push(&__cc_tpl_145, (fail), __cc_tpl_arena_145); cc_string_push_buffer(&__cc_tpl_145, ";\n", 2, __cc_tpl_arena_145); __cc_tpl_145; });
}

/* pad-rule call carrying the collect context (build mode) */
static inline CCString cc_gr_pad_call_ctx_text(CCArena* a, const char* n, const char* cls,
                                               const char* r, const char* fail) {
    return ({ CCArena* __cc_tpl_arena_146 = (a); CCString __cc_tpl_146 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_146, "    if (!", 9, __cc_tpl_arena_146); cc__string_slot_push(&__cc_tpl_146, (n), __cc_tpl_arena_146); cc_string_push_buffer(&__cc_tpl_146, "__", 2, __cc_tpl_arena_146); cc__string_slot_push(&__cc_tpl_146, (cls), __cc_tpl_arena_146); cc_string_push_buffer(&__cc_tpl_146, "_", 1, __cc_tpl_arena_146); cc__string_slot_push(&__cc_tpl_146, (r), __cc_tpl_arena_146); cc_string_push_buffer(&__cc_tpl_146, "(c, s, n, &p)) goto ", 20, __cc_tpl_arena_146); cc__string_slot_push(&__cc_tpl_146, (fail), __cc_tpl_arena_146); cc_string_push_buffer(&__cc_tpl_146, ";\n", 2, __cc_tpl_arena_146); __cc_tpl_146; });
}

/* ---- rules-tier match entry ---- */
static inline CCString cc_gr_match_entry_text(CCArena* a, const char* n) {
    return ({ CCArena* __cc_tpl_arena_147 = (a); CCString __cc_tpl_147 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_147, "static int ", 11, __cc_tpl_arena_147); cc__string_slot_push(&__cc_tpl_147, (n), __cc_tpl_arena_147); cc_string_push_buffer(&__cc_tpl_147, "_match(const char* s, size_t n) {\n    size_t p = 0;\n    if (!", 61, __cc_tpl_arena_147); cc__string_slot_push(&__cc_tpl_147, (n), __cc_tpl_arena_147); cc_string_push_buffer(&__cc_tpl_147, "__ENTRY_M((const unsigned char*)s, n, &p)) return 0;\n    return p == n;\n}\n", 74, __cc_tpl_arena_147); __cc_tpl_147; });
}

/* ---- schema streaming face: try_read / read / Reader cursor ----
 * try_read speaks the channel-recv Result contract: Ok(true) frame
 * parsed + pos advanced; Ok(false) clean end AT a frame boundary;
 * Err(WOULD_BLOCK) partial frame, pos unmoved (refill and retry — the
 * caller, who alone knows whether more bytes can exist, escalates to
 * truncation); Err(PARSE) malformed with the evidence in hand.  The
 * Reader is a CURSOR over resources (buffer, position, arena) — all
 * behavior was specialized at compile time; _next returns 0 at clean
 * end AND on parse error, distinguished by _at_end.  Methods are named
 * after the RECEIVER type so instance UFCS resolves by convention.
 * The `!>` in the emitted signature is SURFACE syntax: generated code
 * speaks the language's own idiom (the result rewriter runs after the
 * grammar seam). */
static inline CCString cc_gr_stream_face_text(CCArena* a, const char* n) {
    return ({ CCArena* __cc_tpl_arena_148 = (a); CCString __cc_tpl_148 = cc_string_new(); cc_string_push_buffer(&__cc_tpl_148, "static bool !>(CCError) ", 24, __cc_tpl_arena_148); cc__string_slot_push(&__cc_tpl_148, (n), __cc_tpl_arena_148); cc_string_push_buffer(&__cc_tpl_148, "_try_read(const char* s0, size_t n,\n        size_t* pos, CCArena* arena, ", 73, __cc_tpl_arena_148); cc__string_slot_push(&__cc_tpl_148, (n), __cc_tpl_arena_148); cc_string_push_buffer(&__cc_tpl_148, "* out) {\n    size_t p = *pos;\n    int cc_i0 = 0;\n    if (p >= n) return cc_ok(false);   /* clean end at frame boundary */\n    if (", 130, __cc_tpl_arena_148); cc__string_slot_push(&__cc_tpl_148, (n), __cc_tpl_arena_148); cc_string_push_buffer(&__cc_tpl_148, "__fill((const unsigned char*)s0, n, &p, arena, out, &cc_i0, 0)) {\n        *pos = p;\n        return cc_ok(true);\n    }\n    if (cc_i0) return cc_err(CC_ERROR(CC_ERR_WOULD_BLOCK, \"", 177, __cc_tpl_arena_148); cc__string_slot_push(&__cc_tpl_148, (n), __cc_tpl_arena_148); cc_string_push_buffer(&__cc_tpl_148, ": incomplete frame\"));\n    return cc_err(CC_ERROR(CC_ERR_PARSE, \"", 65, __cc_tpl_arena_148); cc__string_slot_push(&__cc_tpl_148, (n), __cc_tpl_arena_148); cc_string_push_buffer(&__cc_tpl_148, ": malformed input\"));\n}\nstatic int ", 35, __cc_tpl_arena_148); cc__string_slot_push(&__cc_tpl_148, (n), __cc_tpl_arena_148); cc_string_push_buffer(&__cc_tpl_148, "_read(const char* s0, size_t n, size_t* pos, CCArena* arena, ", 61, __cc_tpl_arena_148); cc__string_slot_push(&__cc_tpl_148, (n), __cc_tpl_arena_148); cc_string_push_buffer(&__cc_tpl_148, "* out) {\n    int cc_i0 = 0;\n    return ", 39, __cc_tpl_arena_148); cc__string_slot_push(&__cc_tpl_148, (n), __cc_tpl_arena_148); cc_string_push_buffer(&__cc_tpl_148, "__fill((const unsigned char*)s0, n, pos, arena, out, &cc_i0, 0);\n}\ntypedef struct ", 82, __cc_tpl_arena_148); cc__string_slot_push(&__cc_tpl_148, (n), __cc_tpl_arena_148); cc_string_push_buffer(&__cc_tpl_148, "Reader {\n    const char* s; size_t n; size_t pos; CCArena* arena;\n} ", 68, __cc_tpl_arena_148); cc__string_slot_push(&__cc_tpl_148, (n), __cc_tpl_arena_148); cc_string_push_buffer(&__cc_tpl_148, "Reader;\nstatic ", 15, __cc_tpl_arena_148); cc__string_slot_push(&__cc_tpl_148, (n), __cc_tpl_arena_148); cc_string_push_buffer(&__cc_tpl_148, "Reader ", 7, __cc_tpl_arena_148); cc__string_slot_push(&__cc_tpl_148, (n), __cc_tpl_arena_148); cc_string_push_buffer(&__cc_tpl_148, "_reader(const char* s, size_t n, CCArena* arena) {\n    ", 55, __cc_tpl_arena_148); cc__string_slot_push(&__cc_tpl_148, (n), __cc_tpl_arena_148); cc_string_push_buffer(&__cc_tpl_148, "Reader r; r.s = s; r.n = n; r.pos = 0; r.arena = arena; return r;\n}\nstatic int ", 79, __cc_tpl_arena_148); cc__string_slot_push(&__cc_tpl_148, (n), __cc_tpl_arena_148); cc_string_push_buffer(&__cc_tpl_148, "Reader_next(", 12, __cc_tpl_arena_148); cc__string_slot_push(&__cc_tpl_148, (n), __cc_tpl_arena_148); cc_string_push_buffer(&__cc_tpl_148, "Reader* r, ", 11, __cc_tpl_arena_148); cc__string_slot_push(&__cc_tpl_148, (n), __cc_tpl_arena_148); cc_string_push_buffer(&__cc_tpl_148, "* out) {\n    if (r->pos >= r->n) return 0;\n    int cc_i0 = 0;\n    return ", 73, __cc_tpl_arena_148); cc__string_slot_push(&__cc_tpl_148, (n), __cc_tpl_arena_148); cc_string_push_buffer(&__cc_tpl_148, "__fill((const unsigned char*)r->s, r->n, &r->pos, r->arena, out, &cc_i0, 0);\n}\nstatic int ", 90, __cc_tpl_arena_148); cc__string_slot_push(&__cc_tpl_148, (n), __cc_tpl_arena_148); cc_string_push_buffer(&__cc_tpl_148, "Reader_at_end(const ", 20, __cc_tpl_arena_148); cc__string_slot_push(&__cc_tpl_148, (n), __cc_tpl_arena_148); cc_string_push_buffer(&__cc_tpl_148, "Reader* r) { return r->pos == r->n; }\n", 38, __cc_tpl_arena_148); __cc_tpl_148; });
}

#endif /* CC_GRAMMAR_EMIT_CCH */
