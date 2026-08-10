/* Wire-value sandbox: mirrors broker.py / index.js tags without JSON.
 *
 * Binary IR (little-endian):
 *   u8 tag:
 *     0 null
 *     1 f64
 *     2 string: u16 len + bytes
 *     3 $nf: u8 (0=nan,1=inf,2=-inf)
 *     4 $h:  u32 id
 *     5 $ta: u8 kind + u32 len + bytes   (inline)
 *     6 $shm:u8 kind + u32 len + bytes   (write temp under sandbox, read, unlink)
 *     7 list: u16 n + n values
 *     8 dict: u16 n + n×(string key, value)  — keys must not start with '$'
 *   kind: 0=f64 1=f32 2=i32 3=i64 4=u8
 */
#include "wire_codec.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
  TAG_NULL = 0,
  TAG_F64 = 1,
  TAG_STR = 2,
  TAG_NF = 3,
  TAG_H = 4,
  TAG_TA = 5,
  TAG_SHM = 6,
  TAG_LIST = 7,
  TAG_DICT = 8,
};

enum { KIND_F64 = 0, KIND_F32 = 1, KIND_I32 = 2, KIND_I64 = 3, KIND_U8 = 4 };

struct CcWireVal {
  int tag;
  union {
    double f;
    struct { char *s; size_t n; } str;
    int nf; /* 0 nan 1 inf 2 -inf */
    uint32_t h;
    struct { uint8_t kind; uint8_t *p; size_t n; } buf;
    struct { CcWireVal **items; size_t n; } list;
    struct { char **keys; CcWireVal **vals; size_t n; } dict;
  } u;
};

static size_t kind_width(uint8_t k) {
  switch (k) {
  case KIND_F64: return 8;
  case KIND_F32: return 4;
  case KIND_I32: return 4;
  case KIND_I64: return 8;
  case KIND_U8: return 1;
  default: return 0;
  }
}

enum { CC_WIRE_CHUNK_DEFAULT = 4096 };

static void arena_free_chunks(CcWireArena *a) {
  CcWireChunk *c = a->head;
  while (c) {
    CcWireChunk *n = c->next;
    free(c);
    c = n;
  }
  a->head = NULL;
}

static void *arena_alloc(CcWireArena *a, size_t n) {
  CcWireChunk *c;
  void *p;
  size_t cap;
  if (n == 0) n = 1;
  /* 16-byte align */
  n = (n + 15u) & ~(size_t)15u;
  if (!a->chunk_size) a->chunk_size = CC_WIRE_CHUNK_DEFAULT;
  c = a->head;
  if (c && c->len + n <= c->cap) {
    p = c->buf + c->len;
    c->len += n;
    return p;
  }
  /* New chunk — never realloc an old one (live pointers stay valid). */
  cap = a->chunk_size;
  if (n > cap) cap = n;
  c = (CcWireChunk *)malloc(sizeof(CcWireChunk) + cap);
  if (!c) return NULL;
  c->next = a->head;
  c->len = 0;
  c->cap = cap;
  a->head = c;
  p = c->buf;
  c->len = n;
  return p;
}

static CcWireVal *val_new(CcWireDom *d) {
  return (CcWireVal *)arena_alloc(&d->arena, sizeof(CcWireVal));
}

int cc_wire_dom_init(CcWireDom *d, const char *shm_dir) {
  memset(d, 0, sizeof(*d));
  if (!shm_dir || !shm_dir[0]) return CC_WIRE_ERR;
  if (strlen(shm_dir) >= sizeof(d->shm_dir)) return CC_WIRE_ERR;
  memcpy(d->shm_dir, shm_dir, strlen(shm_dir) + 1);
  d->arena.chunk_size = CC_WIRE_CHUNK_DEFAULT;
  return CC_WIRE_OK;
}

void cc_wire_dom_reset(CcWireDom *d) {
  d->handle_n = 0;
  arena_free_chunks(&d->arena);
  d->err = 0;
}

void cc_wire_dom_destroy(CcWireDom *d) {
  free(d->handles);
  arena_free_chunks(&d->arena);
  memset(d, 0, sizeof(*d));
}

static int put_handle(CcWireDom *d, CcWireVal *v, uint32_t *id_out) {
  if (d->handle_n + 1 > d->handle_cap) {
    size_t nc = d->handle_cap ? d->handle_cap * 2 : 16;
    CcWireVal **nh = (CcWireVal **)realloc(d->handles, nc * sizeof(*nh));
    if (!nh) return CC_WIRE_ERR;
    d->handles = nh;
    d->handle_cap = nc;
  }
  *id_out = (uint32_t)d->handle_n;
  d->handles[d->handle_n++] = v;
  return CC_WIRE_OK;
}

/* $shm path must be exactly shm_dir/basename with no separators in basename. */
static int sandbox_path(const CcWireDom *d, const char *base, char *out,
                        size_t out_n) {
  size_t i, bl, dl;
  if (!base || !base[0]) return CC_WIRE_REJECT;
  for (i = 0; base[i]; i++) {
    if (base[i] == '/' || base[i] == '\\' || base[i] == '\0')
      return CC_WIRE_REJECT;
    if (base[i] == '.' && base[i + 1] == '.') return CC_WIRE_REJECT;
  }
  bl = strlen(base);
  dl = strlen(d->shm_dir);
  if (dl + 1 + bl + 1 > out_n) return CC_WIRE_REJECT;
  memcpy(out, d->shm_dir, dl);
  out[dl] = '/';
  memcpy(out + dl + 1, base, bl + 1);
  return CC_WIRE_OK;
}

typedef struct {
  const uint8_t *p;
  size_t n;
  size_t i;
} Cursor;

static int need(Cursor *c, size_t k) {
  return c->i + k <= c->n;
}

static int read_u8(Cursor *c, uint8_t *o) {
  if (!need(c, 1)) return CC_WIRE_REJECT;
  *o = c->p[c->i++];
  return CC_WIRE_OK;
}

static int read_u16(Cursor *c, uint16_t *o) {
  if (!need(c, 2)) return CC_WIRE_REJECT;
  *o = (uint16_t)(c->p[c->i] | (c->p[c->i + 1] << 8));
  c->i += 2;
  return CC_WIRE_OK;
}

static int read_u32(Cursor *c, uint32_t *o) {
  if (!need(c, 4)) return CC_WIRE_REJECT;
  *o = (uint32_t)c->p[c->i] | ((uint32_t)c->p[c->i + 1] << 8) |
       ((uint32_t)c->p[c->i + 2] << 16) | ((uint32_t)c->p[c->i + 3] << 24);
  c->i += 4;
  return CC_WIRE_OK;
}

static int read_bytes(Cursor *c, size_t n, const uint8_t **o) {
  if (!need(c, n)) return CC_WIRE_REJECT;
  *o = c->p + c->i;
  c->i += n;
  return CC_WIRE_OK;
}

static int decode_one(CcWireDom *d, Cursor *c, CcWireVal **out);

static int decode_buf(CcWireDom *d, Cursor *c, int shm, CcWireVal **out) {
  uint8_t kind;
  uint32_t len;
  const uint8_t *raw;
  CcWireVal *v;
  size_t w;
  int rc;
  if ((rc = read_u8(c, &kind)) != CC_WIRE_OK) return rc;
  w = kind_width(kind);
  if (!w) return CC_WIRE_REJECT;
  if ((rc = read_u32(c, &len)) != CC_WIRE_OK) return rc;
  if (len > (1u << 22)) return CC_WIRE_REJECT; /* 4 MiB cap */
  if (len % (uint32_t)w != 0) return CC_WIRE_REJECT;
  if ((rc = read_bytes(c, len, &raw)) != CC_WIRE_OK) return rc;

  v = val_new(d);
  if (!v) return CC_WIRE_ERR;
  memset(v, 0, sizeof(*v));
  v->tag = shm ? TAG_SHM : TAG_TA;
  v->u.buf.kind = kind;
  v->u.buf.n = len;
  v->u.buf.p = (uint8_t *)arena_alloc(&d->arena, len ? len : 1);
  if (!v->u.buf.p) return CC_WIRE_ERR;
  if (len) memcpy(v->u.buf.p, raw, len);

  if (shm) {
    char path[768];
    char base[64];
    FILE *f;
    uint8_t *got;
    size_t nr;
    snprintf(base, sizeof(base), "fuzz-%d-%u", (int)getpid(),
             (unsigned)(d->handle_n + 1));
    if (sandbox_path(d, base, path, sizeof(path)) != CC_WIRE_OK)
      return CC_WIRE_REJECT;
    f = fopen(path, "wb");
    if (!f) return CC_WIRE_REJECT;
    if (len && fwrite(raw, 1, len, f) != len) {
      fclose(f);
      unlink(path);
      return CC_WIRE_ERR;
    }
    fclose(f);
    /* Consume like the broker: read then unlink. */
    f = fopen(path, "rb");
    if (!f) return CC_WIRE_ERR;
    got = (uint8_t *)arena_alloc(&d->arena, len ? len : 1);
    if (!got) {
      fclose(f);
      unlink(path);
      return CC_WIRE_ERR;
    }
    nr = fread(got, 1, len, f);
    fclose(f);
    unlink(path);
    if (nr != len) return CC_WIRE_ERR;
    if (len && memcmp(got, raw, len) != 0) return CC_WIRE_ERR;
    v->u.buf.p = got;
  }
  *out = v;
  return CC_WIRE_OK;
}

static int decode_one(CcWireDom *d, Cursor *c, CcWireVal **out) {
  uint8_t tag;
  int rc;
  CcWireVal *v;
  if ((rc = read_u8(c, &tag)) != CC_WIRE_OK) return rc;
  switch (tag) {
  case TAG_NULL:
    v = val_new(d);
    if (!v) return CC_WIRE_ERR;
    memset(v, 0, sizeof(*v));
    v->tag = TAG_NULL;
    *out = v;
    return CC_WIRE_OK;
  case TAG_F64: {
    const uint8_t *b;
    if ((rc = read_bytes(c, 8, &b)) != CC_WIRE_OK) return rc;
    v = val_new(d);
    if (!v) return CC_WIRE_ERR;
    memset(v, 0, sizeof(*v));
    v->tag = TAG_F64;
    memcpy(&v->u.f, b, 8);
    *out = v;
    return CC_WIRE_OK;
  }
  case TAG_STR: {
    uint16_t n;
    const uint8_t *b;
    char *s;
    if ((rc = read_u16(c, &n)) != CC_WIRE_OK) return rc;
    if ((rc = read_bytes(c, n, &b)) != CC_WIRE_OK) return rc;
    v = val_new(d);
    if (!v) return CC_WIRE_ERR;
    memset(v, 0, sizeof(*v));
    v->tag = TAG_STR;
    s = (char *)arena_alloc(&d->arena, (size_t)n + 1);
    if (!s) return CC_WIRE_ERR;
    if (n) memcpy(s, b, n);
    s[n] = 0;
    v->u.str.s = s;
    v->u.str.n = n;
    *out = v;
    return CC_WIRE_OK;
  }
  case TAG_NF: {
    uint8_t k;
    if ((rc = read_u8(c, &k)) != CC_WIRE_OK) return rc;
    if (k > 2) return CC_WIRE_REJECT;
    v = val_new(d);
    if (!v) return CC_WIRE_ERR;
    memset(v, 0, sizeof(*v));
    v->tag = TAG_NF;
    v->u.nf = k;
    *out = v;
    return CC_WIRE_OK;
  }
  case TAG_H: {
    uint32_t id;
    if ((rc = read_u32(c, &id)) != CC_WIRE_OK) return rc;
    if (id >= d->handle_n || !d->handles[id]) return CC_WIRE_REJECT;
    v = val_new(d);
    if (!v) return CC_WIRE_ERR;
    memset(v, 0, sizeof(*v));
    v->tag = TAG_H;
    v->u.h = id;
    *out = v;
    return CC_WIRE_OK;
  }
  case TAG_TA:
    return decode_buf(d, c, 0, out);
  case TAG_SHM:
    return decode_buf(d, c, 1, out);
  case TAG_LIST: {
    uint16_t n;
    size_t i;
    if ((rc = read_u16(c, &n)) != CC_WIRE_OK) return rc;
    if (n > 1024) return CC_WIRE_REJECT;
    v = val_new(d);
    if (!v) return CC_WIRE_ERR;
    memset(v, 0, sizeof(*v));
    v->tag = TAG_LIST;
    v->u.list.n = n;
    v->u.list.items =
        (CcWireVal **)arena_alloc(&d->arena, n ? n * sizeof(CcWireVal *) : 1);
    if (!v->u.list.items) return CC_WIRE_ERR;
    for (i = 0; i < n; i++) {
      if ((rc = decode_one(d, c, &v->u.list.items[i])) != CC_WIRE_OK)
        return rc;
    }
    *out = v;
    return CC_WIRE_OK;
  }
  case TAG_DICT: {
    uint16_t n;
    size_t i;
    if ((rc = read_u16(c, &n)) != CC_WIRE_OK) return rc;
    if (n > 256) return CC_WIRE_REJECT;
    v = val_new(d);
    if (!v) return CC_WIRE_ERR;
    memset(v, 0, sizeof(*v));
    v->tag = TAG_DICT;
    v->u.dict.n = n;
    v->u.dict.keys =
        (char **)arena_alloc(&d->arena, n ? n * sizeof(char *) : 1);
    v->u.dict.vals =
        (CcWireVal **)arena_alloc(&d->arena, n ? n * sizeof(CcWireVal *) : 1);
    if (!v->u.dict.keys || !v->u.dict.vals) return CC_WIRE_ERR;
    for (i = 0; i < n; i++) {
      uint16_t kn;
      const uint8_t *kb;
      char *ks;
      if ((rc = read_u16(c, &kn)) != CC_WIRE_OK) return rc;
      if ((rc = read_bytes(c, kn, &kb)) != CC_WIRE_OK) return rc;
      if (kn && kb[0] == '$') return CC_WIRE_REJECT; /* reserved */
      ks = (char *)arena_alloc(&d->arena, (size_t)kn + 1);
      if (!ks) return CC_WIRE_ERR;
      if (kn) memcpy(ks, kb, kn);
      ks[kn] = 0;
      v->u.dict.keys[i] = ks;
      if ((rc = decode_one(d, c, &v->u.dict.vals[i])) != CC_WIRE_OK)
        return rc;
    }
    *out = v;
    return CC_WIRE_OK;
  }
  default:
    return CC_WIRE_REJECT;
  }
}

int cc_wire_decode_blob(CcWireDom *d, const uint8_t *data, size_t size,
                        CcWireVal **out) {
  Cursor c;
  int rc;
  CcWireVal *v = NULL;
  if (!d || !data) return CC_WIRE_ERR;
  c.p = data;
  c.n = size;
  c.i = 0;
  rc = decode_one(d, &c, &v);
  if (rc == CC_WIRE_OK && v &&
      (v->tag == TAG_TA || v->tag == TAG_SHM || v->tag == TAG_LIST)) {
    uint32_t id;
    if (put_handle(d, v, &id) != CC_WIRE_OK) return CC_WIRE_ERR;
  }
  if (out) *out = (rc == CC_WIRE_OK) ? v : NULL;
  return rc;
}

int cc_wire_encode_ta(const uint8_t *raw, size_t n, uint8_t kind, int spill,
                      const char *shm_dir, uint8_t **out, size_t *out_n) {
  size_t need;
  uint8_t *b;
  size_t i = 0;
  (void)shm_dir;
  if (!raw && n) return CC_WIRE_ERR;
  if (kind > KIND_U8) return CC_WIRE_REJECT;
  need = 1 + 1 + 4 + n;
  b = (uint8_t *)malloc(need);
  if (!b) return CC_WIRE_ERR;
  b[i++] = spill ? TAG_SHM : TAG_TA;
  b[i++] = kind;
  b[i++] = (uint8_t)(n & 0xff);
  b[i++] = (uint8_t)((n >> 8) & 0xff);
  b[i++] = (uint8_t)((n >> 16) & 0xff);
  b[i++] = (uint8_t)((n >> 24) & 0xff);
  if (n) memcpy(b + i, raw, n);
  *out = b;
  *out_n = need;
  return CC_WIRE_OK;
}
