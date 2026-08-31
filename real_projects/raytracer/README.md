# Weekend raytracer

Peter Shirley's [Ray Tracing in One Weekend](https://raytracing.github.io/books/RayTracingInOneWeekend.html)
final scene, three times: sequential C, Concurrent-C `@parallel for` over
scanlines, and Go with one goroutine per row.

The book is the algorithm. This folder is the race: same spheres, same
camera, same per-pixel LCG, a checksum of the gamma-encoded RGB, and a
wall-clock.

```bash
./real_projects/raytracer/compare.sh --smoke   # 48×27, 2 spp
./real_projects/raytracer/compare.sh           # 400×225, 10 spp, depth 20
RT_WIDTH=1200 RT_SAMPLES=10 RT_DEPTH=20 ./real_projects/raytracer/compare.sh
```

`RT_PPM=out/scene.ppm` writes a binary P6 next to the timed run (set it
on a single binary, not on `compare.sh`, if you want one file).

## Fairness

All three ports share:

- the book's cover scene (22×22 small spheres + ground + glass / lambert / metal)
- camera `lookfrom (13,2,3)`, vfov 20, defocus 0.6, focus 10, 16:9
- a fixed LCG for world construction (seed 1) and a per-pixel LCG
  seeded from `(x, y)` — not `rand()`, so parallel rows match sequential
- gamma-2 byte conversion and an FNV-1a checksum of the framebuffer

C is the sequential reference (`cc -O2`). CC defaults to
`@parallel for (y in 0..h)`; `RT_SEQ=1` is the same loop without spawn.
Go defaults to one goroutine per scanline; `RT_SEQ=1` is the ordinary
`for`.

`@parallel for` bisects the row range. Writes are disjoint. The world
and camera are read-only.

Go is not under-built (`go build` is the optimizing compile; C/CC are
`-O2`). Sequential Go is slower because clang inlines `sphere_hit` into
the 485-sphere walk and Go's inliner will not (`sphereHit` cost 347,
budget 80) — every sphere is a call plus, before the tight slice, a
bounds check. Parallel speedups are similar (about 6×), so the spawn
model is not the gap. Do not hand-inline the hit test to chase C.

## Layout

| file | role |
|---|---|
| `rt.c` | sequential C reference |
| `rt.ccs` | CC port; `@parallel for` over rows |
| `go/rt.go` | Go port; per-row goroutines |
| `compare.sh` | build, run five rows, demand matching checksums |
| `COPYING.txt` | Shirley's CC0 notice |

Book defaults for the cover image are 1200×675, 10 samples, depth 20.
The compare default is the same samples and depth at 400×225 so a run
finishes on a laptop. Smoke is 48×27 / 2 / 8 for the test suite.

## Cost

Snapshot from `./compare.sh` on Darwin arm64, 400×225, 10 spp, depth 20
(raw output under `benchmarks/`). `vs_c` is C sequential time over that
row — above 1× means faster than the C reference.

| row | time | vs C seq |
|---|---|---|
| C seq | 1000 ms | — |
| CC seq | 1007 ms | 0.99× |
| CC `@parallel for` | 160 ms | **6.27×** |
| Go seq | 2150 ms | 0.47× |
| Go per-row goroutines | 421 ms | 2.38× |

Same framebuffer checksum on all five rows (`0xba39b7cb028e517f`).
Smoke (48×27) is too small to trust for speed; use it for the checksum
only. Refresh the table from a run on your machine.
