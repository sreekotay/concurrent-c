# Interop baselines

Point-in-time snapshots of `perf/py_baseline.ccs`, collected so a change to
the boundary can be read against history instead of memory.

To add one:

    ./cc/bin/ccc run perf/py_baseline.ccs > /tmp/pb.txt \
      && { echo "# perf/py_baseline.ccs snapshot"; \
           echo "# date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"; \
           echo "# host: $(uname -srm)"; \
           echo "# cc:   $(git rev-parse --short HEAD)"; \
           echo "#"; cat /tmp/pb.txt; } \
           > perf/baselines/py_baseline_$(date +%Y%m%d).txt

Reading them:

- `RESULT` lines are the machine surface — grep them, diff them across
  snapshots. Everything else is for people.
- The **ratios are the stable part.** Absolute ns/call moves with host load
  (this collection includes container-hosted runs); a mode measured against
  its native control on the same run mostly cancels that out. Judge a change
  by its ratio drift, and only then by absolutes from comparable hosts.
- The outbound scalar modes (`cc_to_py_*`) are the noisiest — they are the
  slowest calls and therefore the shortest sample loops. Treat a <15% move
  there as weather.

The benchmark exits non-zero on any cross-mode result mismatch, so a snapshot
that exists is one whose numbers were measuring correct marshalling.
