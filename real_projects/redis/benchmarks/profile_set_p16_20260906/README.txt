=== Redis SET isolation + sample (2026-09-06) ===
Commit: a5990c74
Load: 22:27  up  8:32, 1 user, load averages: 2.42 3.23 3.00
Question: per-op tax vs pool-size tax vs August SET shape

===== SET-only idiomatic default   =====
tmp dir: /var/folders/12/04b0tf2d79b7d7wdgzwx12yr0000gn/T/bench_robust.XXXXXX.9TJLNNQgHC
[warmup]
   idiomatic set   rps=2659574.50
[mem] CC.MEMLOG post-warmup
[round 1/5]
   idiomatic set   rps=2777777.75
[round 2/5]
   idiomatic set   rps=2793296.00
[round 3/5]
   idiomatic set   rps=2777777.75
[round 4/5]
   idiomatic set   rps=2732240.50
[round 5/5]
   idiomatic set   rps=2577319.50
[mem] CC.MEMLOG final

== bench_robust summary ==
rounds=5 requests_per_round=500000 clients=50 pipeline=16 mget_keys=8 cc_workers=default
labels=idiomatic include_sketch=0 include_parallel=0 include_go=0
sample_interval=0.05s  samples_taken: idiomatic=22

[SET]
  label          mean   median      min      max   stddev    cv%
  idiomatic    2.732M   2.778M   2.577M   2.793M   0.089M   3.3%

[RESOURCE PEAKS] (sampled every 0.05 s across the whole run)
  label        peak_rss   peak_threads
  idiomatic      13.1MB              4

[MEM GAP] footprint − (map+key+value live); remainder ≈ runtime/fibers/overflow/frag
  phase           footprint    logical        gap      map      key    value     arenas        src
  owner-ready        1.41MB     42.5KB     1.37MB   42.5KB       0B       0B    16.05MB  mach_phys
  post-warmup       12.50MB     2.44MB    10.06MB   1.68MB  781.2KB       0B    17.69MB  mach_phys
  final             12.59MB     2.44MB    10.15MB   1.68MB  781.2KB       0B    17.69MB  mach_phys

== bench_robust summary ==
rounds=5 requests_per_round=500000 clients=50 pipeline=16 mget_keys=8 cc_workers=default
labels=idiomatic include_sketch=0 include_parallel=0 include_go=0
sample_interval=0.05s  samples_taken: idiomatic=22

[SET]
  label          mean   median      min      max   stddev    cv%
  idiomatic    2.732M   2.778M   2.577M   2.793M   0.089M   3.3%

[RESOURCE PEAKS] (sampled every 0.05 s across the whole run)
  label        peak_rss   peak_threads
  idiomatic      13.1MB              4

[MEM GAP] footprint − (map+key+value live); remainder ≈ runtime/fibers/overflow/frag
  phase           footprint    logical        gap      map      key    value     arenas        src
  owner-ready        1.41MB     42.5KB     1.37MB   42.5KB       0B       0B    16.05MB  mach_phys
  post-warmup       12.50MB     2.44MB    10.06MB   1.68MB  781.2KB       0B    17.69MB  mach_phys
  final             12.59MB     2.44MB    10.15MB   1.68MB  781.2KB       0B    17.69MB  mach_phys

[sched_v2 stats] grow (eager<=2 recheck=25us rate=100us/pop/worker depth_x=2 dwell=3 esc=0): requests=3001 stall=0 backlog=0 escalate=0 held=14816 parked=4669 final_threads=2/10

===== SET-only idiomatic w2  env CC_V2_THREADS=2 CC_V2_EAGER_THREADS=2 =====
tmp dir: /var/folders/12/04b0tf2d79b7d7wdgzwx12yr0000gn/T/bench_robust.XXXXXX.CpQJGPZylh
[warmup]
   idiomatic set   rps=2793296.00
[mem] CC.MEMLOG post-warmup
[round 1/5]
   idiomatic set   rps=2840909.00
[round 2/5]
   idiomatic set   rps=2617801.00
[round 3/5]
   idiomatic set   rps=2717391.25
[round 4/5]
   idiomatic set   rps=2673796.75
[round 5/5]
   idiomatic set   rps=2645502.75
[mem] CC.MEMLOG final

== bench_robust summary ==
rounds=5 requests_per_round=500000 clients=50 pipeline=16 mget_keys=8 cc_workers=default
labels=idiomatic include_sketch=0 include_parallel=0 include_go=0
sample_interval=0.05s  samples_taken: idiomatic=22

[SET]
  label          mean   median      min      max   stddev    cv%
  idiomatic    2.699M   2.674M   2.618M   2.841M   0.087M   3.2%

[RESOURCE PEAKS] (sampled every 0.05 s across the whole run)
  label        peak_rss   peak_threads
  idiomatic      13.2MB              4

[MEM GAP] footprint − (map+key+value live); remainder ≈ runtime/fibers/overflow/frag
  phase           footprint    logical        gap      map      key    value     arenas        src
  owner-ready        1.41MB     42.5KB     1.37MB   42.5KB       0B       0B    16.05MB  mach_phys
  post-warmup       12.64MB     2.44MB    10.20MB   1.68MB  781.2KB       0B    17.69MB  mach_phys
  final             12.67MB     2.44MB    10.23MB   1.68MB  781.2KB       0B    17.69MB  mach_phys

== bench_robust summary ==
rounds=5 requests_per_round=500000 clients=50 pipeline=16 mget_keys=8 cc_workers=default
labels=idiomatic include_sketch=0 include_parallel=0 include_go=0
sample_interval=0.05s  samples_taken: idiomatic=22

[SET]
  label          mean   median      min      max   stddev    cv%
  idiomatic    2.699M   2.674M   2.618M   2.841M   0.087M   3.2%

[RESOURCE PEAKS] (sampled every 0.05 s across the whole run)
  label        peak_rss   peak_threads
  idiomatic      13.2MB              4

[MEM GAP] footprint − (map+key+value live); remainder ≈ runtime/fibers/overflow/frag
  phase           footprint    logical        gap      map      key    value     arenas        src
  owner-ready        1.41MB     42.5KB     1.37MB   42.5KB       0B       0B    16.05MB  mach_phys
  post-warmup       12.64MB     2.44MB    10.20MB   1.68MB  781.2KB       0B    17.69MB  mach_phys
  final             12.67MB     2.44MB    10.23MB   1.68MB  781.2KB       0B    17.69MB  mach_phys

[sched_v2 stats] grow (eager<=2 recheck=25us rate=100us/pop/worker depth_x=2 dwell=3 esc=0): requests=0 stall=0 backlog=0 escalate=0 held=0 parked=0 final_threads=2/2

===== SET-only idiomatic w4  env CC_V2_THREADS=4 CC_V2_EAGER_THREADS=4 =====
tmp dir: /var/folders/12/04b0tf2d79b7d7wdgzwx12yr0000gn/T/bench_robust.XXXXXX.AEeBtfvY70
[warmup]
   idiomatic set   rps=2450980.50
[mem] CC.MEMLOG post-warmup
[round 1/5]
   idiomatic set   rps=2538071.00
[round 2/5]
   idiomatic set   rps=2538071.00
[round 3/5]
   idiomatic set   rps=2590673.50
[round 4/5]
   idiomatic set   rps=2577319.50
[round 5/5]
   idiomatic set   rps=2564102.75
[mem] CC.MEMLOG final

== bench_robust summary ==
rounds=5 requests_per_round=500000 clients=50 pipeline=16 mget_keys=8 cc_workers=default
labels=idiomatic include_sketch=0 include_parallel=0 include_go=0
sample_interval=0.05s  samples_taken: idiomatic=23

[SET]
  label          mean   median      min      max   stddev    cv%
  idiomatic    2.562M   2.564M   2.538M   2.591M   0.023M   0.9%

[RESOURCE PEAKS] (sampled every 0.05 s across the whole run)
  label        peak_rss   peak_threads
  idiomatic      13.2MB              6

[MEM GAP] footprint − (map+key+value live); remainder ≈ runtime/fibers/overflow/frag
  phase           footprint    logical        gap      map      key    value     arenas        src
  owner-ready        1.41MB     42.5KB     1.37MB   42.5KB       0B       0B    16.05MB  mach_phys
  post-warmup       12.69MB     2.44MB    10.25MB   1.68MB  781.2KB       0B    17.69MB  mach_phys
  final             12.70MB     2.44MB    10.26MB   1.68MB  781.2KB       0B    17.69MB  mach_phys

== bench_robust summary ==
rounds=5 requests_per_round=500000 clients=50 pipeline=16 mget_keys=8 cc_workers=default
labels=idiomatic include_sketch=0 include_parallel=0 include_go=0
sample_interval=0.05s  samples_taken: idiomatic=23

[SET]
  label          mean   median      min      max   stddev    cv%
  idiomatic    2.562M   2.564M   2.538M   2.591M   0.023M   0.9%

[RESOURCE PEAKS] (sampled every 0.05 s across the whole run)
  label        peak_rss   peak_threads
  idiomatic      13.2MB              6

[MEM GAP] footprint − (map+key+value live); remainder ≈ runtime/fibers/overflow/frag
  phase           footprint    logical        gap      map      key    value     arenas        src
  owner-ready        1.41MB     42.5KB     1.37MB   42.5KB       0B       0B    16.05MB  mach_phys
  post-warmup       12.69MB     2.44MB    10.25MB   1.68MB  781.2KB       0B    17.69MB  mach_phys
  final             12.70MB     2.44MB    10.26MB   1.68MB  781.2KB       0B    17.69MB  mach_phys

[sched_v2 stats] grow (eager<=4 recheck=25us rate=100us/pop/worker depth_x=2 dwell=3 esc=0): requests=0 stall=0 backlog=0 escalate=0 held=0 parked=0 final_threads=4/4

===== SET-only idiomatic w6  env CC_V2_THREADS=6 CC_V2_EAGER_THREADS=6 =====
tmp dir: /var/folders/12/04b0tf2d79b7d7wdgzwx12yr0000gn/T/bench_robust.XXXXXX.u9YyP6RegS
[warmup]
   idiomatic set   rps=2463054.25
[mem] CC.MEMLOG post-warmup
[round 1/5]
   idiomatic set   rps=2439024.50
[round 2/5]
   idiomatic set   rps=2439024.50
[round 3/5]
   idiomatic set   rps=2415459.00
[round 4/5]
   idiomatic set   rps=2427184.50
[round 5/5]
   idiomatic set   rps=2403846.00
[mem] CC.MEMLOG final

== bench_robust summary ==
rounds=5 requests_per_round=500000 clients=50 pipeline=16 mget_keys=8 cc_workers=default
labels=idiomatic include_sketch=0 include_parallel=0 include_go=0
sample_interval=0.05s  samples_taken: idiomatic=24

[SET]
  label          mean   median      min      max   stddev    cv%
  idiomatic    2.425M   2.427M   2.404M   2.439M   0.015M   0.6%

[RESOURCE PEAKS] (sampled every 0.05 s across the whole run)
  label        peak_rss   peak_threads
  idiomatic      13.4MB              8

[MEM GAP] footprint − (map+key+value live); remainder ≈ runtime/fibers/overflow/frag
  phase           footprint    logical        gap      map      key    value     arenas        src
  owner-ready        1.41MB     42.5KB     1.37MB   42.5KB       0B       0B    16.05MB  mach_phys
  post-warmup       12.80MB     2.44MB    10.35MB   1.68MB  781.2KB       0B    17.69MB  mach_phys
  final             12.83MB     2.44MB    10.39MB   1.68MB  781.2KB       0B    17.69MB  mach_phys

== bench_robust summary ==
rounds=5 requests_per_round=500000 clients=50 pipeline=16 mget_keys=8 cc_workers=default
labels=idiomatic include_sketch=0 include_parallel=0 include_go=0
sample_interval=0.05s  samples_taken: idiomatic=24

[SET]
  label          mean   median      min      max   stddev    cv%
  idiomatic    2.425M   2.427M   2.404M   2.439M   0.015M   0.6%

[RESOURCE PEAKS] (sampled every 0.05 s across the whole run)
  label        peak_rss   peak_threads
  idiomatic      13.4MB              8

[MEM GAP] footprint − (map+key+value live); remainder ≈ runtime/fibers/overflow/frag
  phase           footprint    logical        gap      map      key    value     arenas        src
  owner-ready        1.41MB     42.5KB     1.37MB   42.5KB       0B       0B    16.05MB  mach_phys
  post-warmup       12.80MB     2.44MB    10.35MB   1.68MB  781.2KB       0B    17.69MB  mach_phys
  final             12.83MB     2.44MB    10.39MB   1.68MB  781.2KB       0B    17.69MB  mach_phys

[sched_v2 stats] grow (eager<=6 recheck=25us rate=100us/pop/worker depth_x=2 dwell=3 esc=0): requests=0 stall=0 backlog=0 escalate=0 held=0 parked=0 final_threads=6/6

Verdict
- SET-only (no upstream): default 2.778M CV 3.3% final_threads=2/10.
  pin 2: 2.674M; pin 4: 2.564M; pin 6: 2.427M.
  Extra workers hurt. Default is the hold, not the ncpu walk.
- Interleaved GET/INCR overlap in the dated P=16 file is not this class.
  That file fights upstream on a loaded box; SET-only is the absolute.
- vs Sept 5 interleaved idiomatic SET 2.959M: SET-only here is 2.778M
  (~6%). Not a dest-serve rewrite. Work stacks still find_dense + sendto + read.
- sample SET 8s (idiomatic_set.sample): 2 workers. New vs 2026-08-08 SET
  profile: sysmon hits sched_v2_check_deadlock → classify_parked_fibers
  (~130/6945) when idle==n && rq==0 — the I/O-wait steady state. August
  sysmon was almost all ulock_wait. skipped_local=0 (no channel path).
- Samples: idiomatic_set.sample, idiomatic_get.sample, profile_sched.txt.

