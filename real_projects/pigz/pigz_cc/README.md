# pigz_cc

This directory contains the feature-complete Concurrent-C implementation of `pigz`.

## Relationship to `pigz_idiomatic.ccs`
Compress of a seekable file is the `pigz_idiomatic` loop: one ticket per
block, `@parallel wait` + `@stage` for the 32 KiB dict hop and the
ordered CRC/write, `cache (zs)` for a warm `z_stream`. Stdin (no
`n`, no `pread`) is the same helpers in a sequential while-read.
Decompress is still a three-stage I/O pipeline.

`../pigz_channel.ccs` is the older ordered-channel sketch (independent
members, no dict). This directory is the product binary.

## Goals
- 1:1 feature parity with original `pigz`.
- The wait-for loop is the program; nurseries are not the compress wire.

## Structure
- `pigz_cc.ccs`: Main entry point and implementation.
- `pigz_cc.cch`: Types, CRC/Adler combine, CLI-adjacent helpers.
