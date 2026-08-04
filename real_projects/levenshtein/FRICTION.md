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

## A semicolon inside a leading comment corrupts the .shcc emit plan

The smoke test's header comment said "'é' is one edit; an astral-plane
char is one edit" — and the whole file stopped compiling with
`';' expected (got '@')` on a wrong line. Bisection (unicode, quotes,
line counts, template size — all innocent) landed on the one character
that mattered: the `;` inside the comment. The .shcc emit plan anchors
its prelude insert position on it without comment awareness and splices
result-spec typedefs mid-comment. Tracked as task #53;
`tests/shcc_comment_semicolon_fail.shcc` is the tripwire, and the smoke's
comment stays semicolon-free until the fix lands.

## Keyword-only parameters cannot be spelled

Upstream 0.27 takes its extras keyword-only: `hamming(a, b, *, pad=True)`,
`distance(..., weights=...)`, `score_cutoff` everywhere. py_module
trampolines are positional-only (`METH_FASTCALL`, no kwnames handling), so
cclev cannot even accept `pad`. The specimen bakes in upstream's defaults
(hamming pads) and parity.py calls upstream with `pad=True` explicitly.
Fixing this means kwargs support in the trampoline layer — a real interop
gap, not a specimen problem.

## UTF-8 decoding is hand-rolled

Python str crosses the boundary as UTF-8 bytes in a `CCSlice`; edit distance
must count codepoints ('é' vs 'e' is one edit, not two). The specimen
hand-rolls a ~20-line decoder into an arena. Fine once, but every
text-processing module will write this loop; a stdlib codepoint
iterator/decoder over `CCSlice` would erase it.

## Upstream semantics live in the implementation, not the docs

Two behaviors only empirical probing settled: transposition counts halve
with *integer* division (an odd mismatch count rounds down), and
jaro_winkler applies its prefix bonus only above the classic 0.7 boost
threshold. Neither is in upstream's docstrings; parity.py's random sweep
(5060 checks, ASCII through astral) found both. This is what "judged by the
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
