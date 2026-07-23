# Ownership / concurrency shape

- **Taxonomy class(es):** **T8** Double free / wrong deleter
- **Actors:** Foreign allocator vs CC unique-slice destructor
- **The mistake in one sentence:** Ownership was adopted into CC with a
  deleter that does not match how the bytes were allocated (or freed
  twice: manual + destructor).
- **Rust angle (claim A):** Safe Rust rejects double-drop of `Box`;
  constructing owned memory from FFI is `unsafe` (`from_raw`) — wrong
  layout/deleter is outside claim A’s “ill-formed in safe Rust.”
- **CC angle:** Same trust boundary. Unique/`adopt` prevents *copying*
  ownership (SHAPE-T8-use-after-move); it cannot prove the deleter is
  the right one. Spec today; runtime `adopt` may still be landing — the
  shape is the same with `@destroy { wrong_free(p); }`.
