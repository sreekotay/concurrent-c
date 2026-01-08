# TCC Hook Targets (Planned)

1) Lexer hook
- Allow registration of additional tokens/keywords/operators under a CC build flag.
- No changes to default lexer table when CC disabled.

2) Parser extension entry
- Provide callback points to recognize CC grammar constructs (e.g., `@async`, `@nursery`, `@match`, `T?`, `T!E`, `T[:]`, `T[~]`, `@comptime if`, comptime type-returning functions).
- Keep existing grammar intact; CC mode activates extensions.

3) Constexpr evaluator API
- Expose TCC’s constexpr evaluator as a callable API for `@comptime if` and comptime functions returning types.
- Pure addition; no behavioral change to default builds.

4) AST side-table support
- Optional ID or pointer on nodes to attach CC metadata without altering core layouts.
- Should be zero-cost/no-op when unused.

Notes
- Hooks must be optional and upstreamable; CC mode off should yield pristine TCC behavior.
- Document exact upstream commit hash and patch files once authored.

