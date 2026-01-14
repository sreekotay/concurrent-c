// Build constants example.
// Demonstrates CC_CONST for compile-time constants and CC_OPTION for help.
//
// Run: ./cc/bin/ccc build run --build-file examples/build_stub/build.cc
// Override: ./cc/bin/ccc build run --build-file examples/build_stub/build.cc -DNUM_WORKERS=8

CC_DEFAULT main

CC_CONST DEBUG 1
CC_CONST USE_TLS 1
CC_CONST NUM_WORKERS 4

CC_OPTION NUM_WORKERS Number of worker threads to use

CC_TARGET main exe main.ccs

