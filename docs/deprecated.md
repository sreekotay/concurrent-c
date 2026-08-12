# Deprecated APIs

Surfaces kept for compatibility. Prefer the replacements in new code.

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
  `tests/comptime_header_type_regs_shared.cch` (raw local `.cch` includes that
  are not lower-header’d — file-scope `@typehooks` would reach shadow
  unrewritten; stdlib headers are fine because they lower to stripped `.h`).

## UFCS sink field: `.ufcs_dynamic` / `.ufcs_dynamic2`

**Prefer:** `.ufcs_sink`

`.ufcs_dynamic` and `.ufcs_dynamic2` are accepted spellings of the same
destination-aware last-resort sink. New registrations use `.ufcs_sink`.
