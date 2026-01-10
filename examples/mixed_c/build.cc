// Mixed CC + C build example.
//
// This demonstrates:
// - Compiling a CC entrypoint (.ccs) that includes a local C header ("helper.h")
// - Compiling a plain C translation unit (helper.c)
// - Linking both into one executable
//
// Run:
//   ./cc/bin/ccc build --build-file examples/mixed_c/build.cc --summary

CC_DEFAULT hello

CC_TARGET helper obj helper.c
CC_TARGET hello exe main.ccs

// Per-target properties:
// - include dirs are relative to this build.cc's directory
CC_TARGET_INCLUDE hello .
// - defines become -DNAME[=VALUE]
CC_TARGET_DEFINE hello CCC_MIXED_C_EXAMPLE=1

// - deps are other CC_TARGET names (their sources + build props are included)
CC_TARGET_DEPS hello helper

// - default output name under bin/ (optional; defaults to target name)
CC_TARGET_OUT hello hello

// - used by `ccc build install hello` (optional)
CC_INSTALL hello bin/hello_installed


