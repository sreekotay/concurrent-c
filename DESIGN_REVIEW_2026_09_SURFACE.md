# Design review, 2026-09: the proposed surface, consolidated

Working draft. Every piece of syntax proposed in `DESIGN_REVIEW_2026_09.md`,
placed side by side on real specimens, with the overlaps resolved where
the resolution is clear and left open where it is a choice.

## Where each kind of fact lives

The review's proposals put facts in four places. Putting them together
gives one rule for which place a fact belongs in.

| Place | Holds | Why there |
|-------|-------|-----------|
| **the struct declaration** | fields, and any fact that adds bytes: a value's stamp (`Source::[T]`), a consumer's watermark and slot (`Derived::[T]`), an edit ring | layout is one fact per type, the same in every translation unit |
| **the view** (`@typeview`) | admission (`r:`, `w:`, `rw:`), faces (`as:`), field-shaped reads that store nothing (`d:`) | erased; adds no bytes; everyday syntax |
| **hooks** (`@typehooks`) | lifecycle: create, destroy, validate, extent | advanced; how a value comes to be and ends |
| **declaration suffixes** | how this binding behaves: `@destroy`, `@detach` | local, where the logic is |

The earlier section 4 draft put versions and caches in the view (`v:`,
`c:`), which breaks the view's one guarantee, that it adds no bytes, and
a later draft made them member suffixes (`@primary`, `@derive`). Both are
withdrawn for two generics in the struct: stamps and watermarks are
ordinary fields, and each generic declares its own view.

## The surface, in one list

| Spelling | Section | Status |
|----------|---------|--------|
| `CALL !>;` `!>(e) {…}` `@ok(v)` `@err(e)` `?> v` | 1 | shipped |
| `@ok(v)` inside a scope handler | 1.2 | proposed |
| `@defer`, `@defer name:`, `@cancel_defer`, `@defer(ok\|err)` | 1.4 | shipped |
| `name@(args)`, `@destroy`, `@destroy {…}`, `@detach` | 1.4, 2 | shipped |
| `@detach(owner)`: the obligation goes to a named owner | 2.2 item 8, 3.2 item 7 | proposed, one spelling for both |
| a kept parameter attribute | 2.2 item 3 | proposed, spelling open |
| arena-last parameter is the `Alloc` face | 2.2 item 10 | proposed, no syntax |
| `CCParallel h@();` declared empty join set | 3.2 item 1 | proposed |
| `h.drain()`, `h.settled()` | 3.2 items 2, 3 | proposed |
| `@parallel (h) below (n) {…}` as a `bool` | 4.2 item 1 | proposed |
| `@variant` for a tag beside a payload | 4.2 item 5 | shipped construct, new use |
| `d: name` in a view: a field-shaped read that stores nothing | 4.2 item 3 | proposed |
| `Source::[T] f;` a value and its stamp; `set()`, `mut()` the only stores | 4.2 the loop | proposed; the study's factory, today's generics |
| `Derived::[T] f;` a slot and its watermark; `stale(cc_inputs(…))` returns the slot or NULL, `commit()` after the act succeeds | 4.2 the loop | proposed; the same |
| the read is the method named like the field: `d.lines()` beside `Derived::[LineIndex] lines` | 4.2 the loop | a convention; UFCS already resolves it |
| a watermark alone, for consumers that are not a slot (a flush, a dirty flag) | 4.2 the loop | open: a generic, or the lowered `CCStamp` |
| stamp precision: generation, equal-store-skipping, identity, probe | 4.2 the loop | proposed; one choice per value |
| a log of edits since a watermark, for patching instead of rebuilding | 4.4 | deferred: one specimen |
| `.validate` hook; sealed construction | 4.2 item 6 | proposed |
| `&` of a non-writable field refused | 4.2 item 8 | proposed, no syntax |
| `const` on a member read by the lowering as frozen | census | proposed, no new syntax |

Withdrawn along the way: `@since`, `@settle`, `fresh()`, `@version`, `@log(x, e)`, `v:` and `c:`
view groups, `c: idx from …` edge lists, `@primary`, `@derive`,
`@derive_gen`, init-captures on a derivation, `Derived::[T, Recipe]`,
validation on every assignment, a transaction scope, per-instance
validators, a subscriber graph.

## Worked examples

### 1. cctext: a document and its derived state

Today: `edit_gen` bumped at three sites; the highlight and analysis
caches cleared by hand at four edit sites; `last_off` and `last_markup`
as a one-entry edit log with no lag check.

```c
typedef struct {
    Source::[RtxPieceTree] tree;          /* edits go through its verbs, which bump it */
    Source::[size_t]       width;
    size_t                 hl_from, hl_to; /* the visible window, compared by value */
    Derived::[LineIndex]   idx;
    Derived::[Markup]      hl;
} RtxDoc;

@typeview on RtxDoc { r: ^idx, ^hl; }     /* the slots are read through their methods */

static const LineIndex* RtxDoc_idx(RtxDoc* d) {
    LineIndex* out = d->idx.stale(cc_inputs(d->tree.gen(), d->width.gen()));
    if (out) { out->build(d->tree.ref(), d->width.val()); d->idx.commit(); }
    return d->idx.ref();
}

static const Markup* RtxDoc_hl(RtxDoc* d) {
    Markup* out = d->hl.stale(cc_inputs(d->idx.gen(), d->tree.gen(), d->hl_from, d->hl_to));
    if (out) { out->scan(d->tree.ref(), d->idx(), d->hl_from, d->hl_to); d->hl.commit(); }
    return d->hl.ref();
}
```

`draw(d.hl())` settles `hl`, whose inputs name `idx`'s generation, so
`idx` settles first when `hl` rebuilds; the order comes from the reads.
No invalidation call remains at any edit site: the edit verbs store to
`tree`, which moves its stamp. A value such as `hl_from` is its own
stamp. A pointer from `d.hl()` is a borrow that ends at the next store
to an input.

### 2. cctext: the session snapshot, written three times today

`rtx_ws_safe_note` copies 20 fields, `rtx_ws_safe_stale` compares them,
and `rtx_ws_safe_sig` hashes them. It is a flush: a consumer with a
watermark and no slot.

```c
/* the frame loop: act when an input moved, remember after the write */
CCStampCheck k = cc_stamp_check(&b->flushed, CC_IN(b->doc.tree.gen(), b->top.gen(),
                                                   b->left_col.gen(), b->layout.gen()));
if (k.stale) {
    rtx_ws_flush(b) !>;
    cc_stamp_commit(&k);                  /* only after the write succeeded */
}
```

The three hand-kept lists become one input list, and a field added to
the camera is added in one place. The flush wants the precise stamp:
a store of an equal value should not rewrite the session file.

### 3. staticd: a file cache with a probe and a clock

Today: a 1-second revalidation stamp `checked`, a stat copied into
`FileHold`, a separate block cache comparing mtime and size.

```c
static const FileHold* !>(CCIoError) File_hold(File* f) {
    FileHold* out = f->hold.stale(cc_inputs(cc_stat_stamp(f->path), now_s()));
    if (out) {
        *out = file_open_hold(f->path) !>;   /* a failure leaves the watermark unrecorded */
        f->hold.commit();
    }
    return cc_ok(f->hold.ref());
}

/* a request pins the version it serves */
FileHold h = *(f.hold() !>);              /* a plain copy: Content-Length matches the bytes sent */
```

The file system has no stamp to bump, so its stamp is a probe, and the
clock bucket bounds how often the probe runs. Their cost is in the
text. A pinned snapshot is a copy, not a consumer.

### 4. pigz: the rsyncable segments, and the two schedules

Today: `rsync_scan` fills an array sized for the densest hits, drops the
tail when the array fills, and nothing checks that the segments cover
the block. That is the reproduced data loss.

```c
@typehooks on RsyncSegs {
    .create   = rsync_scan,
    .validate = segs_cover_block,         /* sum(segs) == data.len */
};

RsyncSegs segs@(data, &rolling) !>;       /* the ticket fails loudly instead of truncating */
```

The two schedules are already one body, `@parallel seq (use_par) wait
(ts) for …`. The missing piece is the comparison, and it is a harness
flag, not syntax: run with `seq` false and true and compare the output.

### 5. redis: a promised array

Today: `array_len(n)` then n items, with a comment saying a mid-encode
failure truncates the wire. The KEYS count and emit are two copies of
one filter.

```c
RedisArray arr = enc->array(n) !> @destroy;   /* destroy fails the connection if items != n */
@for (i in 0..n) arr.value(&cells[i]) !>;
```

No new syntax: an obligation with a count, discharged by the ledger.

### 6. The std server: session state

Today: `answered` beside an int `act`, `dead` and `closing` set in pairs
at about fifteen sites, and the engine's pending bit folded into the
page's `want_out` against the header's own rule.

```c
@variant CCIoState { open; closing; dead; }

typedef struct {
    CCIoState st;
    bool      want_out;                   /* the page's bit only */
    ...
} CCIoRow;

@typeview on CCIoRow {
    r: *;
    d: interest;                          /* = want_out || pending_out; stores nothing */
};
```

### 7. curl and staticd: a worker set with a floor and a cap

Today: a CAS on `live` with two rollbacks and one missed decrement in
curl; three CAS helpers on `nworkers` in the server.

```c
CCParallel workers@() @detach(q->arena);     /* the arena owns the join set */

if (!@parallel (workers) below (q->max_threads) { worker(q); })
    return CC_CURLE_OK;                       /* already at the cap */

/* inside a worker, on idle */
if (workers.leave_above(q->min_threads)) return;
```

The count is a read of the set, and the condition and the admit are one
operation under the set's own lock.

## Overlaps, and how they resolve

| Overlap | Spellings that competed | Resolution |
|---------|------------------------|------------|
| where an input is stated | `c: idx from pos, len`; a capture list; the body's free names; a recipe type argument | `cc_inputs(…)` on the consumer's first line |
| where a stamp lives | `v:` in the view; hand-declared `pos_v` fields; `@version`; `@primary` | `Source::[T]`, a field in the struct; `uint64_t`, from per-thread blocks of one counter |
| where a cache lives | `c:` in the view; `@derive` suffix | `Derived::[T]`, a field in the struct, read through the method of its name |
| bringing state up to date | `@settle`; `fresh()`; `@derive_gen`; a read | a read; an effect keeps its own watermark |
| deltas | `@since` switch; `@log` in verbs; `log(N)` / `@patch` | a log of edits since a watermark; deferred |
| a derived field vs a method | `d:`; a method | both: `d:` where call sites read a field today |
| a named owner | section 2's annotation; section 3's `h@(a)`; `create_*` | `@detach(owner)` |
| a cap on a set | `@parallel(h, below: n)`; library verbs | `below (n)`, a contextual word like `seq` and `cache` |
| validation | a check on every store; a hook; sealed | the `.validate` hook at construction, and sealed types |
| frozen after construction | a new attribute | `const` members, read by the lowering |

## Open choices

- Whether `@parallel (h) below (n) {…}` returns `bool`, which makes the
  dest-attach an expression; today it is a statement.
- `@detach(owner)` widens `@detach`'s meaning from "the caller" to "this
  owner".
- Whether the watermark alone is a generic (`Seen`) or stays the lowered
  `CCStamp` for flushes and dirty flags.
- The precise stamp's spelling (a `set` that skips an equal store).
- Whether `d:` earns its place, or methods suffice.
