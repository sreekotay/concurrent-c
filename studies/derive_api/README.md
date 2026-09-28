# The lowered form of derived state

`cc_gen.h` is the plain-C API that any `@derive` syntax would lower to.
It needs no compiler support. `example.ccs` uses it by hand for a text
buffer, a line index patched from an edit log or rebuilt, a derivation
on that derivation, an effect on change, dirty tracking by state
identity, and the execution check. Seed 0.4.0-419:

```
idx: nl=14 rows=10  title="14 lines, 10 rows"
rebuilds=5 patches=2 flushes=7
law (cache == rebuild): holds
after undo: dirty=1
after divergent edit: dirty=1
after redo to saved: dirty=0
```

The counts match the script: a first build, two patches from logged
inserts, rebuilds for an erase, a width change, a lag past the ring,
and a bump with no logged edit; no work on a frame where nothing moved.

## The API

| Piece | Calls | Answers |
|-------|-------|---------|
| `CCGen` | `cc_gen_bump`, `cc_gen_get` | has this source changed since |
| `CCStamp`, `CCStampCheck` | `cc_stamp_check`, `cc_stamp_commit`, `cc_stamp_clear`, `CC_IN(...)` | which inputs moved since the last build, and what they were |
| `CCStateId` | `cc_state_id_new`, `cc_state_id_eq` | is this the same state |
| `CC_GENLOG_DECL(Name, T, N)` | `Name_push`, `Name_since`, `Name_at` | which edits happened since a generation, or rebuild |

One derivation, lowered:

```c
CCStampCheck c = cc_stamp_check(&d->idx_at, CC_IN(cc_gen_get(&d->text.log.gen), d->width));
if (c.stale) {
    ...rebuild, or patch from TextLog_since(&d->text.log, c.prev[0])...
    cc_stamp_commit(&c);        /* only after success */
    cc_gen_bump(&d->idx_gen);   /* for anything derived from idx */
}
```

An effect on change is the same three calls with an effect in the block.

## What writing the example changed

- The first draft of the check result reported only `stale`. The patch
  path then had to read the stamp's array to learn whether `width` had
  moved, and kept its own copy of the text generation it was built at,
  a co-fact inside the API's own example. The check now reports
  `changed` (a bit per input), `prev` (the inputs at the last build),
  and `built`.
- An edit logged as coordinates cannot be patched out when it is an
  erase: the removed bytes are gone. The example rebuilds. The better
  shape is an edit that carries small facts about the removed content,
  computed by the verb when the content is still there, for example the
  number of newlines erased. Values, not views.
- Generations come from one never-repeating counter, so a whole-object
  copy carries a meaningful generation: restoring a full snapshot
  restores content and generation together.

## Open

- `cc_gen_next` uses a C11 atomic. TCC's atomics on the ILP32 bootstrap
  path need the runtime's `cc_atomic` helpers or a per-thread counter
  with a thread index in the top bits.
- `CC_STAMP_MAX` is 4 inputs; a derivation with more is split.
- Nothing here checks that a body reads only its listed inputs, that a
  source's verbs bump its generation, or that the cached field is read
  only through its accessor. Those are the compiler's part, and the
  syntax question is how much of this text it should also write.

## Second pass: the same thing as generic types

`cc_source.cch` defines `Source::[T]` and `Derived::[T]` with
`CC_GENERIC_FACTORY`, so they are ordinary CC generics with methods:

| Type | Methods |
|------|---------|
| `Source::[T]` | `set(v)` stores and bumps; `mut()` returns `T*` and bumps; `val()`, `ref()`, `gen()` |
| `Derived::[T]` | `check(cc_inputs(...))` returns a check whose `.slot` is the writable value only when stale; `commit(&k)` records inputs and bumps; `ref()`, `gen()`, `invalidate()` |

```c
typedef struct {
    Source::[size_t]     width;
    Derived::[LineIndex] idx;
} Doc;

Derived_LineIndex_Check k = d->idx.check(cc_inputs(d->text.gen(), d->width.gen()));
if (k.stale) { ...write *k.slot...; d->idx.commit(&k); }
return d->idx.ref();

d->width.set(4);          /* the bump is the call */
```

`example_family.ccs` runs on 0.4.0-419 (`rows=10 rebuilds=2`).
`example_generic.ccs` is the full document example with the types
written as C macros and called directly; it also carries erased-newline
counts in the edit, so erases patch instead of rebuilding
(`rebuilds=3 patches=3 flushes=6`, law holds).

What the prototype found:

- **Every write is a visible call and every byte is in the type.** There
  is no hidden bump, no hidden field, and no read that silently
  rebuilds. The only writable access to a derived value is `k.slot`
  from a stale check, so "a store to a derived value is refused" falls
  out of the API's shape.
- **The check's type is a mangled name the user must spell**,
  `Derived_LineIndex_Check`. At this seed `@auto` exists only as the
  grower shorthand `@auto(src) name(arena) @destroy;`; a bare
  `@auto k = …` is an unknown word and plain `auto` parses as the C
  storage class. Declaration inference is the general feature this wants.
- **The fence does not reach factory-generated types.**
  `fence_on_generic_probe.ccs`: `@typeview on Plain { r: ^value; }`
  refuses `p.value = 1`; the same view on `Source_size_t`, by exact name
  or by `Source_*`, lets `s.value = 1` through with no diagnostic.
- **C-macro families are invisible to the lowerer**: neither UFCS nor a
  fence resolves on a type made by `#define`. The family has to be a
  factory.
