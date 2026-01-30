# pigz_cc Completion Plan

This plan outlines the steps to achieve 100% functional parity between `pigz_cc` and Mark Adler's original `pigz.c`.

## Phase 1: Zopfli & Format Correctness
- [ ] **Zopfli Support (`-11`)**:
    - Integrate `libzopfli`.
    - Implement Zopfli compression path in `compress_block`.
    - Verify compression ratio and performance (it will be slow, as expected).
- [ ] **Adler-32 Combination**:
    - Implement Adler-32 combination logic for Zlib format.
    - Replace CRC32 placeholder in Zlib trailer with correct Adler-32.
- [ ] **Header Metadata**:
    - Preserve original filename in Gzip header.
    - Preserve modification time (mtime) in Gzip header.
    - Support custom comments (`-C`, `--comment`).

## Phase 2: Rsyncable & Advanced CLI
- [ ] **Rsyncable Support (`-R`)**:
    - Implement rolling hash for content-defined block boundaries.
    - Ensure compatibility with parallel execution.
- [ ] **Stdin/Stdout Streaming**:
    - Support `-` as a filename for stdin.
    - Default to stdin -> stdout if no files are provided.
- [ ] **System Integration**:
    - Implement dynamic block size (`-b`, `--blocksize`).
    - Implement deep sync (`-Y`, `--synchronous`) using `fsync`.
    - Implement independent blocks (`-i`, `--independent`).

## Phase 3: Decompression & Tools
- [ ] **Decompression Engine (`-d`)**:
    - Implement parallel decompression using `inflate`.
    - Support decompressing Gzip, Zlib, and LZW formats.
- [ ] **Integrity Testing (`-t`)**:
    - Implement verification of compressed files without full decompression.
- [ ] **Listing (`-l`)**:
    - Implement metadata listing for compressed files.
- [ ] **Zip Format (`-K`)**:
    - Support producing PKWare zip files (single entry).

## Verification & Benchmarking
- [ ] Continuous benchmarking against `pigz.c`.
- [ ] Correctness verification for all formats (Gzip, Zlib, Zip).
- [ ] Stress testing with deep directory structures and large files.
