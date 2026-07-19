# ffc.h

C99 single-header port of [fast_float](https://github.com/fastfloat/fast_float)
([kolemannix/ffc.h](https://github.com/kolemannix/ffc.h)).

Licenses: Apache-2.0 / MIT / BSL-1.0 (see headers in `ffc.h`).

## Use in Concurrent-C

- Include: `#include <ccc/vendor/ffc.h>` (symlink to this file).
- Define `FFC_IMPL` in exactly one translation unit — `cc/runtime/arena_state.c`
  already does this for benches and the aggregated runtime.
- Prefer `FFC_PRESET_JSON` for JSON number text (`JsonNode_as_f64`, schema
  `float`/`double` binds). Structural scan still borrows number spans by default.
