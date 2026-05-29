# Design discipline

How CC's surface is governed. (Language semantics live in `spec/`; this is *process*.)

## Syntax must be earned

Features start as **lowered code** — plain functions, the explicit tier — and earn dedicated syntax only by proving themselves under real use. The baseline is first-class and clean, so syntax competes against a *good* alternative, not a miserable one. That keeps the bar high.

Unearned syntax is **removed**, not grandfathered. Already cut: `try` and optionals (disuse — real code reached for results / nullable pointers instead); `@nursery` and `@arena` blocks (friction — they wanted RAII that `@destroy` provides generally, and forced odd, less-local syntax).

> Friction is a signal, not a nuisance. A construct that *fights* you — that wants a capability another primitive already owns — is telling you the real primitive lives elsewhere. Sometimes it doesn't exist yet, and the fight is how you discover it (`@destroy` was found this way).

## One meaning per sigil

A sigil signals "CC construct here" and must be colorable with no context. Its **value is inversely proportional to the context needed to interpret it.**

- A sigil has **one meaning**; its grammatical *forms* may multiply under it. `!>` always means "fallible/error machinery" (declaration, forward, handle — one meaning, three forms). `~` always means "routed through a mechanism, not literal" (channel queue; template policy slot).
- A sigil that means *different things in different positions* forfeits both benefits at once: it confuses the reader and forces the highlighter to do parser work.

**Conformance test:** the context-free TextMate grammar (`vscode/ccs-syntax`) must highlight the construct correctly. If correct highlighting needs types or scope, the sigil is overworked.

## Validation ladder

Constructs are proven against progressively more demanding real projects, each chosen to stress a **different axis** — keep the ladder domain-diverse, not just larger:

- **pigz** — from-scratch parallel throughput parity (vs. C / Go / Zig).
- **redis** — stateful server, async, the hard compiler bugs.
- **curl (next)** — brownfield interop: dropping CC into existing C.

A monodomain corpus only finds what that domain predicts. Add at least one off-axis project (compute-heavy or generics-heavy, not concurrency-heavy) to surface gaps the systems-C trio structurally cannot.
