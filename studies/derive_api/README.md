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
