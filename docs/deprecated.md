# Deprecated APIs

Surfaces kept for compatibility. Prefer the replacements in new code.

## Construction: `@create(...)`

**Prefer:** `T name@(args) @destroy;` or `T name@(args) @detach;`

The binder `@(...)` selects the declared type's `.create` hook. Ownership
(`@destroy` / `@detach`) is required on that form.

```c
CCArena a@(megabytes(1)) @destroy;
Vec::[char] out@(&a) @destroy;
```

**Legacy (still accepted):** `T name = @create(args) @destroy;` lowers to
the same hook. New code uses the binder form.

## Type registration: `cc_type_register` / `cc_type_define`

**Prefer:** `@typehooks on Subject { .create = …, .destroy = …, .ufcs = …, };`

```c
@typehooks on MyRes {
    .destroy = cc_type_destroy_call("my_res_close"),
};
```

**Legacy (still accepted):** the same hooks object via an explicit `@comptime`
register / define call. Both spellings are scanned identically; `@typehooks`
in a Concurrent-C TU rewrites to `cc_type_register` before discovery.

```c
@comptime {
    (void)cc_type_register("MyRes", (CCTypeHooks){
        .destroy = cc_type_destroy_call("my_res_close"),
    });
}

@comptime {
    (void)cc_type_define("MyRes", (CCTypeHooks){
        .destroy = cc_type_destroy_call("my_res_close"),
    });
}
```

- Subject rules (exact type, `T*`, trailing-`*` family glob) and hook fields
  match the preferred form. See `spec/draft_typehooks.md` and
  `spec/concurrent-c-spec-complete.md` §9 (type-owned registration).
- Compatibility smokes that keep the legacy call on purpose:
  `tests/comptime_type_register_ufcs_smoke.ccs`,
  `tests/comptime_type_define_smoke.ccs`,
  `tests/comptime_legacy_ufcs_compat_smoke.ccs`,
  `tests/sigils_in_comments_and_strings_smoke.ccs` (string/comment fixtures),
  `tests/header_comptime_backtick_smoke.cch` and
  `tests/comptime_header_type_regs_shared.cch` (legacy `@comptime {
  cc_type_register(...) }` kept on purpose). Quoted project `.cch` now
  lower the same way stdlib headers do; file-scope `@typehooks` /
  `@typeview` belong in the header (`tests/quote_cch_typeview_smoke.ccs`).

## UFCS-only helper: `cc_ufcs_register`

**Prefer:** `@typehooks on Subject { .ufcs = handler, };`

`cc_ufcs_register(pattern, rewrite)` still builds a `CCUfcsRegistration` and
the scanner still accepts `@comptime { (void)cc_ufcs_register(...); }`. New
code puts the rewrite on `.ufcs`. Compatibility smoke:
`tests/comptime_legacy_ufcs_compat_smoke.ccs`.

## UFCS sink field: `.ufcs_dynamic` / `.ufcs_dynamic2`

**Prefer:** `.ufcs_sink`

`.ufcs_dynamic` and `.ufcs_dynamic2` are accepted spellings of the same
destination-aware last-resort sink. New registrations use `.ufcs_sink`.
