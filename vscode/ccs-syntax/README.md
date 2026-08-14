# Concurrent-C Syntax (VS Code)

Syntax highlighting for Concurrent-C source, headers, and scripts:

- `*.ccs` (Concurrent-C source)
- `*.cch` (Concurrent-C headers)
- `*.shcc` (Concurrent-C scripts; same language as `.ccs`, outside the `.ccs`/`.cch` pair)

Concurrent-C is largely C + preprocessor, with a few extra active surface-syntax constructs like:

- `@create(...) @destroy` / `@detach` lifecycle declarations
- `@defer` / `@defer(err|ok)` / `@cancel`
- `@async` / `@await` / `@blocking` / `@nonblocking`
- `@errhandler` / `@err` / result unwrap (`!>`, `?>`)
- `@string` backtick templates (`${…}`, `$~tag{…}`, `${{…}}` verbatim) — string coloring
- `@emit` backtick bodies — Concurrent-C/C highlighting with `${…}` interpolations
- `@grammar(engine) Name {~~~~ … ~~~~}` fences (`rules` / `schema` / `cli`)
- `@variant` / `@variant(packed)`
- UFCS-style task operations like `n->spawn(...)` / `n->wait()`
- UFCS-style `value.method(...)` / `ptr->method(...)` calls
- Type sugar like `T?`, `T!>(E)`, `T[:]`, `T[:!]`, `T[~N …]`
- Duration literals like `10ms`

## Install (local, no marketplace)

VS Code can load extensions from `~/.vscode/extensions/`. Cursor can load extensions from `~/.cursor/extensions/`.

1. Copy this folder to your extensions directory:

```bash
mkdir -p ~/.vscode/extensions
cp -R /path/to/concurrent-c/vscode/ccs-syntax ~/.vscode/extensions/concurrent-c-syntax
```

Or run the helper script from this repo:

```bash
cd /path/to/concurrent-c
./vscode/ccs-syntax/install-local.sh --both   # VS Code + Cursor
```

2. Reload VS Code (`Developer: Reload Window`).

3. Open a `.ccs`, `.cch`, or `.shcc` file; the language mode should be **Concurrent-C**.

A good smoke fixture for templates + `@grammar` is `tools/cc_perf_check.shcc`. For `@emit` C-body highlighting, open `real_projects/levenshtein/levenshtein_cc.ccs`.

## Develop / tweak the grammar

- Grammar: `syntaxes/concurrent-c.tmLanguage.json`
- Language config (comments/brackets): `language-configuration.json`

After edits, reload the window to see changes.
