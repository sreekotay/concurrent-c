# Friction log: python-Levenshtein port

What the port surfaced, in the order it drew blood. Gaps are data
(`real_projects/README.md`); none are smoothed over in the specimen.

## Compiler bug: `double !>(CCError)` module method broke the build

The first fallible `double` method died in the parser pass with
`',' expected (got 'r')` pointing at generated code. `CCResult_double_CCError`
is stdlib-predeclared (string.cch/slice.cch own its definition), so the emit
plan skipped its struct decl — but still emitted the parser-helper prototypes
that name the type, ahead of the forward tags that would have declared it. A
TU including neither owning header handed TCC an undeclared parameter type.
Fixed in this round (forward tags now precede the prototypes);
`tests/py_module_double_result_smoke.shcc` pins it, mutation-checked both ways.
`long long !>` methods never hit it — that spec isn't predeclared, so its
full decl was emitted inline. The failure needed exactly this shape: a
py_module TU using a predeclared Result type without its owning header.

## Scanners read string-literal content as code

The regression test first embedded the module source in a backtick template
(write-to-file-then-build). The include scan, result-spec scan, and factory
scan all read the template's *content*: `#include <ccc/script/py.cch>` inside
the string dragged the real py.cch into the test's own compile, complete with
`CCResult_void_CCPyError` decls landing mid-function. Worked around by
shipping the module as a companion `.src` file the test copies into place.
The scanners have a shared template-mode discipline; these three don't use it
for this case yet.

## Fixed: semicolon in a leading comment broke `@destroy` on `.shcc`

A header comment containing `;` made `CCArena a = … @destroy` die with
`';' expected (got '@')`. The emit-plan insert story was a misread —
prelude insert already skips block comments. The real hole was the
builtin-owned `@destroy` rewrite: its statement-start walk treated any
byte `;`/`{`/`}` as a boundary, so a `;` inside the leading comment
became the anchor and the rewrite skipped the site. Pinned by
`tests/shcc_comment_semicolon_smoke.shcc`.

## Keyword-only (`*`) still unspelled

Defaults are spelled as C++-style literals on parameters
(`long long pad = 1`): reflection sees them, lowering strips them for
host C, and `py_module` fills missing kwargs from the literal. Upstream
0.27's keyword-only marker (`hamming(a, b, *, pad=True)`) is still not
modeled — `pad` may also be passed positionally. Weights / cutoffs are
not exported yet.

## Text as `CCPyStr`, not UTF-8 `CCSlice`

Methods that want Unicode scalars take `CCPyStr` (call-scoped borrow of
the Python `str`) and call `s.codepoints(arena)` — Stable-ABI UCS4 fill,
no UTF-8 round-trip. `CCSlice` remains the UTF-8 byte view when that is
what the API wants.

## `name__helper` statics were mistaken for grammar tape

Any identifier containing `__` was classified as a grammar-engine static
and emitted as opaque tape — so UFCS inside `lev__prep` never rewrote.
Tightened to `Name__r_` / `__m_` / `__b_` / `__fill` / `__s__*` only;
`tests/ufcs_dunder_helper_smoke.ccs` pins it.

## Upstream semantics live in the implementation, not the docs

Two behaviors only empirical probing settled: transposition counts halve
with *integer* division (an odd mismatch count rounds down), and
jaro_winkler applies its prefix bonus only above the classic 0.7 boost
threshold. Neither is in upstream's docstrings; parity.py's random sweep
(6061 checks, ASCII through astral) found both. This is what "judged by the
real package" buys.

## What worked without friction

Module state as one lazily-created scratch arena — reset per call, zero
steady-state allocation, torn down by `destroy` as the module's `m_free` —
fell out of the py_module lifecycle as designed. The one-command build
(`ccc build`, PyInit_ inference, `.abi3.so`) needed nothing project-specific.

The bit-parallel round (Myers/Hyyro distance, Allison-Dix LCS, blocked to
arbitrary lengths) was prototyped against the scalar DP in Python, then
ported as plain uint64 loops: it compiled first try and passed the full
parity sweep unchanged. Dense bit-twiddling C is just C here — no
friction to log, which is itself the data point.
