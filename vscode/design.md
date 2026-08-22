# Design

One language-server core, one thin editor shim (`extension.js` launches
`bin/cc-lsp`). The compiler is the diagnostic authority. Memory is owned or
it is a view. Lifetime is a field, not a protocol. A path that gives up says
so at the position that caused it.

This is the same shape as [cctext](https://github.com/sreekotay/cctext)
(`DESIGN.md`): one document core, two frontends, a host that does not wait
on the expensive walk. Here the “document” is the open buffer set, the
“frontends” are VS Code / Cursor / any LSP client, and the expensive walk
is `ccc --emit-c-only` (later: in-process shadow parse).

Phases and feature inventory live in [`roadmap.md`](roadmap.md). This file
is the locality / epoch / pump contract.

## Locality

Construct, use, `@destroy`. The reader sees the epoch change.

A JSON-RPC frame is bytes on the inbox. The session copies what it must
keep (`uri`, text, `gen`) before the frame arena dies. A check wave owns
its heap and dies at the end of the step — same cut as cctext’s find wave
(`find_step` call-local arena, not `d.find.store`).

Do not extract a shared `LspScan` type. Bits stay local (`doc.gen`,
`check.stale`, stdin EOF). The next walk copies the table below.

`extension.js` only starts the binary. It does not parse diagnostics,
debounce, or hover. That is a frontend, not a second server.

There is no inflight counter and no drain-to-zero on `didChange`. Latest
`gen` wins. A path that gives up publishes an error diagnostic — empty
`diagnostics: []` means “compiler said clean,” not “could not run `ccc`.”

## Epochs

| Epoch | Storage | Lives until |
|---|---|---|
| Process | `g_ccc`, `g_root`, stdout exclusive | `exit` / stdin EOF |
| Session | open-doc table (`uri`, text, `gen`) | `didClose` or process end |
| Frame | inbox payload + parse arena | end of `handle_message` |
| Check snapshot | heap copy `{uri, logical, text, gen}` | end of the check task |
| Check wave | call-local heap (`ccc` stderr, temp dir) | end of the step (`@destroy` on Result return) |
| Hover | session text + static notes | the reply; never a check |

`gen` is a written bit on the document, not a stdin poll. A later
`didChange` bumps it. A finishing wave that sees a newer `gen` drops —
it does not publish, and it does not wait for siblings.

cctext: query + hits stay on `d.find.store`; the 4 MiB wave dies with
`find_step`. Here: buffer text stays on the session doc; `ccc` scratch
dies with the check. Do not park stderr slices on the frame arena and
then `msg.reset()`.

## Interactive

The session fiber does not wait on `ccc`. `initialize`, hover, and the
next keystroke are bytes: parse, table lookup, reply. Diagnostics are a
notification that arrives when a wave finishes — like first paint before
the line index meets the prefix.

Two cameras, two writes — do not mix them.

| camera | origin | write |
|---|---|---|
| session | stdin frame | JSON-RPC **reply** (`initialize`, hover, `shutdown`) |
| check | snapshot + `gen` | **notification** (`publishDiagnostics`) |

Hover on the check camera is the bug: a full emit on the stdio loop
wedged the editor, so `didChange` stopped checking and hover stayed off.
A seek is not “index incomplete.” A reply is not “wait for `ccc`.”

Stdout is one stream. Session replies and check notifications take the
same exclusive (or one writer fiber). They do not share a call stack
with `fread(stdin)`.

Host frame (session): mutate the doc table, bump `gen`, kick a check,
return. One publish per finished current wave — not mutate-after-publish,
not publish-then-kick on the same stale snapshot.

`--check FILE` and `--smoke` are tests: they drain. The interactive path
must not drain before the first reply. cctext: do not drain the first
screen before first paint; tests call `finish`.

## Scan

A check is a pump. The host yields. Kick plants the snapshot and
returns. One closed interval per step (`ccc` on that snapshot);
returning is the yield.

| | start | one wave | live | resume | deny |
|---|---|---|---|---|---|
| check | `didOpen` / debounced `didChange` / `didSave` | `ccc --emit-c-only --no-runtime --no-cache` (later: `cc_shadow_parse_buffer`) | `gen` still current | new snapshot, same `uri` | stale `gen` |
| hover | `textDocument/hover` | token + static note | — | — | no `ccc` |
| stdin | process start | one `Content-Length` frame | until EOF | next frame | — |

Debounce is **one delayed kick per `uri`**, not a sleeper per
`didChange`. `didOpen` and `didSave` kick immediately (delay 0).
`didChange` arms a waiter that sleeps ~100–150 ms, then reads the live
`doc.gen` + text and starts at most one wave.

While that waiter is sleeping, later keystrokes only update the doc —
they do not spawn another timer. After the wave is in flight or the
`uri` is idle, the next `didChange` arms one new waiter; that start
kills the in-flight child. If `gen` moved during the sleep, the waiter
still starts — it starts on the *latest* snapshot, not the one that
armed it. A 30 k TU will feel like “pause then squiggle” until Phase 2.
That is correct.

`gen` is correctness (which snapshot may publish). The ticket chan
(two live `ccc` processes) is a resource bound across **files**. One
`uri` holds at most one wave: start the new one, kill the previous
child (`ccc` pid, not just the fiber — `cmd.output()` will not see a
nursery cancel). The corpse drops on stale `gen` if it still finishes.
Do not spend both tickets on two generations of the same buffer.

`$ /cancelRequest` is a **request-id** cancel (a hover, later a
completion). It does not bump `gen` and it does not kill a check.
Checks are notifications we kicked; they have no request id.
`didClose` drops that `uri` (and its `gen`) and kills its child.
Until a request is expensive enough to cancel, `$ /cancelRequest` is a
no-op.

`didChange` is full document sync (Phase 1: `textDocumentSync: 1`). The
session replaces the text in place, then copies a snapshot for the wave.
The wave never holds a pointer into the live table.

A buffer check writes under `$TMPDIR` (never the project tree) and
stamps `#line 1 "logical"` so quoted includes (`"browse.cch"`) still
resolve from the real file’s directory. `cwd` is that directory, not
the first workspace folder — out-of-tree projects (cctext) are
first-class.

## Surfaces

Fallible work is a Result (`T !>(CCError)`). Value returns are pure
queries of already-valid state (token at offset, `doc_find`, capabilities
JSON).

`ccc` spawn failure, `mkdtemp` failure, and a missing compiler are
errors — publish one diagnostic at `1:1` with that fact. Do not publish
`[]` and look successful.

Owned bytes take the destination arena last (receiver first, arena last).
Check snapshots are heap-owned so they outlive the frame. Views (`CCSlice`
into session text) do not leave the session fiber.

A Result failure on a check is **unchanged session state**. The previous
published diagnostics stay until a current wave replaces them, or
`didClose` publishes `[]`.

## Faces

A role is reach at the call site, not a smaller struct. Add a `@typeview`
when a new function would otherwise take the whole session and only need
a slice.

| face | may | may not |
|---|---|---|
| stdin | read frames, `inbox.send` | `ccc`, `fwrite(stdout)`, `msg.reset`, doc table |
| session | docs, `gen`, replies, kick | block on `ccc`, hold a wave arena across a recv |
| check | snapshot, `ccc`, parse stderr | mutate docs, reply to a request id, write stdout unlocked |
| writer | `lsp_write` | read stdin, run `ccc` |
| hover | session text, static notes | spawn `ccc`, wait on a wave |

`@typehooks` stay next to the types they name (`OpenDoc`, snapshot,
frame). Do not put lifecycle on the JS TU.

## Host

```text
stdin fiber  ──frames──►  inbox  ──►  session (parent)
                                         │
                          hover / init ──┤── exclusive stdout
                          didOpen/Change ┤
                                         │ snapshot {uri, gen, text}
                                         ▼
                               check nursery (≤2 live ccc)
                                         │
                                if gen still current
                                         ▼
                                      outbox ──► publishDiagnostics
```

Nursery is the process epoch. `exit` / stdin EOF cancels it; teardown
waits. Checks are children. A check crash is a child — stdin and session
keep running.

Do not `@parallel` the protocol (not a data-parallel range). Do not
`@async` the whole server (the work is `@blocking` I/O and subprocesses).
A turnstile is for staged hops inside one wave (cctext find), not for
JSON-RPC.

TUI vs GUI host order in cctext (`pump → paint → input` vs
`input → pump → paint`) is the same rule: pick one. Here it is
`recv frame → mutate/kick/reply → (waves publish on their own)`.
Do not `ccc` then reply then `ccc` again on the stdin fiber.

## Files

cctext sits on the 8192-node AST cap; chapters become linked TUs
(`find.ccs`, `gui_draw.ccs`, host loop stays in `cctext.ccs`). Same cut
when `cc_lsp.ccs` hurts:

| TU | owns |
|---|---|
| `cc_lsp.ccs` | `main`, `--stdio` / `--check` / `--smoke` |
| `cc_lsp_wire` | headers, `lsp_write`, outbox |
| `cc_lsp_session` | docs, `gen`, method switch, debounce kick |
| `cc_lsp_check` | temp file / `ccc` / stderr parse |
| `cc_lsp_hover.cch` | already separate (static notes) |

`--check` and `--smoke` call the check and parse helpers directly. They
do not start the nursery.

## Compiler

The compiler is the source of truth. Do not use clangd on lowered C —
UFCS, results, and spawn safety are erased or renamed in emission.
Incomplete files publish what the whitelist recovers; a silent drop is
not a clean buffer.

Phase 1.5 (`--json-diagnostics`) deletes the stderr regex. Phase 2
(`cc_shadow_parse_buffer`) swaps the wave body. The pump table does not
change: same snapshot, same `gen`, same outbox.

## Lessons (cctext → cc-lsp)

| cctext | cc-lsp |
|---|---|
| UI thread does not wait on the line index | session does not wait on `ccc` |
| Kick, then first paint | reply `initialize` / hover before the first wave |
| One wave, then yield (`find_step`) | one `ccc` on one snapshot, then drop or publish |
| Wave arena ≠ find store | check heap ≠ session doc text |
| `gen` / `cancel` is a written bit | `doc.gen` on edit/close; `$ /cancelRequest` is not `gen` |
| Tests drain; interactive does not | `--check` drains; stdio does not |
| Two cameras (seek vs line) | reply vs `publishDiagnostics` |
| Give-up is not success | failed `ccc` is a diagnostic, not `[]` |
| Host: mutate, then one window write | mutate + kick + reply; publish from the wave |
| Thin frontend, fat core | JS launches; CC owns the protocol |
| No inflight drain-to-zero | latest `gen` wins; no join-all on keystroke |
