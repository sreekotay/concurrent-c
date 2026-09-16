# Typeview boundaries: what an allow-list confines

Four probes against `@typeview on Inner { r: *; }` and a named mode.
Toolchain: seed `0.4.0-404`, built in this tree.

| Probe | Shape | Result |
|-------|-------|--------|
| `site_store.ccs` | `x.done = 1` at an ordinary site | refused: `restricted mode '(default)' on 'Inner' does not allow store to field 'done'` |
| `mode_write.ccs` | `@typeview(Scan) Find*` body stores a field outside `Scan`'s `rw:` | refused: `restricted mode 'Scan' on 'Find' does not allow store to field 'offs_len'` |
| `nested_trust.ccs` | `Outer*`-first body stores `o->in.done` where `Inner` is `r: *` | accepted. Trust for the outer type extends to an embedded type's fields |
| `addr_peel.ccs` | `int* p = &x.done; *p = 1;` | accepted. The address is the exit, as with `.ptr` |

So an allow-list confines stores at sites and inside bodies of other
modes, and a named mode partitions a type's write surface by concern.
It does not compose through embedding, and it does not see a pointer.

The pointer case is a one-condition extension. The walk already refuses
`&v` on a binder (`lower_forin.cch:530`, "is not a location"). The
allow-list check (`lower_ufcs.cch:2340-2375`) classifies a member use
as load or store through `uf_is_store`; counting a unary `&` on the
member as a store gives "does not allow the address of field" with no
new machinery. After that, `p->done` through a typed pointer is already
checked at the member, so the only exit left is a peel to another type.

cctext count: 71 `RtxDoc*`-first functions in `core/`, 3 in
`frontend/`; 7 stores to `find.*` state fields outside `core/find.ccs`.
