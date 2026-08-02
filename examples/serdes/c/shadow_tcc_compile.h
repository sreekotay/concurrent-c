/* In-process libtcc compile of shadow emit buffer → executable.
 * No intermediate .c on disk. Runtime is a host-built shared lib
 * (Darwin: TCC cannot link clang .o; dylib works).
 *
 * The runtime load name is the leaf `libcc_runtime` (no absolute path,
 * no @rpath). Run with DYLD_LIBRARY_PATH / LD_LIBRARY_PATH = lib dir. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Compile `c_src` (NUL-terminated C) to executable at `out_exe`.
 * `tcc_lib_dir` — dir with libtcc1.a / TCC include (may be NULL → env/defaults).
 * `inc_dirs` / `ninc` — -I paths for the product C.
 * `runtime_lib_dir` — directory containing libcc_runtime.{dylib,so}.
 * Returns 0 on success. */
int shadow_tcc_compile_exe(const char* c_src, const char* out_exe,
                           const char* tcc_lib_dir,
                           const char* const* inc_dirs, int ninc,
                           const char* runtime_lib_dir);

#ifdef __cplusplus
}
#endif
