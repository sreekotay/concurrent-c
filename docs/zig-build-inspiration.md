### Zig build system inspiration for `ccc build`

This note captures the **useful patterns from Zig’s `zig build` / `build.zig`** that we can mirror in Concurrent‑C’s `ccc build`, while keeping outputs **plain C** and staying friendly to external build systems.

---

### The Zig model (what’s worth copying)

- **Build is a DAG of steps**
  - A `build.zig` file defines a graph of *steps* (compile, install, run, test, custom tasks).
  - `zig build <step>` executes a named step; steps can depend on other steps.
  - Zig can run independent steps concurrently and can print a step summary (`--summary all`).

- **Standard options with `-D`**
  - `build.zig` typically registers “standard” options:
    - `-Dtarget=...` (target triple / target selection)
    - `-Doptimize=Debug|ReleaseSafe|ReleaseFast|ReleaseSmall`
  - Projects can add their own options (e.g. `-Dexe_name=...`) and these show up in `zig build --help`.

- **First-class “build, run, test” workflows**
  - Build: produce artifacts in a well-known output tree (`zig-out/...`).
  - Run: define an explicit `run` step that depends on the build artifact (`addRunArtifact`).
  - Test: define test artifacts/steps that are executed via the same build runner.

- **Compiler-integrated caching**
  - Zig’s build runner uses the compiler’s cache to avoid redundant work, keyed by inputs, options, and toolchain.

Reference example from Zig guide (`build.zig` pattern):
- `b.addExecutable(.{ .name=..., .root_source_file=..., .target=b.standardTargetOptions(.{}), .optimize=b.standardOptimizeOption(.{}) })`
- `b.installArtifact(exe)`
- `run_exe = b.addRunArtifact(exe)`; `run_step = b.step("run", ...)`; `run_step.dependOn(&run_exe.step)`

---

### The CC mapping (our equivalent)

We can mirror the above *without* embedding Zig or changing the “emit C” contract.

- **`build.cc` defines a build graph**
  - CC’s `build.cc` becomes the equivalent of Zig’s `build.zig`: it defines **targets/artifacts and steps**.
  - `ccc build <step>` runs a named step, with a default step (e.g. `default` or `install`).

- **`-D` options become first-class build settings**
  - Keep using `-DNAME[=VALUE]`, but standardize a few “global” options:
    - `-Dtarget=<triple>` (or `--target`, but Zig’s pattern suggests `-Dtarget` integrates naturally with build scripts)
    - `-Doptimize=Debug|ReleaseSafe|ReleaseFast|ReleaseSmall`
    - `-Dout_dir=...` (optional, but useful early)
  - `ccc build --help` should list project-specific options declared by `build.cc`.

- **Install/Run/Test are steps, not ad-hoc flags**
  - `ccc build` produces artifacts in `out/` (our current convention), but step names provide workflows:
    - `ccc build` (default step)
    - `ccc build run`
    - `ccc build test`

- **Cache as a first-class design constraint**
  - We should add a simple, inspectable cache key scheme early (even if it’s just “skip if output newer than inputs” initially).
  - Longer-term: a content-hash cache keyed by:
    - input `.ccs` contents
    - `build.cc` contents + declared options
    - `ccc` version + TCC patch version (or git hash)
    - toolchain selection (`$CC`, flags, target/sysroot)

---

### Concrete next implementation steps (incremental)

1) **Add “named steps” to `ccc build`**
   - Minimal: `ccc build run <input.ccs>` behaves like `ccc build --link ...` then runs the produced binary.
   - Keep current flags; steps are just shortcuts with consistent semantics.

2) **Expose standard build options as `-D`**
   - Teach `build.cc` to read `target`/`optimize` as consts (Phase 0: just parse `CC_CONST` values; later: real comptime).

3) **Add a small build summary**
   - Similar to Zig’s `--summary`, print what got built / skipped, and why.

---

### Good ideas from other modern C build systems (worth copying)

Even though we’re aiming for a Zig-like “compiler-as-build-runner” feel, the best modern C build systems converge on a few practical ideas:

- **Meson**
  - Strong **target-first** UX: targets own sources, include dirs, defines, and link deps.
  - Fast incremental builds with good defaults.
  - Takeaway for CC: keep flags/deps *attached to targets*, not global ambient state.

- **Modern CMake (target-based)**
  - Everything hangs off targets (`target_sources`, `target_include_directories`, `target_compile_definitions`, `target_link_libraries`).
  - Takeaway for CC: `build.cc` should eventually describe target-local `includes/defines/cflags/ldflags/libs`.

- **Bazel / Buck2**
  - Explicit deps and hermetic builds enable robust caching.
  - Takeaway for CC: long-term, make dependency inputs explicit (including generated headers) so caching is sound.

### Mixed CC + C builds (the model)

Zig’s approach maps cleanly:
- `.ccs` sources: **lower** to generated C with `#line`, then compile to `.o`.
- `.c` sources: compile directly to `.o` (no CC lowering).
- headers (`.h` / `.cch`): never “built” as standalone artifacts; they’re discovered via include paths.

The missing piece for correctness is **header dependency tracking**:
- Emit depfiles via the host C compiler (e.g. `-MMD -MF out/obj/<stem>.d`).
- Use depfile contents/mtimes to decide when to rebuild a `.o`.


