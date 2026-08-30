# Is-a faces (`as:`)

Status: draft — surface is `@typeview on T { as: field; }`
(`draft_facets.md`). UFCS-miss retry (including transitive walk),
field-miss retry, arg-position autocast, and `@errhandler` fallback
(exact Result `E`, else unique face path to the handler parameter type).

## 1. Surface

Preferred:

```c
typedef struct CCTempFile {
    CCFile file;
    CCSlice path;
    int owns;
} CCTempFile;

@typeview on CCTempFile {
    as: file;
};
```

- The field name is required; an anonymous face is ill-formed. The lowered
  C member is exactly the source name — no synthetic names.
- A plain `as: field` face is a value embed, or a **single-pointer hop**
  (`as: p` on a named pointer). `T**` and further stars are ill-formed.
  A pointer hop is not a value embed: destroy does not walk it.
  `(Mode)field` may also hop through a pointer (the mode names the
  landing face).
- Layout is ordinary field layout: `sizeof(Type)` at the declaration site
  with `Type`'s alignment. No field flattening.
- At most one face path to any given type from a struct (direct or
  transitive); a second path is ill-formed. Distinct embed types may each
  have one face (`as: file, domain;` when types differ).
- A type-family glob subject (`@typeview on CCSlice_* { as: base; }`)
  applies the same face to every matching concrete type (narrowest match
  wins). See `draft_facets.md`.
- A face grants no access the field name does not already have; it only
  makes the compiler take that path implicitly. Explicit `x.name` and
  `&x.name` remain ordinary member access everywhere.
- Assignment through the field (`x.name = other;`) keeps plain-field
  semantics. Overwriting a lifecycle-managed subobject is the same hazard
  as `memcpy` over it; hook idempotence (§3) is the mitigation.

## 2. UFCS retry and conversion

Method resolution on a receiver of type `Outer` that finds no match retries
each face field in declaration order, resolving the method against the
field's type with receiver `&recv.name`:

```c
tmp.write(buf)      /* lowers to */  cc_file_write(&tmp.file, buf)
w.bump()            /* as: p hop */  WidgetHost_bump(w.p)
```

- Exactly one `as:` field resolving the method: that lowering is taken.
- None: the existing unresolved-method error.
- More than one (distinct `as:` paths both provide the method): ill-formed,
  ambiguous.

A field load or store `x.leaf` (or `x->leaf`) that is not a member of
`Outer` retries the same faces: exactly one landing that has `leaf`
lowers to `x.path.leaf`. A local member wins (`x.leaf` stays `x.leaf`).
`x.leaf()` is UFCS, not this hop. Two faces both offering `leaf` is
ill-formed at the use. Stores inherit the landing facet (`r: *` on
`CCSlice` still denies `xs.len =`). Explicit `x.path` remains ordinary
member access.

Retry walks `as:` fields transitively: after probing `Outer`'s direct embeds,
each embed type's `as:` fields are probed the same way. A cycle in the `as:`
graph is ill-formed at the struct declaration. Method-name clashes across
distinct embed types (same method, different `as:` targets) are ill-formed
at the call. Field-name clashes follow the same rule at the use.

Argument-position conversion applies the same rule to type mismatches: an
`Outer*` (or `Outer` lvalue's address) passed where `T*` is expected, when
`Outer` has a unique `as:` path to `T`, lowers to `&x.path` (e.g.
`&x.mid.file`). The lowering is member selection — emitted C contains no
cast; the host compiler type-checks the selected member. An explicit C cast
keeps C's meaning (no adjustment); a cast from `Outer*` to a `T*` whose
`as:` path is not at offset zero is diagnosed with the member spelling to
use instead.

By-value conversion of `Outer` to `T` is not performed at ordinary call
sites. Handler binding (§5) is the exception.

## 3. Destroy chain

For `Outer x … @destroy;`, cleanup walks the declared type and every
**value** field whose type has a registered destroy or pre-destroy hook,
transitively. Pointer, array, and function-pointer fields are omitted. An
`as:` face is a value embed, so it is in this walk. `x.destroy()` is
ordinary UFCS (`Type_destroy` when that function exists), not this list.

The lowering is a flat call list:

1. Outer registered pre-destroy (if any)
2. Call-site `@destroy { D }` body (if any)
3. Outer registered destroy (if any)
4. Each value field, last declared to first: that type's registered
   pre-destroy, then destroy, on `&x.name` (omitted when the type has no
   hooks), then that type's own value-field chain (same order)

```c
/* CCTempFile tmp = cc_temp_file(&a) !> @destroy; */
__cc_cleanup_1:
    cc_temp_file_unlink(&tmp);
    cc_file_close(&tmp.file);
```

Bodyless `@destroy` is well-formed when this list is non-empty: an outer
hook, or a nested value field that reaches a hook. An empty list is
ill-formed. Two value fields of the same type are both destroyed. No
full-chain symbol is synthesized for `@destroy`.

A cycle in the value-embed graph is ill-formed.

Calling `.destroy()` on a variable also marked `@destroy` runs UFCS and
then the registered chain; hook idempotence covers a `Type_destroy` that
is also the registered hook.

### Hook naming

A registered destroy hook is named for its delta action (`cc_temp_file_unlink`
unlinks; it does not close). The conventional `cc_<type>_destroy` name,
where it exists, always performs the whole chain and is written by hand:

```c
void cc_temp_file_destroy(CCTempFile *t) {
    cc_temp_file_unlink(t);
    cc_file_close(&t->file);
}
```

### Hook idempotence

A callee registered as a destroy hook tolerates a second call on the same
object. `cc_file_close` nulls `handle` and no-ops on a null handle; every
registered hook follows the same shape (guard field, null after release).

## 4. Consumers

`CCTempFile` embeds `CCFile file as:` (`fdopen` of the `mkstemp` fd);
`cc_temp_file_unlink` is the registered delta hook (unlink path, clear
`owns`); `cc_temp_file_destroy` is the hand-written full chain; `CCFile`
registers `cc_file_close` as its destroy hook. The factory remains
`cc_temp_file`.

`CCIoError` embeds `CCError base as:` plus `os_code`. Kind and message live
in `base`; `os_code` is the I/O-only payload. Kind tags share `CCErrorKind`
(`CC_IO_*` names alias the corresponding `CC_ERR_*` values). Constructors
(`cc_io_error_os`, `cc_io_from_errno`) fill `base.message` from the kind
label so an `as:` projection to `CCError` remains printable without the
Io vocabulary. Display prefers a custom message when set (`cc_error_str` /
`cc_io_error_str`).

## 5. Handler dispatch through `as:`

`@errhandler` resolution for an unwrap whose error type is `E`:

1. The nearest in-scope `@errhandler(E)` — exact match — wins, regardless
   of whether a face-typed handler is textually nearer.
2. Otherwise, when `E` has a unique `as:` path to a type `F` with an
   in-scope `@errhandler(F)`, that handler runs with its parameter bound
   to the `F` subobject (`e.path`) — member selection, no conversion
   function, no mapping table.
3. Two `as:` paths from `E` reaching distinct handler types in scope:
   ill-formed, ambiguous (the same rule as §2). Lookup scans every
   in-scope handler before selecting a face; it must not return the
   innermost face when another distinct face is also reachable.
4. The reverse direction never matches: an `@errhandler(E)` where `E` has
   an `as:` field of type `F` does not handle an `F`-typed unwrap.

Handler binding is the one place a by-value `as:` conversion occurs: the
handler receives a copy of the face subobject and sees only the face's
vocabulary. Code that needs the derived payload writes the exact-typed
handler, which wins by rule 1.

A sole `@errhandler(CCError)` — including the script register's injected
default — therefore handles `CCIoError` Results via `base`, while an exact
`@errhandler(CCIoError)` still claims them when `os_code` matters. The
default handler prints `cc_error_str(e)` (kind label when `message` is
null or empty), not a raw `e.message` that may be unset.

Set `CC_DEBUG_ERRHANDLER_AS` to log each successful dispatch as exact,
`as:` (with the dotted path), or ambient `CCError` (Result `E` unresolved).

## 6. Result constructor projection

When constructing or forwarding into a Result whose error face is `F`
(`cc_err(e)` in a `T!>(F)` function, or an assignment/initializer of a
`T!>(F)` value):

1. If `typeof(e)` is `F`, use `e`.
2. Else if there is a unique `as:` embed path from `typeof(e)` to `F`,
   project (`e.path`, e.g. `e.base`).
3. Else the program is ill-formed — no guessing among multiple faces.

Bang binders keep the exact Result error type `E`. Projection happens at
the constructor sink, not at bind time, so `!>(e) { use e.os_code;
return cc_err(e); }` still sees the derived payload inside the binder.
The reverse direction (`F` → richer outer) is never synthesized here;
enrichment stays explicit (`cc_io_error(e)` and peers).

## 7. Out of scope

- Field promotion (`tmp.handle` without naming `file`)
- Virtual dispatch
- Reverse conversion (`T*` → `Outer*`); a later explicit `container_of`
  form may use the same layout facts
- `as:` on non-struct types
- Construction chaining (`name@(args)` through `as:` fields)
- Anonymous `as:` fields; a `@super` spelling
- Synthesized full-chain destroy symbols
