/* Emit-plan stubs for spike TUs that do not link libshadow_comptime.
 * Included by pp_ast_core.cch. Omitted when SHADOW_HAVE_LIBTCC: the
 * production shadow_lower link always provides strong definitions, and
 * TinyCC does not honor __attribute__((weak)) (reports "defined twice"). */
#ifndef CC_SHADOW_EMIT_PLAN_STUBS_H
#define CC_SHADOW_EMIT_PLAN_STUBS_H
/* From libshadow_comptime (emit_plan). Stubs for spike TUs that do not link
 * the comptime engine. Omitted when SHADOW_HAVE_LIBTCC — the production
 * shadow_lower link always provides strong defs, and TinyCC does not honor
 * __attribute__((weak)) (reports "defined twice"). */
#if !defined(SHADOW_HAVE_LIBTCC)
#if defined(__GNUC__) && !defined(__TINYC__)
#define CC__EMIT_PLAN_STUB __attribute__((weak))
#else
#define CC__EMIT_PLAN_STUB static
#endif
CC__EMIT_PLAN_STUB size_t cc_emit_plan_comptime_fragment_count(void) {
    return 0;
}
CC__EMIT_PLAN_STUB const char* cc_emit_plan_comptime_fragment_text(
    size_t frag_index) {
    (void)frag_index;
    return NULL;
}
CC__EMIT_PLAN_STUB int cc_emit_plan_has_generic_factory(const char* name) {
    (void)name;
    return 0;
}
CC__EMIT_PLAN_STUB int cc_emit_plan_generic_factory_names_csv(char* out,
                                                              size_t cap) {
    if (out && cap) out[0] = 0;
    return 0;
}
#undef CC__EMIT_PLAN_STUB
#endif
#endif
