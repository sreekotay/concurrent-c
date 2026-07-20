/* CC-AUTHORED EMITTER — the first peel of grammar_rules.c into the
 * language itself.  This file is user CC (@string templates, CCString,
 * arenas), lowered at compiler-build time by the SAME shared passes that
 * lower every user header (lower_headers stage), and compiled into ccc.
 *
 * INVARIANT (do not regress it into a dialect): engine source is user CC.
 * Any construct valid in a .ccs is valid here; a lowering gap is a BUG in
 * the header-lowering convergence, not a boundary to write around.
 *
 * Contract (must emit BYTE-IDENTICAL text to the C emitter it replaces —
 * that identity is the port's regression gate): the run-scanner for stop
 * set {b < T} u {b1} u {b2} as an arch ladder — 16 B SSE2 where the host
 * compiler has it, portable 8 B SWAR otherwise (also what the TCC
 * front-end parses).  On a hit every arm advances p to the FIRST stop
 * byte.  No NEON arm by measurement (33% regression on Apple Silicon —
 * vector->GPR lane-extract latency dominates ~11-byte median runs). */
#ifndef CC_GRAMMAR_EMIT_SWAR_CCH
#define CC_GRAMMAR_EMIT_SWAR_CCH

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

#endif /* CC_GRAMMAR_EMIT_SWAR_CCH */
