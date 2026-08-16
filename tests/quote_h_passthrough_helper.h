/* QUOTE_H_PASSTHROUGH_MARK — body must not appear in lowered C. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Function type (not pointer) — CC parse rejects this if spliced. */
typedef void quote_h_passthrough_fn(void *);

static inline int quote_h_passthrough_add(int a, int b) {
    return a + b;
}

#ifdef __cplusplus
}
#endif
