# `@as` fields

Status: draft — not implemented (except: `CCFile` registers `cc_file_close`
as its destroy hook).

## 1. Surface

`Type name @as;` marks a named struct field as the is-a embed for `Type`:
the outer type is usable as `Type` through that member.

```c
typedef struct CCTempFile {
    CCFile file @as;
    CCSlice path;
    int owns;
} CCTempFile;
```

- The name is required; an anonymous `@as` field is ill-formed. The lowered
  C member is exactly the source name — no synthetic names.
- Layout is ordinary field layout: `sizeof(Type)` at the declaration site
  with `Type`'s alignment. No field flattening.
- At most one `@as` field of any given type per struct; a second is
  ill-formed. Distinct types may each have one `@as` field.
- `@as` grants no access the field name does not already have; it only
  makes the compiler take that path implicitly. Explicit `x.name` and
  `&x.name` remain ordinary member access everywhere.
- Assignment through the field (`x.name = other;`) keeps plain-field
  semantics. Overwriting a lifecycle-managed subobject is the same hazard
  as `memcpy` over it; hook idempotence (§3) is the mitigation.

## 2. UFCS retry and conversion

Method resolution on a receiver of type `Outer` that finds no match retries
each `@as` field in declaration order, resolving the method against the
field's type with receiver `&recv.name`:

```c
tmp.write(buf)      /* lowers to */  cc_file_write(&tmp.file, buf)
```

- Exactly one `@as` field resolving the method: that lowering is taken.
- None: the existing unresolved-method error.
- More than one (distinct `@as` types both provide the method): ill-formed,
  ambiguous.

Retry is direct only: `@as` fields of `@as` types are not walked.

Argument-position conversion applies the same rule to type mismatches: an
`Outer*` (or `Outer` lvalue's address) passed where `T*` is expected, when
`Outer` has `T name @as`, lowers to `&x.name`. The lowering is member
selection — emitted C contains no cast; the host compiler type-checks the
selected member. An explicit C cast keeps C's meaning (no adjustment); a
cast from `Outer*` to a `T*` whose `@as` field is not at offset zero is
diagnosed with the member spelling to use instead.

By-value conversion of `Outer` to `T` is not performed.

## 3. Destroy chain

For `Outer x … @destroy;`, cleanup lowers to a flat call list:

1. Outer registered pre-destroy (if any)
2. Call-site `@destroy { D }` body (if any)
3. Outer registered destroy (if any)
4. Each `@as` field, last declared to first: that type's registered
   pre-destroy, then destroy, on `&x.name` (skipped when the type has no
   hooks)

```c
/* CCTempFile tmp = cc_temp_file(&a) !> @destroy;  lowers to */
__cc_cleanup_1:
    cc_temp_file_unlink(&tmp);
    cc_file_close(&tmp.file);
```

Bodyless `@destroy` on an outer type with no registered hook is well-formed
when at least one `@as` field has hooks: the chain is the non-empty
cleanup.

Step 4 calls the field type's registered callee only; it does not expand
that type's own `@as` fields. Delta-only hooks and direct-only chaining are
jointly sound only at depth 1: a type whose `@as` field itself has `@as`
fields leaks the inner layer when every author writes delta-only hooks.
Stacking `@as` types awaits a transitive flatten.

`recv.destroy()` on a type with `@as` fields expands inline to the same
call list, including pre-destroy hooks — literal equivalence with bodyless
`@destroy`. Types without `@as` fields keep the single-callee lowering. No
full-chain symbol is synthesized.

Calling `.destroy()` on a variable also marked `@destroy` runs the chain
twice; hook idempotence covers it (below).

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

## 4. First consumer

`CCTempFile` embeds `CCFile file @as` (`fdopen` of the `mkstemp` fd);
`cc_temp_file_unlink` is the registered delta hook (unlink path, clear
`owns`); `cc_temp_file_destroy` is the hand-written full chain; `CCFile`
registers `cc_file_close` as its destroy hook. The factory remains
`cc_temp_file`.

## 5. Out of scope

- Field promotion (`tmp.handle` without naming `file`)
- Transitive `@as` walk, for UFCS retry and for destroy (the depth-1
  boundary above is the documented limit)
- Virtual dispatch
- Reverse conversion (`T*` → `Outer*`); a later explicit `container_of`
  form may use the same layout facts
- `@as` on non-struct types
- Construction chaining (`@create` through `@as` fields)
- By-value conversion of the outer type to an `@as` type
- Anonymous `@as` fields; a `@super` spelling
- Synthesized full-chain destroy symbols
