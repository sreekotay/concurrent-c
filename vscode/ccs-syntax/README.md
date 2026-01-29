# Concurrent-C Syntax (VS Code)

Syntax highlighting for Concurrent-C source and headers:

- `*.ccs` (Concurrent-C source)
- `*.cch` (Concurrent-C headers)

Concurrent-C is largely C + preprocessor, with a few extra surface-syntax constructs like:

- `@arena { ... }`
- `@nursery { ... }`
- `@defer label: { ... }`
- `@async` function modifier
- `spawn (call(...))`
- UFCS-style `value.method(...)` / `ptr->method(...)` calls
- Core keywords like `await`, `try`, `catch`, `unsafe`, `comptime`
- Type sugar like `T?`, `T!>(E)`, `T[:]`, `T[~N ...]`
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

3. Open a `.ccs` or `.cch` file; the language mode should be **Concurrent-C**.

## Develop / tweak the grammar

- Grammar: `syntaxes/concurrent-c.tmLanguage.json`
- Language config (comments/brackets): `language-configuration.json`

After edits, reload the window to see changes.


