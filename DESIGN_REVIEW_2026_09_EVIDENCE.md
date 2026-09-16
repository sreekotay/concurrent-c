# Design review, 2026-09: evidence

Companion to `DESIGN_REVIEW_2026_09.md`. That file is concepts; this one
is where each claim came from. Counts are grep counts and approximate.

Sources: the specimens under `real_projects/`; cctext, rlsw-cc, and
stylo-cc out of tree; `studies/cve_locality/`; the lowerer read as a
program written in CC (`cc/lower/`); the bootstrap chain.

Every finding carries one tag. Only **design** rows are arguments about
the language; the rest are work.

| Tag | Meaning |
|-----|---------|
| **design** | The model would produce this in fresh code by someone who knows the idiom. |
| **drift** | The code predates the concept it now contradicts. |
| **unimplemented** | Spec or a draft says what should happen; the lowerer does not do it yet. |
| **defect** | The lowerer does something wrong and the specimen worked around it. |
| **doc** | A page names a product as a primitive, or describes what the code no longer does. |

The friction files inside specimens mix these. Tagging them would let the
earned-syntax rule read only the design rows.

---

## 1. The local system

| Observation | Where | Tag | Reading |
|-------------|-------|-----|---------|
| Two `!>` on the two calls that touch the world; the Python twin has zero | `examples/serdes/json/tools/minify.shcc`, `minify.py` | design | the cost of visibility is two tokens |
| 151 common-error handlers and 67 index-fault handlers; functions bind both with different policies | cctext `frontend/*.ccs` | design | required handling by type, in production |
| Index-fault type has no face, on purpose | cctext `core/piece_tree.cch`, `DESIGN.md` Surfaces | design | the face criterion, stated by the author |
| Nearly every index handler discards `e` and returns; the fault is latched on the object | cctext frontends | design | the convention that keeps required handling cheap |
| Seven stdlib error types carry a face onto the common error | `cc/include/ccc/cc_io_error.cch`, `script/py.cch`, `script/js.cch`, `script/quickjs.cch`, `cc_slice.cch` | design | all satisfy the criterion; scripts depend on them |
| 22 handlers, 6 distinct policies | `real_projects/pigz/pigz_cc/pigz_cc.ccs` | design | policy varies by scope; not restatement |
| 18 handlers, 1 policy (propagate) | `real_projects/redis/redis_db.cch` | design | the uniform pipe, stated locally |
| 17 handlers, 2 policies | `real_projects/staticd/staticd.ccs` | design | works |
| `.base` projected in every binder between IO and command errors | `real_projects/redis/redis_idiomatic.ccs` | unimplemented | arg-position autocast through faces (`spec/draft_as.md`); partly drift |
| Decompress uses a global atomic as the error channel | `real_projects/pigz/pigz_cc/pigz_cc.ccs` | drift | predates `h.fail`; with no local option the error became ambient state |
| A read error inside a wait-for body is swallowed into a cancel flag | cctext `core/find.ccs` | design | a `return` in a ticket body is a ticket return; no spelling for "this ticket failed" |
| The error reporter needs its own handler to print | `real_projects/pigz/pigz_cc/pigz_cc.ccs` | design | correct refusal of same-E re-entry; costs a scope for the commonest helper |
| `.failed()` polled on templates at 12 sites | `real_projects/staticd/staticd.ccs` | doc | growth failure poisons and never truncates; pin that the poison trips the next consumer |
| 0 handlers, `?> NULL` x12, walks return bool with out-params | `cc/lower/*.cch` | domain | diagnostics are record-and-continue; expressible as a template handler with `@ok` and a site position |
| `@noblock` / `@nonblocking` on helpers outside any `@async` | `real_projects/redis/redis_idiomatic.ccs` | theater | no-op per spec §8.2.1; vestigial from the owner variant |

Work:

- **unimplemented:** arg-position autocast through `as:` faces
  (`spec/draft_as.md`); `@defer(ok|err)` at block scope
  (`cc/lower/lower_cleanup.cch`); `'!> @destroy'` in the clean lowerer
  (`cc/lower/lower_results.cch`, reported twice).
- **test:** re-entry through a face, not only the exact type; one handler
  inlined at three sites with `int`, pointer, and struct destinations once
  `@ok` is admitted; a handler reached from a nested block whose ledger
  entries armed after the handler line.
- **doc:** the-cc-way should state that handlers resume and resume means
  the next statement; that a discarding handler belongs on a narrow type;
  the face criterion; one form and one sugar; "on the declaration" in
  place of "on successful construction" (getting-started, cheatsheet,
  language-concepts).

## 2. Arena as lifetime

| Observation | Where | Tag |
|-------------|-------|-----|
| Slice id: bits 0..31 epoch, 32..58 generation, flags GROWER / CSTR / TRANSFERABLE / SUBSLICE / UNIQUE; owners compare tokens; views are not checked on read | `cc/include/ccc/cc_slice.cch` | design |
| Own step tracks locals only; parameters, field paths, index expressions, call results pass | `cc/lower/lower_own.cch` header | design, by choice |
| Stack-slice escape, arena-epoch pin, pointer-alias capture are three passes | `cc/lower/lower_own.cch`, `lower_closures.cch` | design |
| Closure escape computed as: returned, assigned to member, passed to any call that is not a scoped spawn | `cc/lower/lower_closures.cch` | design; the negative of the order |
| The order flips CVE-2023-54235 to prevented and shrinks SHAPE-T7; four other mitigations are bounds, arithmetic, representation | `studies/cve_locality/` verdicts | design |
| Teardown order is convention, unenforced | `spec/draft_lifetime_parents.md` §8 | design |
| Non-arena hooked types "enter a parent at birth via `create_*` constructors and never move"; only nursery and pool have one | `spec/draft_lifetime_parents.md` §5, `cc_arena.cch` | unimplemented for turnstile, parallel, exclusive |
| A tree keeps a caller's slice from `from_buffer`, guarded by a runtime untracked-id check; the callee cannot compare lifetimes it does not know | cctext `core/piece_tree.ccs` | design; the kept-parameter case |
| Heap arena per block, detached out of its own `@destroy`, shipped in the payload | `real_projects/pigz/pigz_channel.ccs` and siblings | drift |
| 64 KiB arena minted to host one turnstile; two arenas to host one exclusive and two vecs on a calloc'd object; parallel handle calloc'd because it cannot live in a bump arena | pigz, curl `thrdqueue.ccs`, cctext `core/browse.ccs` | design |
| Callee vocabulary for send / reset / spawn is a string table in the step; mutation safety keyed on "atomic" in a callee name | `cc/lower/lower_own.cch`, `lower_closures.cch` | design, contradicts ADR-S2 |
| Three constructors, one engine, differing in where L1 lives | spec §5.0 | doc |

Boundary probes, run on seed 0.4.0-404 (`studies/lifetime_boundaries/`):

| Observation | Tag |
|-------------|-----|
| Every ownership check is keyed to the C spelling; the taught UFCS spelling of reset and alloc is unchecked because the own step runs before UFCS | unimplemented |
| restore, try_restore, detach are spec epoch-ending ops and absent from the step's table | unimplemented |
| A stack slice captured into a same-frame nursery spawn is correctly accepted; the spec's capture example calls this an error | doc |
| Outer nursery plus inner-block stack buffer compiles; `leave` after a stack capture compiles | design; frame < join set |
| Views through a field, a call result, an unwrap, a kept parameter, a dest late-admit, and an arena held in a field are all unchecked | design; the holders in §2.4 |
| Aggregate-with-arena send does not move the arena binding; sender may reset | design |
| Handle copy, destroy through the copy, use of the original: compiles and segfaults | design and runtime |
| Reset after last use is refused by scope, not liveness | design; false refusal |

Peel census, `.ptr` sites in `.ccs` / `.cch` (grep, approximate):

| Specimen | Peels | Lines | Per kloc |
|----------|------:|------:|---------:|
| stylo-cc engine | 119 | 6346 | 18.7 |
| staticd | 101 | 6144 | 16.4 |
| lowerer | 635 | 47853 | 13.2 |
| pigz | 45 | 4362 | 10.3 |
| cctext core | 249 | 26906 | 9.2 |
| cctext frontend | 69 | 9220 | 7.4 |
| redis | 43 | 5982 | 7.1 |
| levenshtein | 4 | 713 | 5.6 |
| parallel_storm | 2 | 1098 | 1.8 |
| raytracer, random_access, curl port | 0 | | 0 |

Of 553 peels in the eight text-heavy specimens: index `.ptr[i]` 20%;
null or OOM check on a slice 17%; pointer arithmetic 7%; casts 7%;
`mem*` calls 4%; C or CC API taking pointer plus length 3%; the rest are
peels into a local `const char*` then a C loop, and comparisons.
`cc_parse` and socket write are the stdlib signatures that force a peel.

Re-cut by what removes each site: 176 (32%) are a `.ptr` passed to a
user helper declared with pointer plus length; 116 (21%) index scans;
96 (17%) null or OOM checks; 27 (5%) `mem*` into raw buffers; 28 (5%)
identity comparisons; 27 (5%) C-library calls. Removable: about 90%.

| Observation | Tag |
|-------------|-----|
| `cc_arena_alloc_slice_bytes` returns an empty slice on failure; 17% of peels are the null check this forces; the walk recipe's `if (buf.len != 8)` is the same check | design; contradicts the-cc-way |
| Scanner loops peel where `index_of`, `trim_set`, `starts_with`, `last_index_of` already exist | doc, and a cursor verb set |
| `cc_parse(G, s.ptr, s.len, …)` and `sock->write(s.ptr + off, …)` take pointer plus length | unimplemented; slice twins |

Result-returning allocation, measured (`-O`, 20M allocs of 16 bytes,
best of 5, shared container so absolute numbers are slow; relative is the
point):

| Path | raw pointer | Result pointer | raw slice | Result slice |
|------|------------:|---------------:|----------:|-------------:|
| shared bump (CAS) | 13.1 ns | 13.2 ns | 13.2 ns | 13.1 ns |
| local bump (no CAS) | 3.45 ns | 3.44 ns | 3.37 ns | 3.45 ns |

The wrappers inline; the Result tag test is the NULL test the caller
already wrote. No cost either way.

Ownership by scope versus by object (grep, `.ccs` / `.cch`):

| Specimen | `@destroy` | `@defer` | explicit destroy / free / close | `create_*` / adopt / attach |
|----------|-----------:|---------:|--------------------------------:|----------------------------:|
| pigz | 69 | 17 | 50 | 21 |
| redis | 32 | 33 | 35 | 0 |
| staticd | 11 | 8 | 22 | 2 |
| curl port | 0 | 14 | 15 | 1 |
| cctext core | 16 | 4 | 157 | 0 |
| cctext frontend | 3 | 0 | 43 | 0 |
| stylo engine | 3 | 1 | 3 | 0 |

cctext `RtxDoc_destroy` (`core/document.ccs`) destroys the tree first
and stops the find scan near the end; the field order declares find last,
so the destroy chain would stop the scan first and free the tree last. A
live scan at close reads a destroyed tree unless callers always stop it
first. **check; the chain's order is the dependency order.** The two
steps the chain cannot do are a history clear (no hook) and a vec unbind
that the chain already orders correctly.

Refcounts: `CCArc`, `cc_arc_*`, and hand `->ref` / `refcnt` appear 0 times
in `real_projects/`, cctext, stylo-cc, rlsw-cc, and `examples/`; 45 in
`cc/include`, 15 in `tests/`, 8 in `cc/lower` (the diagnostic that
recommends it). **design; the case that earns it has not been run.**

Implementation, from `cc/include/ccc/cc_arena.cch` and `cc_slice.cch`:

| Observation | Where | Tag |
|-------------|-------|-----|
| A copied heap-arena handle dereferences the freed host in `cc_arena_is_live`; destroy nulls only the binding passed; owner headers already solve this shape with a generation and a never-unmapped list | `CCArena`, `cc_arena_destroy`, `cc_arena_free`, `CCArenaOwner` | design |
| `cc_arena_pool_init` sets the arena to unbounded growth, or strips overflow when fixed; a relation inheriting a consequence | `cc_arena_pool_init` | design |
| `cc_arena_slice` stamps the root provenance unlocked; `alloc_slice` stamps `epoch_cur` under lock; a view minted over scratch does not go stale at restore | `cc_arena_slice`, `cc__arena_epoch_of` | defect |
| Typed-slice factory emits an unchecked, non-Result `at`; byte slices have the checked one in `std/slice.cch` | `CC_DECL_SLICE_SPEC`, `CC_GENERIC_FACTORY(CCSlice, 1)` | design |
| `cc_slice_from_static` sets the cstr bit unconditionally | `cc_slice_from_static` | defect |
| Comment on `cc_arena_attach` says restore refuses while records are linked; restore runs records attached since the mark | `cc_arena_attach`, `cc__arena_restore_slow` | doc |
| A view cannot find its host; every epoch check takes the arena; hosts draw epochs in 256-aligned blocks so a block-to-host registry would close it | `cc_slice_is_from_arena_epoch`, `cc__arena_epoch_fresh` | design |
| Point store checks stale grower generation; point load, typed `at`, and the walk check nothing | `cc_slice_store_at` | design |
| Grower view ids carry a 32-bit epoch drawn from one global counter per birth, reset, and checkpoint; plain view ids carry 60 bits | `CC_SLICE_ID_EPOCH_MASK`, `cc_slice_make_grower_id` | design |
| Owner generations are a 27-bit global namespace | `cc_slice_gen_birth`, `CC_SLICE_ID_GEN_MAX` | design |
| Pool freelist packs pointers into 48 bits; the header's own comment asks for an init check that is absent | `CC__POOL_PTR_MASK` | defect |
| A checkpoint is a lazily materialized child: marks until promotion, a real host after; the doc describes marks and children as two things | `cc__arena_promote_locked`, `cc_arena_checkpoint` | doc |
| Promotion exists for pre-mark regrow, parent-side records, and mark depth beyond three; per-object overflow already stamps a root epoch and could carry the regrow case | `cc__arena_alloc_parent_epoch`, `cc__arena_alloc_ovf_object` | design, a trade to decide |
| The universal UFCS hook is a snake-case rule plus an exception list for channels, vectors, maps, results; it lives here for include order | `cc_ufcs_generic_cc_prefix_lower_c`, `@typehooks on *` | design; declarative candidate |
| The header is seeded as host C: no Results, handlers, or the-cc-way idioms in the runtime core | header comment "Seeded as host C" | observation |
| `cc_arena_set_heap_overflow` and pool init mutate `_flags` and `block_max` unlocked while alloc paths RMW under the lock | `cc_arena_set_heap_overflow`, `cc_arena_pool_init` | defect, minor |
| The factory body duplicates `CC_DECL_SLICE_SPEC` verbatim as a string template kept in sync by hand | `cc_slice.cch` | design; typed emit |

## 3. Join and job

| Observation | Where | Tag |
|-------------|-------|-----|
| Noop first arm, atomics for done and cancel beside the handle, pause polled inside every stage, internal cancel called from user code, written three times | cctext `core/find.ccs`, `piece_tree.ccs`, `browse.ccs`, `FRICTION.md` | design |
| Loud-noop plant, then signal arm, workers, growth, respawn admitted onto one dest | `real_projects/staticd/CCServer.ccs` | design, works |
| Same stop-and-accept block verbatim | redis idiomatic and sketch | design |
| The compiler uses no concurrency construct; the parallel step is its largest | `cc/lower/lower_parallel.cch` | observation |

## 4. Tagged data

| Observation | Where | Tag |
|-------------|-------|-----|
| 503 of 510 switches carry `default:` | `cc/lower/*.cch` | defect |
| Causes: a generic instance in a case body, `::` in a switch body, an `@` word in a literal in a switch | `stress/break/` | defect |
| Packed variant for values, hand `{kind, u}` for replies, grammar unions tested with `kind ==` | `real_projects/redis/` | design |
| Comptime cannot name arms; MIME ints kept in sync by comment | `real_projects/staticd/staticd.ccs` | design |

## 5. Instance placement

| Observation | Where | Tag |
|-------------|-------|-----|
| Opaque struct plus cast at every use because a member cannot typedef a Table instance; the face includes its own members to pin order | `real_projects/staticd/CCServerPoll.ccs`, `CCServer.cch` | design or unimplemented |
| Vec of pointers needs a typedef; vec of a struct hoists above the prelude; no instance in a prototype | `cc/lower/ast.cch` | same |

## 6. Comptime

| Observation | Where | Tag |
|-------------|-------|-----|
| Packed-variant niche proofs read the driver's own pointer width | `cc/src/preprocess/variant_lower.c` | design |
| Handlers that compute names from argument types are opaque to the index | `cc/lower/INDEX_GAPS.md` | design |
| No specimen uses a computed-name hook | all five specimen reads | observation |
| Three specimens stamp kernels from a range with string templates | levenshtein, rlsw-cc, stylo-cc | design, earned |
| Emit fixed up by a regex pass; generator is `snprintf` into a static megabyte | rlsw-cc `tools/gen.sh`, stylo `engine/sty_emit.cch` | design |

## 7. The runtime as guest

| Observation | Where | Tag |
|-------------|-------|-----|
| Parks wrapped in deadlock-suppress because libcurl's loop is the progress source | `real_projects/curl_dns_port/thrdqueue.ccs` | design |
| One worker inside Ladybird; sequential twin beside every parallel site | stylo-cc `engine/stylebench_cc.ccs` | design |
| Sequential twin beside the stripe loop | rlsw-cc `src/fill/bin_par.ccs` | design |
| `@async` in one file, `@await` in none, with async-specific workarounds | `real_projects/redis/redis_owner.ccs` | design, unearned |

## 8. Receipts

The ladder is the freeze veto. Its rungs:

- Redis wins at pipeline depth 16 with two to four OS threads; not at
  depth 1; slower at one client. `real_projects/redis/benchmarks/`.
- Pigz is parity; the product went bimodal after a runtime change two
  days before the latest receipt; two variants emit zero bytes and remain
  in `make test`. `real_projects/pigz/benchmarks/`.
- The levenshtein headline ratio comes from the upstream number doubling
  between two receipts. `real_projects/levenshtein/benchmarks/`.
- Staticd leads nginx on small files and trails on large.
- rlsw-cc's phase log attributes most of its win to sequential work.
- Curl is a same-band regression check by its own README.

None of it changes a concept. All of it weakens the veto.
