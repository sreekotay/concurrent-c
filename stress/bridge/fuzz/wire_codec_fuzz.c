/* libFuzzer entry for stress/bridge/fuzz/wire_codec.c
 *
 *   ./scripts/fuzz_wire_codec.sh           # 30s default
 *   FUZZ_SECONDS=120 ./scripts/fuzz_wire_codec.sh
 */
#include "wire_codec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static CcWireDom g_dom;
static int g_ready;
static char g_dir[512];

static void ensure_dom(void) {
  if (g_ready) return;
  snprintf(g_dir, sizeof(g_dir), "/tmp/cc-wire-fuzz-%d", (int)getpid());
  if (mkdir(g_dir, 0700) != 0) {
    /* may already exist across reloads */
  }
  if (cc_wire_dom_init(&g_dom, g_dir) != CC_WIRE_OK) abort();
  g_ready = 1;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  CcWireVal *v = NULL;
  int rc;
  ensure_dom();
  cc_wire_dom_reset(&g_dom);
  /* Cap: keep corpus small; wire_codec rejects oversized too. */
  if (size > (1u << 20)) size = 1u << 20;
  rc = cc_wire_decode_blob(&g_dom, data, size, &v);
  (void)rc;
  (void)v;
  return 0;
}

#ifdef FUZZ_STANDALONE_MAIN
/* Optional non-libFuzzer smoke: feed a few hand-built blobs. */
int main(void) {
  uint8_t *blob = NULL;
  size_t n = 0;
  uint8_t raw[32];
  size_t i;
  for (i = 0; i < sizeof(raw); i++) raw[i] = (uint8_t)i;
  ensure_dom();
  if (cc_wire_encode_ta(raw, sizeof(raw), 4 /*u8*/, 0, g_dir, &blob, &n)
      != CC_WIRE_OK)
    return 1;
  if (LLVMFuzzerTestOneInput(blob, n) != 0) return 2;
  free(blob);
  if (cc_wire_encode_ta(raw, sizeof(raw), 4, 1, g_dir, &blob, &n)
      != CC_WIRE_OK)
    return 3;
  if (LLVMFuzzerTestOneInput(blob, n) != 0) return 4;
  free(blob);
  /* reserved-key dict should reject, not crash */
  {
    uint8_t bad[] = {
      8,       /* dict */
      1, 0,    /* n=1 */
      2, 0, '$', 'x', /* key "$x" */
      0        /* null value */
    };
    LLVMFuzzerTestOneInput(bad, sizeof(bad));
  }
  cc_wire_dom_destroy(&g_dom);
  fprintf(stderr, "wire_codec standalone smoke ok\n");
  return 0;
}
#endif
