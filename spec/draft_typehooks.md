# Type hooks (`@typehooks`)

Status: draft — preferred surface for type-owned create / destroy / UFCS
registration. The underlying marker API is `cc_type_register` (see
`docs/deprecated.md`).

## 1. Surface

```c
@typehooks on HooksDemo {
    .create = cc_type_create_call("hooks_demo_create"),
    .destroy = cc_type_destroy_call("hooks_demo_destroy"),
};
```

- Body punctuation is a **strict C designated initializer**: leading `.`,
  one RHS expression per arm, commas between arms, trailing comma allowed.
- Multi-overload create uses `cc_type_create_overloads(...)` (or the other
  existing helpers) — not a comma-list of bare callees on `.create`.
- `CC_TYPE_CREATE_DECL("callee")` marks a create overload as declaration-form:
  the lowerer emits `callee(name, args);` (the macro may declare sibling
  storage) instead of `T name = callee(args);`. Decl-form is only valid on a
  value binder, not a pointer dest or expression position.
- Recognized arms match `CCTypeHooks`: `.create`, `.destroy`, `.ufcs`,
  `.ufcs_sink`, and `.niche`. `.ufcs_dynamic` and `.ufcs_dynamic2` are
  accepted spellings of `.ufcs_sink`. `.niche` donates a bit pattern a
  valid instance never exhibits so a `@variant(packed)` arm can carry
  the discriminant (`spec/draft_variants.md`, packed layout).
- Subject may be an exact type (`CCArena`), a pointer key (`MyHandle*`), or
  a trailing-`*` family glob (`CCChanTx_*`), same match/score rule as
  `@typeview` globs.
- Header lowering strips `@typehooks` from host `.h` text. Discovery scans
  `.cch` / TU Concurrent-C source. The form rewrites to
  `@comptime { (void)cc_type_register("Subject", (CCTypeHooks){ … }); }`
  before the marker tables are filled.

## 2. Destroy lowering

Bodyless `@destroy` on a value binding runs the subject's registered
pre-destroy / destroy hooks, then each **value** field whose type has such
a hook, last-declared to first, transitively. Typedef aliases use the base
type's hooks. Pointer, array, and
function-pointer fields are omitted. The list is ill-formed when empty.
`recv.destroy()` is UFCS (`Type_destroy` when that function exists). See
`spec/concurrent-c-spec-complete.md` (declaration destructor) and
`draft_as.md` §3.

## 3. Relation to `@typeview`

`@typeview` declares faces and allow-lists (`as:`, `r:` / `w:` / `rw:`).
`@typehooks` declares lifecycle and UFCS policy. Same `on Subject` shape;
different bodies — do not merge them.

See `tests/typehooks_create_destroy_smoke.ccs` and type-owned registration
in `spec/concurrent-c-spec-complete.md`.
