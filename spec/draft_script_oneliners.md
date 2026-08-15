# One-liner mode

Status: draft — implemented (`-e` / `-E`, `-n` / `-p`, predecls, toolbox).

## 1. `-e` / `-E`: program text as argument

```text
ccc [ccc-flags...] -e PROGRAM [script-args...]
ccc [ccc-flags...] -E EXPR [script-args...]
ccc [ccc-flags...] -e - [script-args...]
```

`-e PROGRAM` compiles `PROGRAM` as an unnamed `.shcc` unit — the §9.5.1
entry rewrite applies unchanged (prelude force-include, synthetic `main`,
injected default `@errhandler`, `@task` discovery and dispatch) — and runs
it. Program text is content-keyed under `out/.cc-build/e/<hash>.shcc`.
`PROGRAM` is ordinary Concurrent-C / `.shcc` statement text — no bare-template
mode and no alternate string delimiters; `@string` and C literals keep their
usual forms.

`-E EXPR` is `-e` with a trivial print wrap. Trailing whitespace/`';` on
`EXPR` are stripped. When `EXPR` already contains a backtick template or
begins with `@string`, the wrap is:

```c
cc_println(EXPR) !>;
```

Otherwise:

```c
cc_println(@string(`${EXPR}`)) !>;
```

(so `-E '1+2'` still stringifies, while `-E '@string(`hi`, &a)'` does not
nest templates). `-e` and `-E` are mutually exclusive.

`-e -` reads the program text from standard input to end-of-file; the
program then starts with its standard input at end-of-file. Programs that
both arrive on stdin and read data from stdin are not expressible; use a
file.

One-liner mode flags (`-n`, `-p`, `--save`, `--save-to`, `--doc`) may appear
anywhere among `ccc` flags relative to `-e`/`-E` (before or after `PROGRAM`);
remaining non-flag arguments after `PROGRAM` are script arguments (use `--` to
pass a literal `-n`/`-p`). The process `argv[0]` is the synthetic unit name
`-e` or `-E` (script args begin at `argv[1]` / `args[0]`).

### 1.1 Predeclared names

In a `-e` / `-E` program body and in a `.shcc` unit’s synthetic `main` wrap
(top-level statements only), the following names are predeclared. Each
declaration is emitted only when its identifier appears as a code token in
that body (comments and string/character literals are ignored) and the body
does not already declare the name. `@task` bodies never receive these
predecls; they declare `a` / `io` / `in` / `args` explicitly when needed.

| Name | Type | Declaration |
| ---- | ---- | ----------- |
| `a` | `CCArena` | `a@(megabytes(1)) @destroy` |
| `io` | `CCStdio` | private `__cc_io_arena` + `io@(&__cc_io_arena) @destroy` |
| `in` | `char[:]` | `io.read_all() !>` |
| `args` | `CCSlice` | `{ .ptr = (char *)(argv + 1), .len = (size_t)(argc > 1 ? argc - 1 : 0) }` |
| `line` | `char[:]` | current input line (`-n` / `-p` only; loop-local) |
| `nr` | `size_t` | 1-based line counter (`-n` / `-p` only; loop-local) |

Using `in` implies `io`; using `io` implies its private arena (and `a` only
when `a` is also referenced). Using `line` or `nr` implies `io`. `-E`, `-n`,
and `-p` force `io` (and `a` when needed).

The predeclared arena `a` is fixed at 1 MiB initial capacity (arena growth on
overflow still applies where the allocator allows it). Large `in = read_all()`
payloads can exhaust it; use an explicit arena or a file unit when that
limit is too tight.

Predeclared names are ambient plumbing: each has exactly one reasonable
initialization and no failure path the program manages (`in`'s `!>` under
the injected default handler is the register's posture, not management).
Error binders are never predeclared: error access stays the explicit `(e)`
binder form (§3.1).

### 1.2 `-n` and `-p`: line loops

`-n` wraps the (possibly `-E`-wrapped) body in a loop over standard-input
lines via `CCStdio.read_line`. Each iteration binds `line` (without its
trailing newline) and increments `nr`. `line`'s storage is for the current
iteration only. `-p` is `-n` with `line.println() !>;` appended to the loop body, so
`continue` skips the print and `break` ends the loop.

```text
... | ccc -n -e 'if (nr == 1) line.println() !>;'
... | ccc -p -e '/* transform line */'
... | ccc -n -E 'nr'
```

`-n` / `-p` require `-e` or `-E`. `-p` cannot combine with `-E`.

### 1.3 Result consumption

`-e` / `-E` programs follow the same result-consumption rules as every other
unit (§3.1), including type-matched `@errhandler` dispatch. No mode relaxes
the unhandled-result diagnostic. The injected default `@errhandler(CCError)`
(in synthetic `main` and in each `@task` body) makes bare `!>` a complete
failure policy for `CCError` / untyped unwraps: message to stderr via
`cc_eprintln(cc_error_str(e))` (Result discarded), exit 1. `CCIoError`
Results match through the `@typeview` `as:` face; constructors fill that face's
message so the print is not blank. A script may register additional
handlers for other error types in the same scope; they coexist with the
default by type match.

### 1.4 Console print

When `io` is in scope, prefer handle-first UFCS. Data-first and naked aliases
remain valid (UFCS either way). Cstr / string-literal *data* receivers coerce
to `CCSlice` then `cc_slice_*` (no `cc_char_*` UFCS family):

```c
io.println(path) !>;
io.eprintln(line) !>;
io.println(@string(`n=${n}`, &a)) !>;
path.println() !>;                 /* also OK */
"literal".println() !>;
println(path) !>;                  /* naked alias */
path.fprint(STDERR_FILENO) !>;
```

Short names `println` / `eprintln` are not free macros — a function-like
`#define println(x)` would steal UFCS `x.println()`. Returns are
`CCResult_size_t_CCError` (same as `CCStdio.println`). The macros
`cc_print` / `cc_println` / `cc_eprint` / `cc_eprintln` are lowered-C only
(driver inject, `-E` desugar in §1).

## 2. Toolbox

`--save NAME` (with `-e` / `-E`) appends the program to the toolbox as a
`@task`. The saved body is the fully lowered register (`-E` / `-n` / `-p`
desugar and used predecls written out explicitly).

```c
/**
 * @task TEXT of --doc when given, else the program's first line.
 */
static int NAME(int argc, char **argv) { ... }
```

`NAME` must be a C identifier not already present in the toolbox; a
duplicate is an error.

The toolbox is `./tools/toolbox.shcc` when the working directory is inside a
repo (dev-repo markers), else `~/.ccc/toolbox.shcc`; `--save-to PATH`
overrides.

A first positional argument beginning with `@`, with no unit path, resolves
against the toolbox and dispatches as §9.5.2a:

```text
ccc @grep_errs < build.log
ccc @                        # list saved tasks with summaries
```

## 3. Out of scope

- Field splitting (`-F`, awk registers)
- Additional predeclared names (`out`, `root`, `tmp`, `env`, error binders)
- A REPL
- Relaxed result-consumption modes for `-e`
- Alternate `@string` delimiters / bare-template `-e` (rejected: `-e` is code)
- Silent expression auto-detect without `-E`
