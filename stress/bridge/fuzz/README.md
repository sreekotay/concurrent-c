# Wire-codec libFuzzer (no Node)

Standalone C decoder for the isolated-bridge value tags (`$shm` / `$ta` /
`$h` / `$nf`, nested list/dict). Paths for `$shm` must stay under a sandbox
directory (basename only) — rejects `..` and reserved `$` dict keys.
Allocation is a **chunked** bump arena (new chunks only; never `realloc` a
live chunk) so nested list/dict decode cannot UAF.

```bash
./scripts/fuzz_wire_codec.sh --smoke   # no libFuzzer RT needed
FUZZ_SECONDS=60 ./scripts/fuzz_wire_codec.sh
```

On macOS, the libFuzzer run uses Docker (Xcode clang often lacks the fuzzer
runtime). CI / Linux builds with host `clang -fsanitize=fuzzer,address`.

Artifacts: `out/fuzz/wire_codec/` (corpus, crashes, `last_run.log`).

Plan + receipts: [`docs/sanitizers.md`](../../../docs/sanitizers.md).
