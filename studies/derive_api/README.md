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
| `Derived::[T]` | `stale(cc_inputs(...))` returns `T*` to rebuild into, or `NULL` when fresh; `commit()` records those inputs and bumps; `ref()`, `gen()`, `invalidate()` |

```c
typedef struct {
    Source::[size_t]     width;
    Derived::[LineIndex] idx;
} Doc;

LineIndex* out = d->idx.stale(cc_inputs(d->text.gen(), d->width.gen()));
if (out) { ...write *out...; d->idx.commit(); }
return d->idx.ref();

d->width.set(4);          /* the bump is the call */
```

`example_doc.ccs` is the short full example (fourth pass below).
`example_generic.ccs` is the full document example with the types
written as C macros and called directly; it also carries erased-newline
counts in the edit, so erases patch instead of rebuilding
(`rebuilds=3 patches=3 flushes=6`, law holds).

What the prototype found:

- **Every write is a visible call and every byte is in the type.** There
  is no hidden bump, no hidden field, and no read that silently
  rebuilds. The only writable access to a derived value is the pointer
  `stale` returns, so "a store to a derived value is refused" falls
  out of the API's shape.
- **The first version returned a check the user had to spell**,
  `Derived_LineIndex_Check k = …`. At this seed `@auto` exists only as the
  grower shorthand `@auto(src) name(arena) @destroy;`; a bare
  `@auto k = …` is an unknown word and plain `auto` parses as the C
  storage class. Keeping the pending inputs in the value (one `CCInputs`,
  40 bytes) removed the check type: `stale` returns the slot, `commit()`
  takes nothing.
- **The fence does not reach factory-generated types.**
  `fence_on_generic_probe.ccs`: `@typeview on Plain { r: ^value; }`
  refuses `p.value = 1`; the same view on `Source_size_t`, by exact name
  or by `Source_*`, lets `s.value = 1` through with no diagnostic.
- **C-macro families are invisible to the lowerer**: neither UFCS nor a
  fence resolves on a type made by `#define`. The family has to be a
  factory.

## Third pass: the recipe named in the field's type, withdrawn

`Derived::[LineIndex, Doc_idx] idx;` named the function that derives the
field, and `get(holder)` ran it. It worked (`rows 4 4 10 builds=2`) but
the factory sees only its arguments, so `get` took the holder as
`void *` and `d.idx.get(&other)` compiled. Naming the holder,
`Derived::[LineIndex, Doc, Doc_idx]`, typed it, and read oddly: the
recipe is called for you, yet you spell its holder's type, and you pass
the holder at every call although `d.idx` already says which one. Both
redundancies come from putting the connection in the member's type,
which cannot see the struct around it. It also needed the forward
`typedef struct Doc Doc;`: without it the instance is placed after the
struct that needs it, and fails as `unknown type name`.


## Fourth pass: the field and its read share a name

```c
typedef struct {
    Source::[size_t]     text_len, width;
    Derived::[LineIndex] lines;          /* storage; d.lines() is the read */
} Doc;

@typeview on Doc { r: ^lines; }       /* only Doc bodies touch the storage */

static const LineIndex* Doc_lines(Doc* d) {
    LineIndex* out = d->lines.stale(cc_inputs(d->text_len.gen(), d->width.gen()));
    if (out) { ...build *out...; d->lines.commit(); }
    return d->lines.ref();
}

d.width.set(4);
size_t rows = d.lines()->rows;          /* rebuilds once, then fresh */
```

`example_doc.ccs` prints `rows 4 4 10 10 builds=3` on 0.4.0-419: a build,
a fresh read, a rebuild after `width.set(4)`, and a rebuild after a
second `width.set(4)` of the same value, since a store bumps whether or
not the value changed (over-reporting, the safe direction).

What it found:

- **Nothing is typed twice.** The member says what is cached; the method
  of the same name says how, with its inputs on its first line. UFCS
  already resolves `d.lines()` to `Doc_lines(&d)` beside a field named
  `lines`; the two do not collide.
- **The storage is private with no new mechanism.** `r: ^lines` in the
  view refuses `d.lines.ref()` in `main` ("restricted mode '(default)'
  on 'Doc' does not allow field 'lines'"), and `Doc_lines` is a trusted
  body because its first parameter is a `Doc *`. The deny works here
  because `Doc` is a user struct; on the factory's own type it still
  does not (the fence finding above).
- **What the shape does not catch.** A missing `commit()` rebuilds on
  every read: slow, never wrong. An input left out of `cc_inputs` gives a
  stale value: wrong. The second is what the execution check exists
  for: `d.lines.invalidate()`, read again, compare.
- **A pointer from the read ends at the next store to an input.** The
  recipe pass held two pointers across `width.set(4)` and saw the
  rebuilt value through both. That is section 2's borrow rule, and a
  checker applying it to this read would refuse the held pointer.

## Costs measured

`gen_bench.c`, gcc -O2, 4 cores, 50M stores per thread, time per store:

| Bump | 1 thread | 4 threads |
|------|----------|-----------|
| one global atomic (the first `cc_gen_next`) | 7.4 ns | 128 ns |
| per-thread block of 1024 from the global (now) | 0.8 ns | 1.1 ns |
| plain per-object increment | 0.3 ns | 0.4 ns |

The global counter exists so a restored snapshot can never reuse a
number. Per-thread blocks keep that, since every number is still unique
in the process, at about one nanosecond. A plain per-object increment
does not: restore gen 5, store, and gen 6 names a second value.

`sizeof(Derived::[T])` was `sizeof(T) + 88`: a 4-slot stamp (40), the
pending inputs (40), and the generation (8). `stale()` now writes the
inputs straight into the stamp with a building mark (the stamp's count's
top bit), and `commit()` clears it; a build that fails part way leaves
the mark, and a marked stamp reads as stale, so the next read builds
again. That is `sizeof(T) + 48`.

What changing the counter found:

- **The first counter was one per translation unit.** `cc_gen_next` was
  `static inline` with a `static` counter inside it, which C gives each
  unit its own copy of. Two units storing to the same source handed it
  the same number: `gen_units_repro.sh` against the first header prints
  "unit A gave 1, unit B gave 1", the under-reporting direction. The
  counter is now one `_Atomic` object defined where `CC_GEN_IMPL` is
  defined, and the examples define it. A CC runtime home for it would
  remove the macro.
- **The mark cannot also detect a cycle.** A read of `d.lines()` from
  inside `Doc_lines` finds the mark set, but so does the read after a
  build that failed; both read as stale. Telling them apart needs a
  second bit set only while the build is on the stack, and clearing it
  on every exit path, which is `@defer`'s job, not the type's.
