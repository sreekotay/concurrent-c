#ifndef CC_BUILD_BUILD_H
#define CC_BUILD_BUILD_H

#include <stddef.h>

#include "comptime/symbols.h"

typedef struct {
    const char* os;
    const char* arch;
    const char* abi;
    const char* endian;
    int ptr_width;
} CCBuildTarget;

typedef struct {
    const CCBuildTarget* target;
    // Placeholder for future whitelisted env; kept NULL/pure for now.
    const char* const* envp;
} CCBuildInputs;

typedef struct {
    const char* name;
    const char* help;
} CCBuildOptionDecl;

typedef enum {
    CC_BUILD_TARGET_EXE = 1,
    CC_BUILD_TARGET_OBJ = 2,
} CCBuildTargetKind;

typedef struct {
    const char* name;
    CCBuildTargetKind kind;
    const char** srcs;      // heap array of heap strings
    size_t src_count;
    const char** deps;      // heap array of heap strings (target names)
    size_t dep_count;
    const char* out_name;   // optional output binary name (heap)
    const char* target_triple; // optional --target value (heap)
    const char* sysroot;    // optional --sysroot value (heap)
    const char* install_dest; // optional install destination (heap)
    const char** include_dirs; // heap array of heap strings (resolved by driver relative to build.cc dir)
    size_t include_dir_count;
    const char** defines;   // heap array of heap strings (NAME or NAME=VALUE)
    size_t define_count;
    const char** libs;      // heap array of heap strings (either "m" or "-lm" style)
    size_t lib_count;
    const char* cflags;     // optional raw flags string (heap)
    const char* ldflags;    // optional raw flags string (heap)
} CCBuildTargetDecl;


// Stub loader: if build.cc exists, return a fixed const set; else empty.
// Returns 0 on success, non-zero on error. The returned bindings are owned
// by the caller (no allocations are performed here).
int cc_build_load_consts(const char* build_path, const CCBuildInputs* inputs, CCConstBinding* out_bindings, size_t* out_count);

// Optional: enumerate build options declared in build.cc via lines like:
//   CC_OPTION <NAME> <HELP...>
// Returns 0 on success. The returned strings are heap-allocated; caller must free via cc_build_free_options().
int cc_build_list_options(const char* build_path, CCBuildOptionDecl* out_opts, size_t* out_count, size_t max);
void cc_build_free_options(CCBuildOptionDecl* opts, size_t count);

// Targets (Zig-ish): lines in build.cc like:
//   CC_DEFAULT <NAME>
//   CC_TARGET <NAME> exe <src1> <src2> ...
//   CC_TARGET <NAME> obj <src1> <src2> ...
//   CC_TARGET_DEPS <NAME> <dep1> <dep2> ...
//   CC_TARGET_OUT <NAME> <binname>
//   CC_TARGET_TARGET <NAME> <triple>
//   CC_TARGET_SYSROOT <NAME> <path>
//   CC_INSTALL <NAME> <dest>
// Returns 0 on success. The returned strings/arrays are heap-allocated; caller must free via cc_build_free_targets().
int cc_build_list_targets(const char* build_path, CCBuildTargetDecl* out_targets, size_t* out_count, size_t max, char** out_default_name);
void cc_build_free_targets(CCBuildTargetDecl* targets, size_t count, char* default_name);

#endif // CC_BUILD_BUILD_H

