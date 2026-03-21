# CC Parser Overlay

Extends TCC grammar for active CC constructs (`@async`, `@match`, `T?`, `T!>(E)`, `T[:]`, `T[~]`, `@comptime if`, comptime type-returning functions) and rejects retired block syntax such as `@nursery`/`@arena` with migration errors. Uses parser hook entry points; avoid core grammar forks.

