# Project Proposal: Porting Curl's Asynchronous DNS Resolver

This project aims to test the interop and build system practicality of Concurrent-C by porting a specific, high-value component of `libcurl`.

## Target: `lib/asyn-thrdd.c`

The `asyn-thrdd.c` file in `curl` implements an asynchronous DNS resolver using standard OS threads (pthreads or Windows threads). It spawns a thread for each `getaddrinfo` call to avoid blocking the main event loop.

### Why this is a great target:

1.  **Surgical Interop:** It's a self-contained file (approx 500-800 lines). We can replace the thread-spawning logic with Concurrent-C fibers and a `@nursery` without rewriting the rest of `curl`.
2.  **Build System Test:** `curl` uses a complex Autotools/CMake build system. Integrating `ccc` to compile this one file and link it into `libcurl.so` is the ultimate test of Concurrent-C's "brownfield" integration capabilities.
3.  **Structural Safety:** DNS resolution is a common source of memory leaks and use-after-free bugs during cancellation. Using `@arena` for the lifetime of a DNS request and `@nursery` for the resolver fiber ensures deterministic cleanup.
4.  **Fiber Efficiency:** Moving from OS threads to fibers reduces the overhead of concurrent DNS lookups, especially when many transfers are initiated simultaneously.

## Implementation Strategy:

1.  **Build Integration:** Modify the `curl` build process to use `ccc` for `asyn-thrdd.c`.
2.  **Fiber Replacement:** Replace `Curl_thread_create` and thread-joining logic with `spawn` and `@nursery`.
3.  **Arena Management:** Use a `CCArena` to hold the `addrinfo` results and other per-request metadata, ensuring they are freed exactly when the resolver fiber completes or is cancelled.
4.  **Event Loop Integration:** Ensure the fiber-based resolver correctly signals the main `curl` event loop upon completion (likely via the existing socket-pair or pipe signaling mechanism).

## Success Metrics:

- **Correctness:** Pass the existing `curl` test suite (specifically the 500+ series tests for DNS).
- **Build Practicality:** Successfully link the `ccc`-compiled object into a standard `libcurl` build.
- **Performance:** Measure the overhead of spawning 100+ concurrent DNS lookups compared to the original thread-based implementation.
