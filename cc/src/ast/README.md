# CC AST Side Tables

Store CC-specific metadata (async flags, scoped markers, slice provenance annotations, comptime function markers) without forking TCC node layouts. Prefer side tables keyed by node IDs.

