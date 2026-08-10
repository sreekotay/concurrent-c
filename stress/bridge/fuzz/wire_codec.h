/* Standalone wire-value codec for the isolated bridge tags
 * ($shm / $ta / $h / $nf).  No Node, no Python — libFuzzer target.
 * Paths for $shm must stay under the sandbox dir (basename only). */
#ifndef CC_BRIDGE_WIRE_CODEC_H
#define CC_BRIDGE_WIRE_CODEC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  CC_WIRE_OK = 0,
  CC_WIRE_ERR = -1,
  CC_WIRE_REJECT = 1 /* articulate refusal, not a crash */
};

typedef struct CcWireVal CcWireVal;

/* Chunked bump arena: prior chunks never move (no realloc-under-pointers). */
typedef struct CcWireChunk {
  struct CcWireChunk *next;
  size_t len;
  size_t cap;
  char buf[]; /* flexible — allocated with the chunk */
} CcWireChunk;

typedef struct CcWireArena {
  CcWireChunk *head; /* newest first */
  size_t chunk_size;
} CcWireArena;

typedef struct CcWireDom {
  char shm_dir[512];
  CcWireVal **handles;
  size_t handle_cap;
  size_t handle_n;
  CcWireArena arena;
  int err; /* sticky articulate error */
} CcWireDom;

int cc_wire_dom_init(CcWireDom *d, const char *shm_dir);
void cc_wire_dom_reset(CcWireDom *d);
void cc_wire_dom_destroy(CcWireDom *d);

/* Decode a structure-aware binary blob (see wire_codec.c).  Returns
 * CC_WIRE_OK / CC_WIRE_REJECT / CC_WIRE_ERR.  Never crashes on bad input. */
int cc_wire_decode_blob(CcWireDom *d, const uint8_t *data, size_t size,
                        CcWireVal **out);

/* Round-trip: encode a simple $ta / $shm / $h value back to bytes for
 * self-tests.  out must be freed with free(). */
int cc_wire_encode_ta(const uint8_t *raw, size_t n, uint8_t kind,
                      int spill, const char *shm_dir,
                      uint8_t **out, size_t *out_n);

#ifdef __cplusplus
}
#endif

#endif
