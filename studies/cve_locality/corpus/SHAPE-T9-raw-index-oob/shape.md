# Ownership / concurrency shape

- **Taxonomy class(es):** **T9** Wire / length vs buffer
- **Actors:** Holder of a `CCSlice` / buffer writes at an untrusted index
- **The mistake in one sentence:** Index not proven `< len` before a
  raw pointer write.
- **Rust angle (claim A):** `slice[i] =` is bounds-checked (panic) or
  `get_mut` returns `Option` — silent OOB write is ill-formed in safe Rust.
- **Likely CC verdict:** `still_expressible` — no compile-time ban on
  `ptr[i]=` past `len`.
