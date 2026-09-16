# Lifetime boundary probes

Small programs at the edge of what the ownership step (`cc/lower/lower_own.cch`)
and the closure step (`lower_closures.cch`) check. Each compiles alone with
`./cc/bin/ccc build --no-cache FILE`. Results below are from seed 0.4.0-404.

Prefixes: `c` control (documented behavior, holds), `u` the taught UFCS spelling
of a control, `t` an operation the spec lists as epoch-ending but the step's
table does not, `h` a holder the model has no fact for, `f` a false refusal.

| File | Shape | Result | Reading |
|------|-------|--------|---------|
| c2_pin | nursery spawn captured a view; reset | refused | pin holds |
| c3_reset_borrow | reset with a derived borrow in scope | refused | holds |
| c4_copy_propagates | view copied to an outer local, inner leaves scope, reset | refused | copies are tracked |
| c1_same_frame_nursery_ok | stack slice captured into a same-frame nursery spawn | accepted | correct: the nursery joins before the buffer dies; spec §capture example shows this as an error |
| u1_reset_borrow_ufcs | c3 spelled `a.alloc_slice_bytes(n)` / `a.reset()` | accepted | the step runs before UFCS and matches C names only; the taught spelling is unchecked |
| t1_restore_borrow | checkpoint, mint a view, restore, use | accepted | restore / try_restore not in the epoch-op table; spec lists them |
| t2_detach_borrow | mint a view, detach, use | accepted | detach not in the table; spec lists it |
| f1_lexical_not_liveness | use view, reset, mint a fresh view | refused | scope, not last use |
| h1_outer_nursery_inner_stack | outer nursery, inner-block stack buffer, spawn | accepted | frame < join set; the CVE-2023-54235 shape |
| h2_leave_stack | stack slice into a nursery, then `leave` | accepted | leave is an escape |
| h3_field_holder | view stored into a struct field, reset | accepted | field is a holder with no lifetime fact |
| h4_call_result | view returned by an arena-last callee, reset | accepted | arena-last could be inferred |
| h5_unwrap_result | same through `!>` | accepted | same |
| h6_return_stack_view | return a view of a stack arena | accepted; silent at run time | frame < caller |
| h7_map_keeps_key | request-arena key inserted into a store-arena map | accepted | kept parameter |
| h8_scratch_kept | scratch string kept through a global by a callee | accepted | kept parameter |
| h9_dest_admit_no_pin | view in an inner block admitted onto a live dest; reset | accepted | dest does not pin; nursery does |
| h10_send_arena_then_reset | aggregate with an arena field sent; sender resets | accepted | send should move the arena binding |
| h11_arena_as_field | arena held as a struct field; view minted; field reset | accepted | field-path arena is not a name |
| h12_handle_alias_segv | handle copied; destroyed through the copy; original used | accepted; SIGSEGV | alias untracked; freed host dereferenced |
