# Taxonomy — bug class × CC mechanism

Map root-cause shapes to Concurrent-C seams. Mechanisms are **candidates**,
not automatic absolution. Evaluate **all three families** on every CVE:

1. **Locality / concurrency** — closures, nurseries, `@destroy`, channels, provenance  
2. **SERDES** — `@grammar(rules|schema)`, codecs, truthful borrow/materialize, `cc_parse`/`cc_write`  
3. **Variants** — `@variant` data-model sums (draft), Result as two-arm case, schema `one of`

A CVE can hit more than one family. Primary verdict still uses one label;
cite which family drove it in the rationale.

| Class | Typical failure | CC seams that may help | Often still open |
|-------|-----------------|------------------------|------------------|
| **T1 Teardown vs in-flight** | Worker frees / owner frees while peer uses | Nursery join before `@destroy`; drain-as-join | Raw `free` from a child; foreign allocators |
| **T2 Capture escape** | Stack / request buffer into longer-lived task | Closure capture rules; stack-slice rejection | Raw `T*`; `unsafe` / untracked slices; `&`-captured scalars (escaping-frame check covers slices only) |
| **T3 Borrow across handoff** | View sent on channel / stored past reset | Provenance; stabilize; unique/`send_take`; serdes materialize-on-escape | Borrowed `T[:]` still sendable without a language ban |
| **T4 Arena / epoch stale** | Use after `arena_reset` / realloc-replace | Lexical borrow ban on reset; spawn capture **epoch pin**; slice/arena provenance ids; optional `CC_DEBUG_ARENA_PROVENANCE` | Raw `char*` / `@unsafe` / untyped smuggling; channel-send stabilize still protocol |
| **T5 Error ignore / nonlocal** | Fallible op ignored; half-updated state | Forced `T!>(E)`; `@errhandler`; `@defer(err)` | `(void)` discard; C APIs that return `int` |
| **T6 Shutdown / cancel race** | Use after stop | Deadline/cancel; nursery cancel; channel EOF | External threads; dishonest `@blocking` |
| **T7 Shared mutable without owner** | Data race on non-atomic shared state | Channel single-writer; `@scoped` guards | Shared `T*` folklore |
| **T8 Double free / wrong deleter** | Two owners free; adopt mismatch | Unique slices; move; `@destroy` once | Manual `free` beside `@destroy` |
| **T9 Wire length vs buffer** | Attacker length > buffer; over-read (Heartbleed-shaped); size wrap → tiny alloc; OOB index write; pointer/length desync | Schema `bytes len`; `cc_*_i64_checked`; Result `at`/`set` (all builds); slice carries `.ptr`+`.len` as one value | Hand `memcpy(dst, p, wire_len)`; bare `size_t a+b`; raw `ptr[i]=` past `len`; writing `.len` on a live slice. Cross-parser HTTP policy is out of corpus |
| **T10 Wrong representation** | Value stored as text/bytes when a sum type is needed; parse/encode tax hides bugs; inactive-arm use | `@variant` + protected projection; raw `.u` ban; schema `one of` (same surface); Result `!>`/`?>` | `@unsafe`; compound-literal `.kind`/`.u` interop; flow-insensitive domination |

## Scoring hint

- Prefer **prevented** only when a compile-time or type/schema rule blocks the shape.  
- Prefer **mitigated** when idiomatic structure or idiomatic serdes/variant use makes the bug unnatural but escape hatches remain.  
- Prefer **still_expressible** when ordinary CC (including hand parsers / raw buffers) can recreate it — file `needs_language:`.  
- Prefer **n/a** only for crypto/authz/logic with **no** ownership, wire-framing, or representation angle.  

Do **not** mark Heartbleed-class length-vs-buffer as `n/a` solely because nurseries don’t help — check SERDES first. Omit cross-parser HTTP smuggling from the corpus.
