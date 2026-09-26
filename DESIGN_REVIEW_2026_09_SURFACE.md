# Design review, 2026-09: the proposed surface, consolidated

Working draft. Every piece of syntax proposed in `DESIGN_REVIEW_2026_09.md`,
placed side by side on real specimens, with the overlaps resolved where
the resolution is clear and left open where it is a choice.

## Where each kind of fact lives

The review's proposals put facts in four places. Putting them together
gives one rule for which place a fact belongs in.

| Place | Holds | Why there |
|-------|-------|-----------|
| **the struct declaration** | fields, and any fact that adds bytes: a primary's generation, a cached derivation and its stamps, an edit ring | layout is one fact per type, the same in every translation unit |
| **the view** (`@typeview`) | admission (`r:`, `w:`, `rw:`), faces (`as:`), field-shaped reads that store nothing (`d:`) | erased; adds no bytes; everyday syntax |
| **hooks** (`@typehooks`) | lifecycle: create, destroy, validate, extent | advanced; how a value comes to be and ends |
| **declaration suffixes** | how this binding behaves: `@destroy`, `@detach`, `@derive` | local, where the logic is |

The earlier section 4 draft put versions and caches in the view (`v:`,
`c:`). That breaks the view's one guarantee, that it adds no bytes.
Moving them to the struct declaration, as member suffixes, keeps views
erased and keeps nothing new in hooks.

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
| `T f @primary;` member suffix: stores bump the field's generation | 4.2 item 4 | proposed; replaces `v:` and `@version` |
| `T x = expr @derive;` and `T x @derive {…};` | 4.2 derivations | proposed; replaces `c:` and `from` |
| `[name = expr]` init-captures on `@derive` for probes and clocks | 4.2 derivations | proposed; syntax already exists for closures |
| `@derive_gen(x)`: the generation of a primary or a derivation, `uint64_t`; settles a derivation first | 4.2 derivations | proposed; replaces `@settle` and `fresh()` |
| `T f @primary log(N);` and `@patch (T.edit e) {…}` | 4.2 deltas | proposed, deferred: one specimen |
| `.validate` hook; sealed construction | 4.2 item 6 | proposed |
| `&` of a non-writable field refused | 4.2 item 8 | proposed, no syntax |
| `const` on a member read by the lowering as frozen | census | proposed, no new syntax |

Withdrawn along the way: `@since`, `@settle`, `fresh()`, `@version`, `@log(x, e)`, `v:` and `c:`
view groups, `c: idx from …` edge lists, validation on every assignment,
a transaction scope, per-instance validators, a subscriber graph.

## Worked examples

### 1. cctext: a document and its derived state

Today: `edit_gen` bumped at three sites; the highlight and analysis
caches cleared by hand at four edit sites; `last_off` and `last_markup`
as a one-entry edit log with no lag check.

```c
typedef struct {
    RtxPieceTree tree  @primary;          /* a store bumps tree's generation */
    size_t       width @primary;
    size_t       hl_from, hl_to;          /* the visible window */

    LineIndex idx @derive {               /* edges: tree, width */
        idx.build(&tree, width);
    };
    Markup hl @derive {                   /* edges: idx, tree, hl_from, hl_to */
        hl.scan(&tree, &idx, hl_from, hl_to);
    };
} RtxDoc;

@typeview on RtxDoc {
    r: *;                                 /* stores only in RtxDoc* bodies */
    d: line_count;                        /* = rtx_doc_line_count(d): stores nothing */
};
```

Use sites: `draw(&d->hl)` checks `hl`'s stamps, which checks `idx`'s,
and rebuilds what is stale in that order. No invalidation call remains at
any edit site; the edit verbs store to `tree`, which bumps it.

Rules this example fixes:
- A member derivation's body names its siblings directly, and its edges
  are the sibling fields it reads. An edge outside the object means the
  derivation belongs on the object that holds both, or in a local.
- An edge must be a field declared `@primary`, a `const` field, or a
  value compared by value. Reading any other mutable field is refused:
  "edge `hl_from` is not a primary; add `@primary` or make it a value
  capture". Here `hl_from` and `hl_to` are compared by value, since
  their stamp would be the value itself.
- `typeof(d->idx)` is `const LineIndex`. A read settles, then loads; a
  store is refused by C's own `const` rule; the body alone may write it.
  A copy is an ordinary value, which is a pinned snapshot. `&d->idx`
  settles and yields a `const LineIndex *`, a borrow that ends at the
  next store to an edge, which section 2's borrow rule already checks.

With deltas, later and only if the one-specimen case holds up:

```c
    RtxPieceTree tree @primary log(8);
    LineIndex idx @derive { idx.build(&tree, width); }
                  @patch (RtxPieceTree.edit e) {
                      @switch (e) {
                      case .insert(off, len): idx.insert(off, len);
                      case .erase(off, n):    idx.erase(off, n);
                      default:                @rebuild;
                      }
                  };
```

### 2. cctext: the session snapshot, written three times today

`rtx_ws_safe_note` copies 20 fields, `rtx_ws_safe_stale` compares them,
and `rtx_ws_safe_sig` hashes them.

```c
typedef struct {
    RtxDoc     doc;
    size_t     top @primary, left_col @primary, seek_off @primary;
    RtxLayout  layout @primary;
    ...
    RtxSafeCam cam @derive {              /* pure: builds the snapshot, writes nothing else */
        cam = rtx_safe_cam(&doc, top, left_col, seek_off, &layout);
    };
    uint64_t   cam_written;               /* the generation last flushed */
} RtxBuf;

/* the frame loop: the effect is written where it runs */
uint64_t g = @derive_gen(b->cam);         /* settles cam, returns its generation */
if (g != b->cam_written) {
    rtx_ws_flush(&b->cam) !>;
    b->cam_written = g;                   /* recorded only after the flush succeeds */
}
```

A derivation writes only its own cache, so the execution check can
rebuild it from scratch without repeating an effect. An effect on change
is an ordinary `if` on a remembered generation. Early cut-off carries
through: `cam` gets a new generation only when its value changes. The
three hand-kept lists become the body's reads, and `cam_written` is one
word recording a different fact, what was last written.

A generation answers "has this changed since", and may over-report.
It is not a state identity: dirty tracking needs ids that undo
restores, and stays the program's own fact.

### 3. staticd: a file cache with a probe and a clock

Today: a 1-second revalidation stamp `checked`, a stat copied into
`FileHold`, a separate block cache comparing mtime and size.

```c
StatStamp st @derive [sec = now_s()] {    /* at most one stat per second */
    st = cc_stat_stamp(path) !>;
};
FileHold hold @derive {                   /* edge: st */
    hold = file_open_hold(path, st) !>;
};

/* a request pins the version it serves */
FileHold h = c->hold;                     /* a plain copy: Content-Length matches the bytes sent */
```

An init-capture is compared by value on each read, so `[sec = now_s()]`
is a time bucket and a probe is an init-capture of the probe. The clock
and the filesystem have no version to bump; their cost is in the text.
A pinned snapshot is simply not a derivation.

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
| where a dependency edge is stated | `c: idx from pos, len`; a capture list; the body's free names | free names; a list only for init-captures and value compares |
| where a generation lives | `v:` in the view; hand-declared `pos_v` fields; `@version`, `@tracked` | `@primary` on the member; the counter is part of the member's lowering, `uint64_t` so it never wraps on a 32-bit target |
| where a cache lives | `c:` in the view; `@derive` suffix | `@derive` on the member or the local |
| bringing state up to date | `@settle`; `fresh()`; a read | a read; `@derive_gen(x)` where an effect needs to know whether anything changed |
| deltas | `@since` switch; `@log` in verbs | `@primary log(N)` and `@patch`; deferred |
| a derived field vs a method | `d:`; a method | both: `d:` where call sites read a field today |
| a named owner | section 2's annotation; section 3's `h@(a)`; `create_*` | `@detach(owner)` |
| a cap on a set | `@parallel(h, below: n)`; library verbs | `below (n)`, a contextual word like `seq` and `cache` |
| validation | a check on every store; a hook; sealed | the `.validate` hook at construction, and sealed types |
| frozen after construction | a new attribute | `const` members, read by the lowering |

## Open choices

- Bare sibling names inside a member derivation, or `self->`.
- Whether `@parallel (h) below (n) {…}` returns `bool`, which makes the
  dest-attach an expression; today it is a statement.
- `@detach(owner)` widens `@detach`'s meaning from "the caller" to "this
  owner".
- Whether `@derive_gen` stays a compound or a shorter spelling is found.
- Whether `d:` earns its place, or methods suffice.
