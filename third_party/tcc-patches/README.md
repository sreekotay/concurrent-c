# TCC Patch Notes

Goal: minimal, optional patch set to integrate CC without forking TCC.

Target hook surface:
- Lexer token registration for CC keywords/operators (behind build flag).
- Parser extension entry points to allow CC productions.
- Constexpr evaluator API exposure for `@comptime if` and type-returning comptime functions.
- Optional AST side-table or small ext fields (no structural divergence).

Keep patches small, well-commented, and upstreamable. Document TCC commit hash used.

