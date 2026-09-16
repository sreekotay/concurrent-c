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
undeclared`. The stage's arguments are spelled four times
(`lower_parallel.cch:3308-3345`): at the wait, at the pass, and twice
more at the runner's one exit, where every stage the body has not
passed is discharged so a parked successor wakes. That exit is outside
the body's scope, and it also discharges stages the body never reached
(a `break` or an error before the stage), whose arguments were never
evaluated. So a name that depends on a body local is not lowerable in
general: the exit could not compute it for an unreached stage. The fix
is a lowerer diagnostic, not a hoist: a stage name may read the loop
variable, the `worker` binder, and captured names of the enclosing
frame, and nothing declared in the body. A second, smaller defect rides
along: the arguments are evaluated more than once, so an expression
with a side effect (`next++`) names two different cells at wait and
pass. Evaluate-once temporaries at the stage fix that for reached
stages; the diagnostic covers the rest.

Toolchain: seed `0.4.0-404`, built in this tree.
