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

CC_TARGET hello exe main.ccs helper.c


