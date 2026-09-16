# Stage name probe: what the detector says

`bad_name.ccs` puts ticket `i` at stage name `(i*5) % 8` on one gate:
names 0,5,2,7,4,1,6,3, so ticket 1 waits on name 5, which ticket 3
passes (its name is 2 → pass touches 3? no: ticket 3 sits at name 7 and
passes 8). Nobody passes 5. Run with `CC_DEADLOCK_PERSIST_MS=300`.

| Schedule | Result | Message |
|----------|--------|---------|
| parallel (`bad_name 1`) | detector fires after the latch, exit 124 | two fibers `reason=exclusive_when obj=…`; no gate, no name, no ticket, no source line; "Common causes" lists channels and joins only |
| sequential (`bad_name 0`, `seq` false) | hangs; killed by the 12 s timeout | nothing. The caller parks on the host-thread path (`wake_primitive_wait`), which the detector does not scan |

Also found: a stage argument that names a body local (`int name = …;
@stage (ts, 0, name)`) fails in the host compiler with `'name'
undeclared`, because the exit's discharge (`fail`/`pass`) is emitted
outside the body scope. The lowerer should refuse it with its own
diagnostic or hoist the expression.

Toolchain: seed `0.4.0-404`, built in this tree.
