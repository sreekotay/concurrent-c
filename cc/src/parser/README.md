# CC Parser Overlay

Extends TCC grammar for active CC constructs (`@async`, `@match`, `T!>(E)`, `T[:]`, `T[~]`, `@comptime if`, comptime type-returning functions) and rejects retired syntax such as the `T?` optional type and the `@nursery`/`@arena` block forms with migration errors. Uses parser hook entry points; avoid core grammar forks.

