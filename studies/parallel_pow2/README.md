# Bend's pow2 in CC: the deniable join against the stated one

`parallel_pow2_rows.shcc` is `perf/parallel_pow2.shcc` with its two
`@errhandler(CCError)` lines removed (seed 414 refuses them as dead over
a join, see `studies/join_dead_handler/`) and four rows added:

| Row | Form |
|-----|------|
| `seq` | plain C recursion |
| `cut` | `@parallel (d > depth-cut)` at every node |
| `spawn` | `@parallel spawn` above the cut, plain C below: meeting admit, never denied |
| `cut_seqbelow` | `@parallel (1)` above the cut, plain C below: the deniable join alone |
| `gate0` | `@parallel (0)` at every node: the construct's false path alone |
| `off` | `#pragma(@parallel) off`: the static denial |

Depth 24 (16.8M leaves), cut 8 (255 spawns), median of 5 inside each
row, three repetitions per configuration. Linux, 4 cores, seed
0.4.0-414. Full rows in `runs_linux_4core_414.txt`.

| Row | default, ms | vs seq | `CC_V2_EAGER_THREADS=4`, ms | vs seq |
|-----|------------:|-------:|----------------------------:|-------:|
| seq | 8.2 | 1.00 | 8.3 | 1.00 |
| cut | 10.6, 11.2, 10.7 | 0.75 | 42.0, 41.5, 41.8 | 0.20 |
| spawn | 2.42, 2.43, 2.45 | 3.4 | 3.29, 2.49, 2.54 | 3.0 |
| cut_seqbelow | 8.96, 9.17, 9.37 | 0.90 | 9.04, 9.27, 9.07 | 0.91 |
| gate0 | 7.25, 7.36, 7.12 | 1.15 | 7.10, 7.17, 7.13 | 1.16 |
| off | 7.52, 7.97, 7.72 | 1.07 | 7.52, 7.81, 7.63 | 1.08 |

Other single runs: `cut` with the gate off (`CC_PAR_ADAPT=0`) 13.0 ms;
`cut_seqbelow` with `CC_V2_WAKE_SKIP_DEPTH=1000000` 9.5 ms; on two
occasions out of about a dozen `cut_seqbelow` ran at 2.5 to 2.7 ms
(3.2x), so the machinery can run the same 255 arms in parallel and
usually does not.

What the rows say:

- The false path costs nothing. `gate0` and `off` are faster than the
  hand-written C, so a `@parallel (pred)` left in a recursion is free
  where the predicate is false.
- The stated schedule works. `spawn` is 3.4x on 4 cores, stable across
  every run and configuration.
- The deniable join does not deliver here. Its 255 arms are the same
  work as `spawn`'s, and it lands at 0.9x with the whole cost of the
  construct at the cut and none of the parallelism, in both
  configurations. With the construct at every node it is 0.75x, and
  with four eager workers it is 0.20x, repeatably.
- The author's receipt on a 10-core Darwin machine
  (`perf/baselines/parallel_hello_20260904_grow_park.txt`, same tree
  shape) has `cut` at 3.3x. The deniable path is platform and
  scheduler dependent; the meeting-admit path is not.

The gate is not the cause: with `CC_PAR_ADAPT_DEBUG=1` the cut site
reads `real` (203 us arms), and turning the gate off does not help. The
wake-skip depth is not the cause. Where the time goes inside the
deniable join on this platform is a runtime question this study does
not answer; it only shows that the reconstruction (deny, inline on
deny, grow on demand) loses to the statement (`spawn`) by 4x to 17x on
the same arms, and that nothing in the program text explains why one
form is chosen over the other.
