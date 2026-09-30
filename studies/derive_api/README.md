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
- **Withdrawn: "the fence does not reach factory-generated types."**
  It does. The first probe stored to a fenced plain struct first, and
  the checker reports one refusal per unit, so the second store's
  refusal never printed. A view on `Source_size_t`, on `Source_*`, or
  emitted by the factory itself, refuses the store (see the fifth
  pass). The one spelling that did bind nothing was
  `@typeview on Source::[size_t]`; that is fixed in the compiler now.
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
  body because its first parameter is a `Doc *`. The factory's own type
  is fenced the same way, by the view it emits (fifth pass).
- **What the shape does not catch.** A missing `commit()` rebuilds on
  every read: slow, never wrong. An input left out of `cc_inputs` gives a
  stale value: wrong. The second is what the execution check exists
  for: `d.lines.invalidate()`, read again, compare.
- **A pointer from the read ends at the next store to an input.** The
  recipe pass held two pointers across `width.set(4)` and saw the
  rebuilt value through both. That is section 2's borrow rule, and a
  checker applying it to this read would refuse the held pointer.

## Fifth pass: each generic declares its own privacy

The factory writes the view into the code it emits, next to the struct:

```c
typedef struct ${mangled} { ${arg(0)} value; CCGen g; } ${mangled};
@typeview on ${mangled} { r: ^value, ^g; }
```

and for `Derived::[T]`, `{ r: ^value, ^at, ^g; }`. Every instance is then
private without the program writing a view: the only code that touches
the bytes is the instance's own methods, whose first parameter is the
instance, so `set()` and `mut()` are the only stores to a source and both
bump its generation, and nothing outside `stale()` / `commit()` can forge
a stamp. `fence_probe.sh` builds each case on its own: a store to
`s.value` is refused, a read of `s.g` is refused, `set()` and `val()`
build and run.

What it found:

- **The fence was never missing** (the second pass's finding is
  withdrawn above). Two things made it look missing: the checker's one
  refusal per unit, and a `#if` meant to pick a case: CC checks the source
  before the C preprocessor, so every branch is checked and the first
  refusal in any branch is the one reported. Each case is its own build.
- **`@typeview on Source::[size_t]` bound nothing, silently.** A view's
  subject was kept as its source text and matched against the instance's
  name, `Source_size_t`; the generic spelling never matched and nothing
  said so. `cc__hook_subject_text` now gives a subject written
  `Name::[args]` the instance's mangled name (the same recipe that names
  the instance), which fixes `@typehooks on Name::[args]` too.
  `tests/typeview_on_generic_spelling_fail.ccs` covers it; the typeview,
  typehooks, generic and factory slices of the compiler's tests pass
  (one typeview run failed `typeview_as_ufcs_smoke` once and passed on
  two reruns and standalone).

### Two views on one instance

The factory's view is the instance's unnamed view, so a program that also
writes `@typeview on Source_int { … }` (or `on Source::[int]`) has two
views of one name on one type. The rule was already written down: the
narrowest pattern wins, and equal patterns are ill-formed. It was not
enforced, for factories or for two views on a plain struct: whichever the
index read first governed and the other was silently ignored, whether it
loosened or tightened. The compiler now refuses the second, with the
first as its note (for a factory's, the note is at
`<CC_GENERIC_FACTORY(Source) instance Source_int>`). A family glob
(`Source_*`) under the instance's exact view is still the documented
narrowest-wins, and a named view (`@typeview Ro on Source::[int]`) is a
separate facet and still narrows a binding.

Left open: a function the program writes whose first parameter is the
instance counts as the type's own code, so it can store to `value`
without the bump (`poke(Source_int*)` changes the value and keeps the
generation). For a factory's instance, the type's own code could be
exactly the functions the factory emitted.

## Sixth pass: the Ruby / Rust `update`, as one loop

`example_record.ccs` is the `update` from a thread comparing a Ruby and a
Rust version of the same method: three columns, a JSON column derived
from one of them, a keep / clear / set argument, and a partial `UPDATE`.
The original does four jobs in one function; here each is where its fact
is stated once:

| Job | Original | Here |
|-----|----------|------|
| did this column change | `name != @name` before each store | `Source::[Text, Text_eq]`: the precise stamp, chosen at the declaration; `set()` moves it only on a real change |
| keep the JSON in step | `if @settings_json.nil? \|\| updated != original` inside `update` | `Derived::[Json]` read as `r.settings_json()`, built when the flush first needs it |
| which columns to write | `sets << [...]` inside `update` | a watermark with no slot, `CCStamp written`; the per-input changed bits are the SET list |
| keep / clear / set | `UNSET` sentinel, `Option<Option<&str>>` | `@variant Patch { keep; clear; to: Text; }` |

`update()` states intent only, and does its one failing step (assigning
settings) first, on a copy. The original assigns `name` and
`custom_styles` to the object before that step, so a failure there left
the object ahead of the database. The flush records the watermark only
after the write succeeds, so a failed write is retried at the next
flush. The run:

```
the same name again                      (nothing to write)
a new name and styles                    UPDATE record SET name='beta', custom_styles='bold'
a font                                   UPDATE record SET settings='{"theme":"dark","font":"mono"}'
the same font again                      (nothing to write)
a name with a bad setting                update refused; name is still 'beta'
clear the styles, and the write fails    the row stays unwritten
the next flush                           UPDATE record SET custom_styles=NULL
settings JSON built 1 time(s)
```

What it found:

- **The precise stamp belongs in the declaration.** `Source::[T, eq]`
  makes `set()` compare, once for the value, rather than a `set_eq` at
  each call. The factory declares `eq`'s typed prototype itself, so the
  equality can be defined anywhere in the unit.
- **The column list is still stated twice**: once as the struct's
  fields and once as `Record_row()` with the SET clause per bit. A
  comptime pass over the struct's `Source_*` fields could write both
  from the struct alone; that is the next thing this example asks for.
- **The in-memory copy is still trusted as the database.** Nothing
  here reads the row's own version, so another writer's change is
  invisible to the watermark; a real store would put the row's version
  (or an `updated_at`) among the inputs it checks before writing.

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
