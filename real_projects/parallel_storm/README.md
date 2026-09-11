# parallel_storm

Walkable hello-storm leaf: infinite lattice of spheres on a checkerboard,
first person. `hit` is a sequential fold (XZ DDA over the lattice — no
sphere array). The scene is one owner; tiles write disjoint framebuffer
slots. Raylib is fetched into `vendor/`, not committed.

Two pages, same leaf:

| binary | page | what it collects |
|---|---|---|
| `storm` | `storm.ccs` | pixels into a flat buffer |
| `storm_tile` | `storm_tile.ccs` | same 4-way join, `Child` tree, then flatten, then drop |

`make run` is still the buffer renderer.

Three forks on the buffer page:

| `STORM_MODE` | fork |
|---|---|
| `for` (default) | `@parallel for` over scanlines |
| `unbound` | quadtree, `@parallel { }` at every split |
| `quad` | same tree, `@parallel (d < STORM_CUT)` (default cut 3) |

The tile page is the same quadtree fork as buffer `unbound` / `quad`
(not scanlines). Four-way join returns a `Child` (inline `pix`, heap
`Tile` only at a split), flatten writes RGBA, drop walks. Spawned arms
bump exclusive heap children of the frame arena; a per-frame checkpoint
restores. Default framebuffer is 1024², same as the buffer page.

Raylib is a **not-in-git** fetch (`vendor/raylib`, gitignored). The CC page
does not include `raylib.h` — `storm_rl.c` is the window.

```bash
make
make run
make run-tile

# or:
./make.shcc @
./make.shcc @build
./make.shcc @run
./make.shcc @run_unbound
./make.shcc @run_quad
./make.shcc @run_tile
./make.shcc @run_tile_unbound
```

From this folder: `make run`, or `STORM_MODE=unbound make run`.
Tile: `make run-tile`, or `STORM_MODE=unbound make run-tile`.

Click to look. Esc releases the mouse. WASD move, shift sprint, Q quit.

| env | default | |
|---|---|---|
| `STORM_W` / `STORM_H` | 1024×1024 | framebuffer |
| `STORM_WIN_W` / `STORM_WIN_H` | 1024×1024 | window |
| `STORM_GRID` | 64 | XZ DDA march steps (sphere lattice) |
| `STORM_MODE` | `for` (`storm`) / `quad` (`storm_tile`) | see above |
| `STORM_CUT` | 3 | quadtree spawn levels |
| `STORM_SEQ=1` | 0 | sequential, no spawn |
| `STORM_SHADE=0` | 1 | skip hit/shade (cheap leaf); isolate tree/FB |
| `STORM_FB_GAMMA=0` | 1 | skip `sqrt` in `put_rgb` (linear bytes) |
| `STORM_DROP` | `par` | `storm_tile` only: `par` / `seq` |
| `STORM_FLAT` | `seq` | `storm_tile` only: `par` / `seq` |

The checksum join that started this is still `perf/parallel_trace.ccs`.
This folder is the same leaf, on screen.
