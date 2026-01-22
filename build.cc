// Root build file for Concurrent-C examples, stress tests, and perf benchmarks.
//
// Usage:
//   ccc build                           # build default (hello example)
//   ccc build run hello                 # build and run hello example
//   ccc build run spawn_storm           # build and run a stress test
//   ccc build run perf_channel          # build and run a perf benchmark
//
// List all targets:
//   ccc build --help --build-file build.cc
//
// Note: The compiler and conformance test suite use Make (see Makefile).
//       This build.cc is for examples, demos, and benchmarks only.

CC_DEFAULT hello

// ============================================================================
// Examples - introductory demos
// ============================================================================

CC_TARGET hello exe examples/hello.ccs

// Recipes - small focused examples of specific features
CC_TARGET recipe_arena exe examples/recipe_arena_scope.ccs
CC_TARGET recipe_async exe examples/recipe_async_await.ccs
CC_TARGET recipe_pipeline exe examples/recipe_channel_pipeline.ccs
CC_TARGET recipe_defer exe examples/recipe_defer_cleanup.ccs
CC_TARGET recipe_capture exe examples/recipe_explicit_capture.ccs
CC_TARGET recipe_fanout exe examples/recipe_fanout_capture.ccs
CC_TARGET recipe_http exe examples/recipe_http_get.ccs
CC_TARGET recipe_optional exe examples/recipe_optional_values.ccs
CC_TARGET recipe_result exe examples/recipe_result_error_handling.ccs
CC_TARGET recipe_tcp exe examples/recipe_tcp_echo.ccs
CC_TARGET recipe_timeout exe examples/recipe_timeout.ccs
CC_TARGET recipe_worker exe examples/recipe_worker_pool.ccs

// Multi-file examples (use their own build.cc for full control)
// CC_TARGET multi exe examples/multi/main.ccs examples/multi/add.ccs
// CC_TARGET mixed exe examples/mixed_c/main.ccs examples/mixed_c/helper.c

// ============================================================================
// Stress Tests - concurrent correctness under load
// ============================================================================

CC_TARGET stress_arena exe stress/arena_concurrent.ccs
CC_TARGET stress_async exe stress/async_await_flood.ccs
CC_TARGET stress_block exe stress/block_combinators_stress.ccs
CC_TARGET stress_cancel exe stress/cancellation_close_race.ccs
CC_TARGET stress_channel exe stress/channel_flood.ccs
CC_TARGET stress_closure exe stress/closure_capture_storm.ccs
CC_TARGET stress_deadline exe stress/deadline_race.ccs
CC_TARGET stress_deadlock exe stress/deadlock_detect_demo.ccs
CC_TARGET stress_fanout exe stress/fanout_fanin.ccs
CC_TARGET stress_nursery exe stress/nursery_deep.ccs
CC_TARGET stress_pipeline exe stress/pipeline_long.ccs
CC_TARGET stress_mixed exe stress/spawn_async_mixed.ccs
CC_TARGET stress_spawn exe stress/spawn_storm.ccs
CC_TARGET stress_unbuf exe stress/unbuffered_rendezvous.ccs
CC_TARGET stress_worker exe stress/worker_pool_heavy.ccs

// ============================================================================
// Performance Benchmarks
// ============================================================================

CC_TARGET perf_async exe perf/perf_async_overhead.ccs
CC_TARGET perf_channel exe perf/perf_channel_throughput.ccs
CC_TARGET perf_match exe perf/perf_match_select.ccs
CC_TARGET perf_zerocopy exe perf/perf_zero_copy.ccs
