// Minimal "target graph" example for ccc build.
// Run from repo root:
//   ./cc/bin/ccc build --build-file examples/build_graph/build.cc
//   ./cc/bin/ccc build run --build-file examples/build_graph/build.cc
//   ./cc/bin/ccc build multi --build-file examples/build_graph/build.cc

CC_DEFAULT hello

CC_OPTION target Choose target triple (not implemented yet; demo option)
CC_OPTION optimize Debug|ReleaseSafe|ReleaseFast|ReleaseSmall (not implemented yet; demo option)

CC_TARGET hello exe main.ccs
CC_TARGET multi exe multi_main.ccs add.ccs


