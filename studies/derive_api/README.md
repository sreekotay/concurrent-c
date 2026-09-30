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
| `CCStamp`, `CCStampCheck` | `cc_stamp_check`, `cc_stamp_commit`, `cc_stamp_prev`, `cc_stamp_clear`, `CC_IN(...)` | which inputs moved since the last build, and what they were |
| `CCStateId` | `cc_state_id_new`, `cc_state_id_eq` | is this the same state |
| `CC_GENLOG_DECL(Name, T, N)` | `Name_push`, `Name_since`, `Name_at` | which edits happened since a generation, or rebuild |

One derivation, lowered:

```c
CCStampCheck c = cc_stamp_check(&d->idx_at, CC_IN(cc_gen_get(&d->text.log.gen), d->width));
if (c.stale) {
    ...rebuild, or patch from TextLog_since(&d->text.log, cc_stamp_prev(&c, 0))...
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
  `changed` (a bit per input), the inputs at the last build
  (`cc_stamp_prev`, read before the commit), and `built`.
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

(The example this pass describes is superseded by the seventh pass
below; `git show 98284b9:studies/derive_api/example_record.ccs` has it.)

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
| keep / clear / set | `UNSET` sentinel, `Option<Option<&str>>` | `@variant Patch { keep; clear; to: char[:]; }` |

`update()` states intent only, and does its one failing step (assigning
settings) first, on a copy. The original assigns `name` and
`custom_styles` to the object before that step, so a failure there left
the object ahead of the database. The flush records the watermark only
after the write succeeds, so a failed write is retried at the next
flush. The database stand-in takes the SET list as bound parameters,
as the Rust version's `Box<dyn ToSql>` does. The run:

```
the same name again                      (nothing to write)
a new name and styles                    UPDATE record SET name = ?1, custom_styles = ?2   ['beta', 'bold']
a font                                   UPDATE record SET settings = ?1   ['{"theme":"dark","font":"mono"}']
the same font again                      (nothing to write)
a name with a bad setting                update refused: tab_width is not a number; name is still 'beta'
clear the styles, and the write fails    write failed: disk full; the row stays unwritten
the next flush                           UPDATE record SET custom_styles = ?1   [NULL]
settings JSON built 1 time(s)
```

What it found:

- **The precise stamp belongs in the declaration.** `Source::[T, eq]`
  makes `set()` compare, once for the value, rather than a `set_eq` at
  each call. The factory declares `eq`'s prototype itself, taking the
  values by value (slices and variants are small headers, and a pointer
  prototype met `const char[:]*`, where `const` binds to the element).
- **The column list is still stated twice**: once as the struct's
  fields and once as `Record_row()` with a SET per bit. A comptime pass
  over the struct's `Source_*` fields could write both from the struct
  alone; that is the next thing this example asks for.
- **The in-memory copy is still trusted as the database.** Nothing
  here reads the row's own version, so another writer's change is
  invisible to the watermark; a real store would put the row's version
  (or an `updated_at`) among the inputs it checks before writing.

Writing it in idiomatic CC (`@for`, template strings, arena-kept
`char[:]`, `CCVec`, bound parameters) met five gaps in the language as
it stands, each worked around in the file and worth fixing:

- **A string literal in a `char[:]` struct field is silently empty.**
  `Set a = { .col = "name" };` and `(Set){ .col = "name" }` both leave
  `.col` empty, with no diagnostic; only a `char[:]` variable works
  (`literal_field_probe.ccs`: `decl=[] compound=[] via-local=[name]`).
  The first run of this example printed `SET  = ?1` and a blank name.
  This is the one that returns a wrong value.
- **A typedef of an extent does not walk.** `typedef char[:] Text;` and
  `typedef CCVec::[Setting] Settings;` both lose the `.len` hook, so
  `@for (c in text)` is refused; the example spells the types out.
- **A `CCVec` of structs has no slice view.** `Setting[:] s = v;` works
  for `int` and `char` vecs and emits a call to an undeclared
  `CCVec_Setting_as_slice` for a struct element.
- **`CCVec::[Setting] !>(CCError)` is refused** because its Result is
  generated before the vec instance's type; the example fills a vec the
  caller made (a destination parameter) instead of returning one.
- **A method-shaped name hijacks UFCS.** An equality called `text_eq`
  for `typedef char[:] Text` *is* `Text`'s `eq` method, so `a->eq(b)`
  inside it called itself and spun. That is UFCS as designed; the
  equalities are named `same_*`.

## Seventh pass: a row is one Source

The sixth pass stated the column list twice, needed a same-value
function per column, and a three-arm `Patch` for keep / clear / set.
None of that was new information: the row's struct already says what
the columns are. This pass widens the three nouns already here instead
of adding any:

| Rule | What it widens |
|------|----------------|
| A Source has a stamp per part: a scalar, slice or list is one part, a struct one per field (one level) | `Source::[Row]` (the struct branch of the same factory) |
| `set` takes the arena the value's bytes live on, and copies only the parts that changed onto it | `set(next, arena)`, which says which fields moved (bit i: field i) |
| A watermark is as wide as what it watches | `CCStamp::[Row]`: a `CCStamp` with one slot per field; `cc_stamp_check` / `cc_stamp_commit` take any stamp with `n` and `at[]` |

`CCStampCheck` is unchanged in kind; it holds up to 32 inputs, one bit
of `changed` each, and `prev[]` became `cc_stamp_prev(&c, i)` (read from
the stamp before the commit), so the check does not carry a second copy.

How a part compares and copies follows from its type, which the factory
reads by reflection: a scalar by `==`; `char[:]` by its bytes and
`clone_into`; a struct field by field; a `CCVec::[T]` element by
element; anything else (a `@variant`) by the type's own `T_eq(T*, T)` and
`T_clone_into(T*, CCArena)`, which the instance declares.

The record, now:

```c
typedef struct {
    char[:]          name;
    Styles           custom_styles;
    CCVec::[Setting] settings;
} Row;

typedef struct {
    CCArena        keep;
    Source::[Row]  row;        /* the row, and a stamp per field */
    CCStamp::[Row] written;    /* the row as last written */
} Record;

static void !>(CCError) Record_update(Record* r, Row next) {
    next.check() !>;                     /* a rule on the whole row */
    r->row.set(next, r->keep) !>;
    return cc_ok();
}

static void !>(CCError) Record_flush(Record* r, SqlTable::[Row]* db) {
    CCStampCheck k = r->row.since(&r->written);
    if (!k.stale) return cc_ok();
    db->update(r->row.ref(), k.changed) !>;
    cc_stamp_commit(&k);
    return cc_ok();
}

/* the caller */
Row next = r.row.val();
next.name = "beta";
next.custom_styles = (Styles){ .some = bold };
save(&r, &db, next);
```

Keep is "the same value", which moves no stamp; `Patch`, the `same_*`
functions, `COL_*`, `Record_row()` and the per-bit SET pushes are gone.
"name cannot be NULL" is the type of `name`. The JSON column is no
longer a `Derived`: the watermark already builds it only when the
settings changed and a flush writes them (still one build in the run).
`sql_standin.cch` is the database stand-in: `SqlTable::[Row]` writes
the fields a mask names, by reflection, each through its type's
`T_sql(T*, CCArena)`. The run is the sixth pass's, line for line.

`set()` copies only what changed: after a width edit, the name's bytes
and the list are the ones already on `keep`; after the first set, all
three are copies, not the caller's.

What it found:

- **Reflection refused a struct with a `char[:]` or `CCVec::[T]`
  field**, the whole struct (`cc_reflect_field_count` = -1). Parameter
  lists already rewrote slice sugar before the member grammar; struct
  fields did not, and neither rewrote `Name::[args]`. Both now do: a
  field reflects as `CCSlice name` / `CCVec_Setting settings`, the
  instance names the lowering produces (`preprocess.c`).
- **The index read every template of a factory as declaring every
  instance.** A factory whose branches write different functions, as
  `Source`'s scalar and struct branches do, gave each instance every
  branch's declarations, first one wins: `Source::[size_t].set` read as
  returning a Result, and `example_doc` stopped compiling. The run output
  of a factory that computes is now indexed over those guesses
  (`lower_generics.cch`, `index_impl.cch`); the run arrives with its
  Results lowered, so a `CCResult_V_E` return is read back as `V !>(E)`
  through the same unmangler that reads `CCVec_int`.
- **A factory body compiles on its own.** `@comptime` helper functions
  and file-scope macros are not in its unit, so the struct branch's
  helpers are macros defined inside the body.
- **A generated definition cannot see a Result function it declares.**
  The per-type helpers the factory writes return `bool` with an out
  parameter; only `set` returns a Result.
- **A `@variant` does not reflect in a factory** (the lowered-C reflect
  source is never set), so `Styles` brings its own `eq` and
  `clone_into`. With it, every one of the sixth pass's `same_*`
  functions would be generated.
- **The fence does not follow a field chain.** `r.row.value.width = 9;`
  builds, and so does `h.n.value = 4;` for a `Source::[size_t]` field;
  a store through a binding of the instance or a pointer to it is
  refused (`fence_probe.sh`, `via_field`). That predates this pass, and
  matters more now that a Source is usually a field.
- **`val()` shares the row's lists.** A row from `r.row.val()` holds the
  record's `CCVec`; an in-place edit of it changes the record behind its
  stamps, and `set()` then sees "the same". The example's edit builds a
  new list (`settings_put`). A read-only face on `val()`'s result would
  make that a compile error.
- **The literal gap reaches variant arms.** `(Styles){ .some = "bold" }`
  is empty like a `char[:]` field initializer; assignment
  (`next.name = "beta"`) works.
- `Table` is an existing family name; the stand-in is `SqlTable`.

Left open: a derivation over one field reads its stamp as
`r.row.gen(2)`, a field number; a name would be better. And a Source of
a struct costs 8 bytes per field for the stamps, and each `set()` one
comparison per field (a list compares element by element).

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
