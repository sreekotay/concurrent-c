/* CC lowered C output */
#include <stdlib.h>
#include <stdint.h>
#include <ccc/cc_nursery.h>
#include <ccc/cc_closure.h>
#include <ccc/cc_slice.h>
#include <ccc/cc_runtime.h>
#include <ccc/std/io.h>
#include <ccc/std/task.h>
typedef intptr_t CCAbIntptr;
#include <ccc/cc_closure_helper.h>

#line 1 "examples/serdes/c/shadow_lower.ccs"
/* SERDES shadow lowerer — successor front (opt-in via ccc --frontend=serdes).
 *
 * Usage:
 *   shadow_lower <in.ccs|.cch> [-o out] [--exe binary] [--no-cache]
 *
 *   -o out.c/.h     write lowered text
 *   -o binary       emit → cache → host cc → link runtime (ccc-compatible)
 *   --exe binary    emit buffer → libtcc (no .c on disk; leaf libcc_runtime)
 *   (no -o)         write text to stdout
 */
#include <ccc/std/prelude.h>
#include <unistd.h>

#line 14 "examples/serdes/c/shadow_lower.ccs"
/* @grammar(rules) PpTok (line 14): 21 rule(s). API:
 *   cc_match(PpTok, s, n)                     -> PpTok_match
 *   cc_parse(PpTok, s, n, arena)              -> PpTok_parse -> PpTokNode* tape
 *   cc_collect(PpTok, s, n, arena, cb, env)   -> PpTok_collect
 *   nodes: PpTokNode_{id,is_list,len,first,next,count,slice}; ids: PpTok_KEEP_<rule>
 */
typedef struct { int rule_count; } PpTok;
static inline int PpTok_rule_count(void) { return 21; }
typedef struct { unsigned long long meta;
    union { const char* bytes; } u; } PpTokNode;
struct CCShapeB; struct CCShapeVal;
typedef struct { PpTokNode* tape; size_t total, cap;
    size_t bstack[512]; size_t bdepth; CCArena* arena;
    const char** mat_ptr; size_t* mat_len; unsigned char* mat_cow;
    /* shaped-DOM hook (armed by _dom; NULL for plain parse):
     * completed containers hand their value up this stack —
     * one push per container, anchored for derived restore */
    struct CCShapeB* sb;
    struct CCShapeVal* vstack; const unsigned char** vanchor;
    size_t vsp, vcap;
    size_t vbase[512]; } PpTok__ctx;
static __attribute__((noinline)) int PpTok__tgrow(PpTok__ctx* c) {
    size_t nc = c->cap * 2;
    PpTokNode* nt = (PpTokNode*)cc_arena_realloc(c->arena, c->arena, c->tape,
        c->cap * sizeof(PpTokNode), nc * sizeof(PpTokNode), _Alignof(PpTokNode));
    if (!nt) return 0;
    c->tape = nt;
    c->cap = nc; return 1;
}
static __attribute__((noinline)) void PpTok__unwind(PpTok__ctx* c, const unsigned char* sv) {
    while (c->total > 1 && (const unsigned char*)c->tape[c->total - 1].u.bytes >= sv)
        c->total--;
    while (c->bdepth > 0 && c->bstack[c->bdepth - 1] >= c->total) c->bdepth--;
    if (c->vanchor)
        while (c->vsp > 0 && c->vanchor[c->vsp - 1] >= sv) c->vsp--;
}
#define PpTok_KEEP_pp_number 9
#define PpTok_KEEP_ident 10
#define PpTok_KEEP_string 14
#define PpTok_KEEP_charlit 15
#define PpTok_KEEP_bt_string 16
#define PpTok_KEEP_punct 19
static const unsigned char PpTok__pool[309] = {99,111,109,109,101,110,116,0,119,115,0,116,111,107,0,108,105,110,101,95,99,111,109,109,101,110,116,0,98,108,111,99,107,95,99,111,109,109,101,110,116,0,47,47,0,47,42,0,98,108,111,99,107,95,99,104,97,114,0,42,47,0,42,0,100,105,103,105,116,0,46,0,100,105,103,105,116,0,100,105,103,105,116,0,110,111,110,100,105,103,105,116,0,46,0,110,111,110,100,105,103,105,116,0,105,100,99,111,110,116,0,92,0,34,0,115,116,114,99,104,97,114,0,101,115,99,0,34,0,39,0,99,104,114,99,104,97,114,0,101,115,99,0,39,0,96,0,96,0,46,46,46,0,60,60,61,0,62,62,61,0,45,62,0,61,62,0,33,62,0,63,62,0,61,60,33,0,60,63,0,58,58,0,43,43,0,45,45,0,60,60,0,62,62,0,60,61,0,62,61,0,61,61,0,33,61,0,38,38,0,124,124,0,42,61,0,47,61,0,37,61,0,43,61,0,45,61,0,38,61,0,94,61,0,124,61,0,35,35,0,112,117,110,99,116,95,109,117,108,116,105,0,112,117,110,99,116,95,111,110,101,0,105,100,101,110,116,0,112,112,95,110,117,109,98,101,114,0,115,116,114,105,110,103,0,98,116,95,115,116,114,105,110,103,0,99,104,97,114,108,105,116,0,112,117,110,99,116,0,};
#if defined(__SSE2__)
#include <emmintrin.h>
#endif
static const unsigned char PpTok__cs[13][32] = {
  /* cs0: tab-nl cr ' ' */
  {0,38,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,},
  /* cs1: 0x00-tab 0x0B-0xFF */
  {255,251,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,},
  /* cs2: 0x00-')' '+'-0xFF */
  {255,255,255,255,255,251,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,},
  /* cs3: 0x00-'.' '0'-0xFF */
  {255,255,255,255,255,127,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,},
  /* cs4: '0'-'9' */
  {0,0,0,0,0,0,255,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,},
  /* cs5: 'A'-'Z' '_' 'a'-'z' */
  {0,0,0,0,0,0,0,0,254,255,255,135,254,255,255,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,},
  /* cs6: '0'-'9' 'A'-'Z' '_' 'a'-'z' */
  {0,0,0,0,0,0,255,3,254,255,255,135,254,255,255,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,},
  /* cs7: 'E' 'P' 'e' 'p' */
  {0,0,0,0,0,0,0,0,32,0,1,0,32,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,},
  /* cs8: '+' '-' */
  {0,0,0,0,0,40,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,},
  /* cs9: 0x00-tab 0x0B-'!' '#'-'[' ']'-0xFF */
  {255,251,255,255,251,255,255,255,255,255,255,239,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,},
  /* cs10: 0x00-tab 0x0B-'&' '('-'[' ']'-0xFF */
  {255,251,255,255,127,255,255,255,255,255,255,239,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,},
  /* cs11: 0x00-'_' 'a'-0xFF */
  {255,255,255,255,255,255,255,255,255,255,255,255,254,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,},
  /* cs12: '!' '#' '%'-'&' '('-0x2F ':'-'@' '[' ']'-'^' '{'-'~' */
  {0,0,0,0,106,255,0,252,1,0,0,104,0,0,0,120,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,},
};
static int PpTok__m_top(const unsigned char* s, size_t n, size_t* io);
static int PpTok__b_top(PpTok__ctx* c, const unsigned char* s, size_t n, size_t* io);
static int PpTok__r_punct_multi(const unsigned char* s, size_t n, size_t* io);
static int PpTok__m_punct(const unsigned char* s, size_t n, size_t* io);
static int PpTok__b_punct(PpTok__ctx* c, const unsigned char* s, size_t n, size_t* io);
static int PpTok__m_tok(const unsigned char* s, size_t n, size_t* io);
static int PpTok__b_tok(PpTok__ctx* c, const unsigned char* s, size_t n, size_t* io);
static int PpTok__m_top(const unsigned char* s, size_t n, size_t* io) {
    size_t p = *io;
    (void)s; (void)n;
    { size_t sv1;
    for (;;) { sv1 = p;
    { size_t sv2 = p;
    { size_t sv3 = p;
    if (!(p + 2 <= n && memcmp(s + p, PpTok__pool + 42, 2) == 0)) goto La3_0;   /* "\057\057" */
    p += 2;
#if defined(__SSE2__)
    { while (p + 16 <= n) {
        __m128i w4 = _mm_loadu_si128((const __m128i*)(s + p));
        __m128i m4 = _mm_setzero_si128();
        m4 = _mm_or_si128(m4, _mm_cmpeq_epi8(w4, _mm_set1_epi8((char)10)));
        { int hm4 = _mm_movemask_epi8(m4);
          if (hm4) { p += (size_t)__builtin_ctz((unsigned)hm4); break; } }
        p += 16;
    } }
#else
    { const unsigned long long L4 = 0x0101010101010101ULL, H4 = 0x8080808080808080ULL;
    while (p + 8 <= n) { unsigned long long w4; memcpy(&w4, s + p, 8);
        unsigned long long m4 = 0;
        { unsigned long long x = w4 ^ (L4 * 10); m4 |= (x - L4) & ~x & H4; }
        if (m4) { p += (size_t)(__builtin_ctzll(m4) >> 3); break; }
        p += 8;
    } }
#endif
    while (p < n && (PpTok__cs[1][((unsigned char)s[p]) >> 3] & (1u << (((unsigned char)s[p]) & 7u)))) p++;
    goto Lok3;
La3_0: p = sv3;
    if (!(p + 2 <= n && memcmp(s + p, PpTok__pool + 45, 2) == 0)) goto La3_1;   /* "\057\052" */
    p += 2;
    { size_t sv5;
    for (;;) { sv5 = p;
    {
    if (p >= n) goto Lb6;
    switch (s[p]) {
    case 42:
    if (!(p < n && s[p] == 42 /*0x2A*/)) goto Lb6;
    p++;
    if (!(p < n && (PpTok__cs[3][((unsigned char)s[p]) >> 3] & (1u << (((unsigned char)s[p]) & 7u))))) goto Lb6;
    p++;
    break;
    default:
    if (!(p < n && (PpTok__cs[2][((unsigned char)s[p]) >> 3] & (1u << (((unsigned char)s[p]) & 7u))))) goto Lb6;
    p++;
    break;
    }
    goto Lok6;
Lb6: goto Ly5;
Lok6: ; }
    if (p == sv5) break;
    }
    goto Lok5;
Ly5: p = sv5;
Lok5: ; }
    if (!(p + 2 <= n && memcmp(s + p, PpTok__pool + 59, 2) == 0)) goto La3_1;   /* "\052\057" */
    p += 2;
    goto Lok3;
La3_1: p = sv3;
    goto La2_0;
Lok3: ; }
    goto Lok2;
La2_0: p = sv2;
    if (!(p < n && (s[p] == 9 || s[p] == 10 || s[p] == 13 || s[p] == 32))) goto La2_1;
    p++;
    while (p < n && (s[p] == 9 || s[p] == 10 || s[p] == 13 || s[p] == 32)) p++;
    goto Lok2;
La2_1: p = sv2;
    if (!PpTok__m_tok(s, n, &p)) goto La2_2;
    goto Lok2;
La2_2: p = sv2;
    goto Ly1;
Lok2: ; }
    if (p == sv1) break;
    }
    goto Lok1;
Ly1: p = sv1;
Lok1: ; }
    *io = p; return 1;
Lf0:
    return 0;
}
static int PpTok__m_punct(const unsigned char* s, size_t n, size_t* io) {
    size_t p = *io;
    (void)s; (void)n;
    { size_t sv8 = p;
    if (!PpTok__r_punct_multi(s, n, &p)) goto La8_0;
    goto Lok8;
La8_0: p = sv8;
    if (!(p < n && (PpTok__cs[12][((unsigned char)s[p]) >> 3] & (1u << (((unsigned char)s[p]) & 7u))))) goto La8_1;
    p++;
    goto Lok8;
La8_1: p = sv8;
    goto Lf7;
Lok8: ; }
    *io = p; return 1;
Lf7:
    return 0;
}
static int PpTok__m_tok(const unsigned char* s, size_t n, size_t* io) {
    size_t p = *io;
    (void)s; (void)n;
    { size_t sv10 = p;
    if (!(p < n && (PpTok__cs[5][((unsigned char)s[p]) >> 3] & (1u << (((unsigned char)s[p]) & 7u))))) goto La10_0;
    p++;
    while (p < n && (PpTok__cs[6][((unsigned char)s[p]) >> 3] & (1u << (((unsigned char)s[p]) & 7u)))) p++;
    goto Lok10;
La10_0: p = sv10;
    {
    if (p >= n) goto Lb11;
    switch (s[p]) {
    case 48:
    case 49:
    case 50:
    case 51:
    case 52:
    case 53:
    case 54:
    case 55:
    case 56:
    case 57:
    if (!(p < n && ((unsigned)(s[p] - 48) <= 9u))) goto Lb11;
    p++;
    break;
    case 46:
    if (!(p < n && s[p] == 46 /*'.'*/)) goto Lb11;
    p++;
    if (!(p < n && ((unsigned)(s[p] - 48) <= 9u))) goto Lb11;
    p++;
    break;
    default: goto Lb11;
    }
    goto Lok11;
Lb11: goto La10_1;
Lok11: ; }
    { size_t sv12;
    for (;;) { sv12 = p;
    { size_t sv13 = p;
    if (!(p < n && (s[p] == 69 || s[p] == 80 || s[p] == 101 || s[p] == 112))) goto La13_0;
    p++;
    if (!(p < n && (s[p] == 43 || s[p] == 45))) goto La13_0;
    p++;
    goto Lok13;
La13_0: p = sv13;
    if (!(p < n && ((unsigned)(s[p] - 48) <= 9u))) goto La13_1;
    p++;
    goto Lok13;
La13_1: p = sv13;
    if (!(p < n && (PpTok__cs[5][((unsigned char)s[p]) >> 3] & (1u << (((unsigned char)s[p]) & 7u))))) goto La13_2;
    p++;
    goto Lok13;
La13_2: p = sv13;
    if (!(p < n && s[p] == 46 /*'.'*/)) goto La13_3;
    p++;
    goto Lok13;
La13_3: p = sv13;
    goto Ly12;
Lok13: ; }
    if (p == sv12) break;
    }
    goto Lok12;
Ly12: p = sv12;
Lok12: ; }
    goto Lok10;
La10_1: p = sv10;
    if (!(p < n && s[p] == 34 /*'"'*/)) goto La10_2;
    p++;
    for (;;) {
#if defined(__SSE2__)
    { while (p + 16 <= n) {
        __m128i w14 = _mm_loadu_si128((const __m128i*)(s + p));
        __m128i m14 = _mm_setzero_si128();
        m14 = _mm_cmpeq_epi8(_mm_min_epu8(w14, _mm_set1_epi8((char)10)), w14);
        m14 = _mm_or_si128(m14, _mm_cmpeq_epi8(w14, _mm_set1_epi8((char)34)));
        m14 = _mm_or_si128(m14, _mm_cmpeq_epi8(w14, _mm_set1_epi8((char)92)));
        { int hm14 = _mm_movemask_epi8(m14);
          if (hm14) { p += (size_t)__builtin_ctz((unsigned)hm14); break; } }
        p += 16;
    } }
#else
    { const unsigned long long L14 = 0x0101010101010101ULL, H14 = 0x8080808080808080ULL;
    while (p + 8 <= n) { unsigned long long w14; memcpy(&w14, s + p, 8);
        unsigned long long m14 = 0;
        m14 |= (w14 - L14 * 11) & ~w14 & H14;
        { unsigned long long x = w14 ^ (L14 * 34); m14 |= (x - L14) & ~x & H14; }
        { unsigned long long x = w14 ^ (L14 * 92); m14 |= (x - L14) & ~x & H14; }
        if (m14) { p += (size_t)(__builtin_ctzll(m14) >> 3); break; }
        p += 8;
    } }
#endif
    while (p < n && (PpTok__cs[9][((unsigned char)s[p]) >> 3] & (1u << (((unsigned char)s[p]) & 7u)))) p++;
    { size_t sv14 = p;
    if (!(p < n && s[p] == 92 /*0x5C*/)) goto Le14;
    p++;
    if (!(p < n)) goto Le14;
    p++;
    continue;
Le14: p = sv14; }
    break;
    }
    if (!(p < n && s[p] == 34 /*'"'*/)) goto La10_2;
    p++;
    goto Lok10;
La10_2: p = sv10;
    if (!(p < n && s[p] == 96 /*'`'*/)) goto La10_3;
    p++;
#if defined(__SSE2__)
    { while (p + 16 <= n) {
        __m128i w15 = _mm_loadu_si128((const __m128i*)(s + p));
        __m128i m15 = _mm_setzero_si128();
        m15 = _mm_or_si128(m15, _mm_cmpeq_epi8(w15, _mm_set1_epi8((char)96)));
        { int hm15 = _mm_movemask_epi8(m15);
          if (hm15) { p += (size_t)__builtin_ctz((unsigned)hm15); break; } }
        p += 16;
    } }
#else
    { const unsigned long long L15 = 0x0101010101010101ULL, H15 = 0x8080808080808080ULL;
    while (p + 8 <= n) { unsigned long long w15; memcpy(&w15, s + p, 8);
        unsigned long long m15 = 0;
        { unsigned long long x = w15 ^ (L15 * 96); m15 |= (x - L15) & ~x & H15; }
        if (m15) { p += (size_t)(__builtin_ctzll(m15) >> 3); break; }
        p += 8;
    } }
#endif
    while (p < n && (PpTok__cs[11][((unsigned char)s[p]) >> 3] & (1u << (((unsigned char)s[p]) & 7u)))) p++;
    if (!(p < n && s[p] == 96 /*'`'*/)) goto La10_3;
    p++;
    goto Lok10;
La10_3: p = sv10;
    if (!(p < n && s[p] == 39 /*0x27*/)) goto La10_4;
    p++;
    for (;;) {
#if defined(__SSE2__)
    { while (p + 16 <= n) {
        __m128i w16 = _mm_loadu_si128((const __m128i*)(s + p));
        __m128i m16 = _mm_setzero_si128();
        m16 = _mm_cmpeq_epi8(_mm_min_epu8(w16, _mm_set1_epi8((char)10)), w16);
        m16 = _mm_or_si128(m16, _mm_cmpeq_epi8(w16, _mm_set1_epi8((char)39)));
        m16 = _mm_or_si128(m16, _mm_cmpeq_epi8(w16, _mm_set1_epi8((char)92)));
        { int hm16 = _mm_movemask_epi8(m16);
          if (hm16) { p += (size_t)__builtin_ctz((unsigned)hm16); break; } }
        p += 16;
    } }
#else
    { const unsigned long long L16 = 0x0101010101010101ULL, H16 = 0x8080808080808080ULL;
    while (p + 8 <= n) { unsigned long long w16; memcpy(&w16, s + p, 8);
        unsigned long long m16 = 0;
        m16 |= (w16 - L16 * 11) & ~w16 & H16;
        { unsigned long long x = w16 ^ (L16 * 39); m16 |= (x - L16) & ~x & H16; }
        { unsigned long long x = w16 ^ (L16 * 92); m16 |= (x - L16) & ~x & H16; }
        if (m16) { p += (size_t)(__builtin_ctzll(m16) >> 3); break; }
        p += 8;
    } }
#endif
    while (p < n && (PpTok__cs[10][((unsigned char)s[p]) >> 3] & (1u << (((unsigned char)s[p]) & 7u)))) p++;
    { size_t sv16 = p;
    if (!(p < n && s[p] == 92 /*0x5C*/)) goto Le16;
    p++;
    if (!(p < n)) goto Le16;
    p++;
    continue;
Le16: p = sv16; }
    break;
    }
    if (!(p < n && s[p] == 39 /*0x27*/)) goto La10_4;
    p++;
    goto Lok10;
La10_4: p = sv10;
    if (!PpTok__m_punct(s, n, &p)) goto La10_5;
    goto Lok10;
La10_5: p = sv10;
    goto Lf9;
Lok10: ; }
    *io = p; return 1;
Lf9:
    return 0;
}
static int PpTok__b_top(PpTok__ctx* c, const unsigned char* s, size_t n, size_t* io) {
    size_t p = *io;
    (void)c; (void)s; (void)n;
    { size_t sv18;
    for (;;) { sv18 = p;
    { size_t sv19 = p;
    { size_t sv20 = p;
    if (!(p + 2 <= n && memcmp(s + p, PpTok__pool + 42, 2) == 0)) goto La20_0;   /* "\057\057" */
    p += 2;
#if defined(__SSE2__)
    { while (p + 16 <= n) {
        __m128i w21 = _mm_loadu_si128((const __m128i*)(s + p));
        __m128i m21 = _mm_setzero_si128();
        m21 = _mm_or_si128(m21, _mm_cmpeq_epi8(w21, _mm_set1_epi8((char)10)));
        { int hm21 = _mm_movemask_epi8(m21);
          if (hm21) { p += (size_t)__builtin_ctz((unsigned)hm21); break; } }
        p += 16;
    } }
#else
    { const unsigned long long L21 = 0x0101010101010101ULL, H21 = 0x8080808080808080ULL;
    while (p + 8 <= n) { unsigned long long w21; memcpy(&w21, s + p, 8);
        unsigned long long m21 = 0;
        { unsigned long long x = w21 ^ (L21 * 10); m21 |= (x - L21) & ~x & H21; }
        if (m21) { p += (size_t)(__builtin_ctzll(m21) >> 3); break; }
        p += 8;
    } }
#endif
    while (p < n && (PpTok__cs[1][((unsigned char)s[p]) >> 3] & (1u << (((unsigned char)s[p]) & 7u)))) p++;
    goto Lok20;
La20_0: p = sv20;
    if (!(p + 2 <= n && memcmp(s + p, PpTok__pool + 45, 2) == 0)) goto La20_1;   /* "\057\052" */
    p += 2;
    { size_t sv22;
    for (;;) { sv22 = p;
    {
    if (p >= n) goto Lb23;
    switch (s[p]) {
    case 42:
    if (!(p < n && s[p] == 42 /*0x2A*/)) goto Lb23;
    p++;
    if (!(p < n && (PpTok__cs[3][((unsigned char)s[p]) >> 3] & (1u << (((unsigned char)s[p]) & 7u))))) goto Lb23;
    p++;
    break;
    default:
    if (!(p < n && (PpTok__cs[2][((unsigned char)s[p]) >> 3] & (1u << (((unsigned char)s[p]) & 7u))))) goto Lb23;
    p++;
    break;
    }
    goto Lok23;
Lb23: goto Ly22;
Lok23: ; }
    if (p == sv22) break;
    }
    goto Lok22;
Ly22: p = sv22;
Lok22: ; }
    if (!(p + 2 <= n && memcmp(s + p, PpTok__pool + 59, 2) == 0)) goto La20_1;   /* "\052\057" */
    p += 2;
    goto Lok20;
La20_1: p = sv20;
    goto La19_0;
Lok20: ; }
    goto Lok19;
La19_0: p = sv19; PpTok__unwind(c, s + p);
    if (!(p < n && (s[p] == 9 || s[p] == 10 || s[p] == 13 || s[p] == 32))) goto La19_1;
    p++;
    while (p < n && (s[p] == 9 || s[p] == 10 || s[p] == 13 || s[p] == 32)) p++;
    goto Lok19;
La19_1: p = sv19; PpTok__unwind(c, s + p);
    if (!PpTok__b_tok(c, s, n, &p)) goto La19_2;
    goto Lok19;
La19_2: p = sv19; PpTok__unwind(c, s + p);
    goto Ly18;
Lok19: ; }
    if (p == sv18) break;
    }
    goto Lok18;
Ly18: p = sv18; PpTok__unwind(c, s + p);
Lok18: ; }
    *io = p; return 1;
Lf17:
    return 0;
}
static int PpTok__r_punct_multi(const unsigned char* s, size_t n, size_t* io) {
    size_t p = *io;
    (void)s; (void)n;
    { size_t sv25 = p;
    if (!(p + 3 <= n && memcmp(s + p, PpTok__pool + 149, 3) == 0)) goto La25_0;   /* "\056\056\056" */
    p += 3;
    goto Lok25;
La25_0: p = sv25;
    if (!(p + 3 <= n && memcmp(s + p, PpTok__pool + 153, 3) == 0)) goto La25_1;   /* "\074\074\075" */
    p += 3;
    goto Lok25;
La25_1: p = sv25;
    if (!(p + 3 <= n && memcmp(s + p, PpTok__pool + 157, 3) == 0)) goto La25_2;   /* "\076\076\075" */
    p += 3;
    goto Lok25;
La25_2: p = sv25;
    if (!(p + 2 <= n && memcmp(s + p, PpTok__pool + 161, 2) == 0)) goto La25_3;   /* "-\076" */
    p += 2;
    goto Lok25;
La25_3: p = sv25;
    if (!(p + 2 <= n && memcmp(s + p, PpTok__pool + 164, 2) == 0)) goto La25_4;   /* "\075\076" */
    p += 2;
    goto Lok25;
La25_4: p = sv25;
    if (!(p + 2 <= n && memcmp(s + p, PpTok__pool + 167, 2) == 0)) goto La25_5;   /* "\041\076" */
    p += 2;
    goto Lok25;
La25_5: p = sv25;
    if (!(p + 2 <= n && memcmp(s + p, PpTok__pool + 170, 2) == 0)) goto La25_6;   /* "\077\076" */
    p += 2;
    goto Lok25;
La25_6: p = sv25;
    if (!(p + 3 <= n && memcmp(s + p, PpTok__pool + 173, 3) == 0)) goto La25_7;   /* "\075\074\041" */
    p += 3;
    goto Lok25;
La25_7: p = sv25;
    if (!(p + 2 <= n && memcmp(s + p, PpTok__pool + 177, 2) == 0)) goto La25_8;   /* "\074\077" */
    p += 2;
    goto Lok25;
La25_8: p = sv25;
    if (!(p + 2 <= n && memcmp(s + p, PpTok__pool + 180, 2) == 0)) goto La25_9;   /* "\072\072" */
    p += 2;
    goto Lok25;
La25_9: p = sv25;
    if (!(p + 2 <= n && memcmp(s + p, PpTok__pool + 183, 2) == 0)) goto La25_10;   /* "\053\053" */
    p += 2;
    goto Lok25;
La25_10: p = sv25;
    if (!(p + 2 <= n && memcmp(s + p, PpTok__pool + 186, 2) == 0)) goto La25_11;   /* "--" */
    p += 2;
    goto Lok25;
La25_11: p = sv25;
    if (!(p + 2 <= n && memcmp(s + p, PpTok__pool + 189, 2) == 0)) goto La25_12;   /* "\074\074" */
    p += 2;
    goto Lok25;
La25_12: p = sv25;
    if (!(p + 2 <= n && memcmp(s + p, PpTok__pool + 192, 2) == 0)) goto La25_13;   /* "\076\076" */
    p += 2;
    goto Lok25;
La25_13: p = sv25;
    if (!(p + 2 <= n && memcmp(s + p, PpTok__pool + 195, 2) == 0)) goto La25_14;   /* "\074\075" */
    p += 2;
    goto Lok25;
La25_14: p = sv25;
    if (!(p + 2 <= n && memcmp(s + p, PpTok__pool + 198, 2) == 0)) goto La25_15;   /* "\076\075" */
    p += 2;
    goto Lok25;
La25_15: p = sv25;
    if (!(p + 2 <= n && memcmp(s + p, PpTok__pool + 201, 2) == 0)) goto La25_16;   /* "\075\075" */
    p += 2;
    goto Lok25;
La25_16: p = sv25;
    if (!(p + 2 <= n && memcmp(s + p, PpTok__pool + 204, 2) == 0)) goto La25_17;   /* "\041\075" */
    p += 2;
    goto Lok25;
La25_17: p = sv25;
    if (!(p + 2 <= n && memcmp(s + p, PpTok__pool + 207, 2) == 0)) goto La25_18;   /* "\046\046" */
    p += 2;
    goto Lok25;
La25_18: p = sv25;
    if (!(p + 2 <= n && memcmp(s + p, PpTok__pool + 210, 2) == 0)) goto La25_19;   /* "\174\174" */
    p += 2;
    goto Lok25;
La25_19: p = sv25;
    if (!(p + 2 <= n && memcmp(s + p, PpTok__pool + 213, 2) == 0)) goto La25_20;   /* "\052\075" */
    p += 2;
    goto Lok25;
La25_20: p = sv25;
    if (!(p + 2 <= n && memcmp(s + p, PpTok__pool + 216, 2) == 0)) goto La25_21;   /* "\057\075" */
    p += 2;
    goto Lok25;
La25_21: p = sv25;
    if (!(p + 2 <= n && memcmp(s + p, PpTok__pool + 219, 2) == 0)) goto La25_22;   /* "\045\075" */
    p += 2;
    goto Lok25;
La25_22: p = sv25;
    if (!(p + 2 <= n && memcmp(s + p, PpTok__pool + 222, 2) == 0)) goto La25_23;   /* "\053\075" */
    p += 2;
    goto Lok25;
La25_23: p = sv25;
    if (!(p + 2 <= n && memcmp(s + p, PpTok__pool + 225, 2) == 0)) goto La25_24;   /* "-\075" */
    p += 2;
    goto Lok25;
La25_24: p = sv25;
    if (!(p + 2 <= n && memcmp(s + p, PpTok__pool + 228, 2) == 0)) goto La25_25;   /* "\046\075" */
    p += 2;
    goto Lok25;
La25_25: p = sv25;
    if (!(p + 2 <= n && memcmp(s + p, PpTok__pool + 231, 2) == 0)) goto La25_26;   /* "\136\075" */
    p += 2;
    goto Lok25;
La25_26: p = sv25;
    if (!(p + 2 <= n && memcmp(s + p, PpTok__pool + 234, 2) == 0)) goto La25_27;   /* "\174\075" */
    p += 2;
    goto Lok25;
La25_27: p = sv25;
    if (!(p + 2 <= n && memcmp(s + p, PpTok__pool + 237, 2) == 0)) goto La25_28;   /* "\043\043" */
    p += 2;
    goto Lok25;
La25_28: p = sv25;
    goto Lf24;
Lok25: ; }
    *io = p; return 1;
Lf24:
    return 0;
}
static int PpTok__b_punct(PpTok__ctx* c, const unsigned char* s, size_t n, size_t* io) {
    size_t p = *io;
    (void)c; (void)s; (void)n;
    { size_t ka27 = p;
    { size_t sv28 = p;
    if (!PpTok__r_punct_multi(s, n, &p)) goto La28_0;
    goto Lok28;
La28_0: p = sv28;
    if (!(p < n && (PpTok__cs[12][((unsigned char)s[p]) >> 3] & (1u << (((unsigned char)s[p]) & 7u))))) goto La28_1;
    p++;
    goto Lok28;
La28_1: p = sv28;
    goto Lf26;
Lok28: ; }
    if (c->total == c->cap && !PpTok__tgrow(c)) goto Lf26;
    { PpTokNode* nd27 = &c->tape[c->total++];
      nd27->meta = 19u | (((unsigned long long)(p - ka27)) << 10);
      nd27->u.bytes = (const char*)(s + ka27); }
    }
    *io = p; return 1;
Lf26:
    return 0;
}
static int PpTok__b_tok(PpTok__ctx* c, const unsigned char* s, size_t n, size_t* io) {
    size_t p = *io;
    (void)c; (void)s; (void)n;
    { size_t sv30 = p;
    { size_t ka31 = p;
    if (!(p < n && (PpTok__cs[5][((unsigned char)s[p]) >> 3] & (1u << (((unsigned char)s[p]) & 7u))))) goto La30_0;
    p++;
    while (p < n && (PpTok__cs[6][((unsigned char)s[p]) >> 3] & (1u << (((unsigned char)s[p]) & 7u)))) p++;
    if (c->total == c->cap && !PpTok__tgrow(c)) goto La30_0;
    { PpTokNode* nd31 = &c->tape[c->total++];
      nd31->meta = 10u | (((unsigned long long)(p - ka31)) << 10);
      nd31->u.bytes = (const char*)(s + ka31); }
    }
    goto Lok30;
La30_0: p = sv30; PpTok__unwind(c, s + p);
    { size_t ka32 = p;
    {
    if (p >= n) goto Lb33;
    switch (s[p]) {
    case 48:
    case 49:
    case 50:
    case 51:
    case 52:
    case 53:
    case 54:
    case 55:
    case 56:
    case 57:
    if (!(p < n && ((unsigned)(s[p] - 48) <= 9u))) goto Lb33;
    p++;
    break;
    case 46:
    if (!(p < n && s[p] == 46 /*'.'*/)) goto Lb33;
    p++;
    if (!(p < n && ((unsigned)(s[p] - 48) <= 9u))) goto Lb33;
    p++;
    break;
    default: goto Lb33;
    }
    goto Lok33;
Lb33: goto La30_1;
Lok33: ; }
    { size_t sv34;
    for (;;) { sv34 = p;
    { size_t sv35 = p;
    if (!(p < n && (s[p] == 69 || s[p] == 80 || s[p] == 101 || s[p] == 112))) goto La35_0;
    p++;
    if (!(p < n && (s[p] == 43 || s[p] == 45))) goto La35_0;
    p++;
    goto Lok35;
La35_0: p = sv35;
    if (!(p < n && ((unsigned)(s[p] - 48) <= 9u))) goto La35_1;
    p++;
    goto Lok35;
La35_1: p = sv35;
    if (!(p < n && (PpTok__cs[5][((unsigned char)s[p]) >> 3] & (1u << (((unsigned char)s[p]) & 7u))))) goto La35_2;
    p++;
    goto Lok35;
La35_2: p = sv35;
    if (!(p < n && s[p] == 46 /*'.'*/)) goto La35_3;
    p++;
    goto Lok35;
La35_3: p = sv35;
    goto Ly34;
Lok35: ; }
    if (p == sv34) break;
    }
    goto Lok34;
Ly34: p = sv34;
Lok34: ; }
    if (c->total == c->cap && !PpTok__tgrow(c)) goto La30_1;
    { PpTokNode* nd32 = &c->tape[c->total++];
      nd32->meta = 9u | (((unsigned long long)(p - ka32)) << 10);
      nd32->u.bytes = (const char*)(s + ka32); }
    }
    goto Lok30;
La30_1: p = sv30; PpTok__unwind(c, s + p);
    { size_t ka36 = p;
    if (!(p < n && s[p] == 34 /*'"'*/)) goto La30_2;
    p++;
    for (;;) {
#if defined(__SSE2__)
    { while (p + 16 <= n) {
        __m128i w37 = _mm_loadu_si128((const __m128i*)(s + p));
        __m128i m37 = _mm_setzero_si128();
        m37 = _mm_cmpeq_epi8(_mm_min_epu8(w37, _mm_set1_epi8((char)10)), w37);
        m37 = _mm_or_si128(m37, _mm_cmpeq_epi8(w37, _mm_set1_epi8((char)34)));
        m37 = _mm_or_si128(m37, _mm_cmpeq_epi8(w37, _mm_set1_epi8((char)92)));
        { int hm37 = _mm_movemask_epi8(m37);
          if (hm37) { p += (size_t)__builtin_ctz((unsigned)hm37); break; } }
        p += 16;
    } }
#else
    { const unsigned long long L37 = 0x0101010101010101ULL, H37 = 0x8080808080808080ULL;
    while (p + 8 <= n) { unsigned long long w37; memcpy(&w37, s + p, 8);
        unsigned long long m37 = 0;
        m37 |= (w37 - L37 * 11) & ~w37 & H37;
        { unsigned long long x = w37 ^ (L37 * 34); m37 |= (x - L37) & ~x & H37; }
        { unsigned long long x = w37 ^ (L37 * 92); m37 |= (x - L37) & ~x & H37; }
        if (m37) { p += (size_t)(__builtin_ctzll(m37) >> 3); break; }
        p += 8;
    } }
#endif
    while (p < n && (PpTok__cs[9][((unsigned char)s[p]) >> 3] & (1u << (((unsigned char)s[p]) & 7u)))) p++;
    { size_t sv37 = p;
    if (!(p < n && s[p] == 92 /*0x5C*/)) goto Le37;
    p++;
    if (!(p < n)) goto Le37;
    p++;
    continue;
Le37: p = sv37; }
    break;
    }
    if (!(p < n && s[p] == 34 /*'"'*/)) goto La30_2;
    p++;
    if (c->total == c->cap && !PpTok__tgrow(c)) goto La30_2;
    { PpTokNode* nd36 = &c->tape[c->total++];
      nd36->meta = 14u | (((unsigned long long)(p - ka36)) << 10);
      nd36->u.bytes = (const char*)(s + ka36); }
    }
    goto Lok30;
La30_2: p = sv30; PpTok__unwind(c, s + p);
    { size_t ka38 = p;
    if (!(p < n && s[p] == 96 /*'`'*/)) goto La30_3;
    p++;
#if defined(__SSE2__)
    { while (p + 16 <= n) {
        __m128i w39 = _mm_loadu_si128((const __m128i*)(s + p));
        __m128i m39 = _mm_setzero_si128();
        m39 = _mm_or_si128(m39, _mm_cmpeq_epi8(w39, _mm_set1_epi8((char)96)));
        { int hm39 = _mm_movemask_epi8(m39);
          if (hm39) { p += (size_t)__builtin_ctz((unsigned)hm39); break; } }
        p += 16;
    } }
#else
    { const unsigned long long L39 = 0x0101010101010101ULL, H39 = 0x8080808080808080ULL;
    while (p + 8 <= n) { unsigned long long w39; memcpy(&w39, s + p, 8);
        unsigned long long m39 = 0;
        { unsigned long long x = w39 ^ (L39 * 96); m39 |= (x - L39) & ~x & H39; }
        if (m39) { p += (size_t)(__builtin_ctzll(m39) >> 3); break; }
        p += 8;
    } }
#endif
    while (p < n && (PpTok__cs[11][((unsigned char)s[p]) >> 3] & (1u << (((unsigned char)s[p]) & 7u)))) p++;
    if (!(p < n && s[p] == 96 /*'`'*/)) goto La30_3;
    p++;
    if (c->total == c->cap && !PpTok__tgrow(c)) goto La30_3;
    { PpTokNode* nd38 = &c->tape[c->total++];
      nd38->meta = 16u | (((unsigned long long)(p - ka38)) << 10);
      nd38->u.bytes = (const char*)(s + ka38); }
    }
    goto Lok30;
La30_3: p = sv30; PpTok__unwind(c, s + p);
    { size_t ka40 = p;
    if (!(p < n && s[p] == 39 /*0x27*/)) goto La30_4;
    p++;
    for (;;) {
#if defined(__SSE2__)
    { while (p + 16 <= n) {
        __m128i w41 = _mm_loadu_si128((const __m128i*)(s + p));
        __m128i m41 = _mm_setzero_si128();
        m41 = _mm_cmpeq_epi8(_mm_min_epu8(w41, _mm_set1_epi8((char)10)), w41);
        m41 = _mm_or_si128(m41, _mm_cmpeq_epi8(w41, _mm_set1_epi8((char)39)));
        m41 = _mm_or_si128(m41, _mm_cmpeq_epi8(w41, _mm_set1_epi8((char)92)));
        { int hm41 = _mm_movemask_epi8(m41);
          if (hm41) { p += (size_t)__builtin_ctz((unsigned)hm41); break; } }
        p += 16;
    } }
#else
    { const unsigned long long L41 = 0x0101010101010101ULL, H41 = 0x8080808080808080ULL;
    while (p + 8 <= n) { unsigned long long w41; memcpy(&w41, s + p, 8);
        unsigned long long m41 = 0;
        m41 |= (w41 - L41 * 11) & ~w41 & H41;
        { unsigned long long x = w41 ^ (L41 * 39); m41 |= (x - L41) & ~x & H41; }
        { unsigned long long x = w41 ^ (L41 * 92); m41 |= (x - L41) & ~x & H41; }
        if (m41) { p += (size_t)(__builtin_ctzll(m41) >> 3); break; }
        p += 8;
    } }
#endif
    while (p < n && (PpTok__cs[10][((unsigned char)s[p]) >> 3] & (1u << (((unsigned char)s[p]) & 7u)))) p++;
    { size_t sv41 = p;
    if (!(p < n && s[p] == 92 /*0x5C*/)) goto Le41;
    p++;
    if (!(p < n)) goto Le41;
    p++;
    continue;
Le41: p = sv41; }
    break;
    }
    if (!(p < n && s[p] == 39 /*0x27*/)) goto La30_4;
    p++;
    if (c->total == c->cap && !PpTok__tgrow(c)) goto La30_4;
    { PpTokNode* nd40 = &c->tape[c->total++];
      nd40->meta = 15u | (((unsigned long long)(p - ka40)) << 10);
      nd40->u.bytes = (const char*)(s + ka40); }
    }
    goto Lok30;
La30_4: p = sv30; PpTok__unwind(c, s + p);
    if (!PpTok__b_punct(c, s, n, &p)) goto La30_5;
    goto Lok30;
La30_5: p = sv30; PpTok__unwind(c, s + p);
    goto Lf29;
Lok30: ; }
    *io = p; return 1;
Lf29:
    return 0;
}
#define PpTok__ENTRY_M PpTok__m_top
#define PpTok__ENTRY_B PpTok__b_top
#define PpTok__ENTRY_PURE 0
static int PpTokNode_id(const PpTokNode* nd) { return (int)(nd->meta & 0xFFu); }
static int PpTokNode_is_list(const PpTokNode* nd) { return (int)((nd->meta >> 8) & 1u); }
static size_t PpTokNode_len(const PpTokNode* nd) { return (size_t)(nd->meta >> 10); }
static PpTokNode* PpTokNode_first(PpTokNode* nd) {
    return PpTokNode_is_list(nd) && (nd->meta >> 10) > 1 ? nd + 1 : 0; }
static PpTokNode* PpTokNode_next(PpTokNode* nd, PpTokNode* parent) {
    PpTokNode* nx = nd + (PpTokNode_is_list(nd) ? (size_t)(nd->meta >> 10) : 1);
    return nx < parent + (size_t)(parent->meta >> 10) ? nx : 0; }
static size_t PpTokNode_count(PpTokNode* nd) {
    size_t k = 0;
    for (PpTokNode* ch = PpTokNode_first(nd); ch; ch = PpTokNode_next(ch, nd)) k++;
    return k; }
static void PpTokNode_slice(const PpTokNode* nd, CCSlice* out) {
    size_t l = (size_t)(nd->meta >> 10);
    if ((nd->meta >> 9) & 1u)
        *out = cc_slice_from_parts((void*)nd->u.bytes, l,
                   cc_slice_make_id(3ULL, true, false, false), l);
    else
        *out = cc_slice_from_buffer((void*)nd->u.bytes, l);
}
#include <ccc/vendor/ffc.h>
static inline double PpTokNode_as_f64(const PpTokNode* nd) {
    if ((nd->meta >> 8) & 1u) return 0.0;
    { double v = 0.0; size_t l = (size_t)(nd->meta >> 10);
      ffc_parse_options o; o.format = FFC_PRESET_JSON; o.decimal_point = '.';
      ffc_result r = ffc_from_chars_double_options(
          nd->u.bytes, nd->u.bytes + l, &v, o);
      return r.outcome == FFC_OUTCOME_OK ? v : 0.0; }
}
static int PpTok_match(const char* s, size_t n) {
    size_t p = 0;
    if (!PpTok__ENTRY_M((const unsigned char*)s, n, &p)) return 0;
    return p == n;
}
static PpTokNode* PpTok_parse(const char* s, size_t n, CCArena* arena) {
    PpTok__ctx c0;
    size_t p = 0;
    c0.cap = 2048 + (n / 24);  /* ~real density; tgrow covers denser input */
    c0.tape = (PpTokNode*)cc_arena_alloc_local(arena, c0.cap * sizeof(PpTokNode), _Alignof(PpTokNode));
    if (!c0.tape) return 0;
    c0.total = 1; c0.bdepth = 0; c0.arena = arena;
    c0.mat_ptr = 0; c0.mat_len = 0; c0.mat_cow = 0;
    c0.sb = 0; c0.vstack = 0; c0.vanchor = 0; c0.vsp = 0; c0.vcap = 0;
    c0.tape[0].meta = 0x100u | 0xFFu;   /* root list */
    c0.tape[0].u.bytes = s;             /* source anchor */
#if PpTok__ENTRY_PURE
    if (!PpTok__ENTRY_B((const unsigned char*)s, n, &p) || p != n) return 0;
#else
    if (!PpTok__ENTRY_B(&c0, (const unsigned char*)s, n, &p) || p != n) return 0;
#endif
    c0.tape[0].meta |= ((unsigned long long)c0.total) << 10;
    return c0.tape;
}
static int PpTok_collect(const char* s, size_t n, CCArena* arena,
        int (*cb)(void* env, int id, CCSlice v), void* env) {
    PpTokNode* tape = PpTok_parse(s, n, arena);
    if (!tape) return 0;
    { size_t total = (size_t)(tape[0].meta >> 10);
    for (size_t t = 1; t < total; t++) {
        if ((tape[t].meta >> 8) & 1u) continue;   /* interior */
        { CCSlice v; PpTokNode_slice(&tape[t], &v);
          if (cb(env, (int)(tape[t].meta & 0xFFu), v)) return 0; }
    } }
    return 1;
}
#line 18 "examples/serdes/c/shadow_lower.ccs"
/* @grammar(rules) PpStmt (line 18): 12 rule(s). API:
 *   cc_match(PpStmt, s, n)                     -> PpStmt_match
 *   cc_parse(PpStmt, s, n, arena)              -> PpStmt_parse -> PpStmtNode* tape
 *   cc_collect(PpStmt, s, n, arena, cb, env)   -> PpStmt_collect
 *   nodes: PpStmtNode_{id,is_list,len,first,next,count,slice}; ids: PpStmt_KEEP_<rule>
 */
typedef struct { int rule_count; } PpStmt;
static inline int PpStmt_rule_count(void) { return 12; }
typedef struct { unsigned long long meta;
    union { const char* bytes; } u; } PpStmtNode;
struct CCShapeB; struct CCShapeVal;
typedef struct { PpStmtNode* tape; size_t total, cap;
    size_t bstack[512]; size_t bdepth; CCArena* arena;
    const char** mat_ptr; size_t* mat_len; unsigned char* mat_cow;
    /* shaped-DOM hook (armed by _dom; NULL for plain parse):
     * completed containers hand their value up this stack —
     * one push per container, anchored for derived restore */
    struct CCShapeB* sb;
    struct CCShapeVal* vstack; const unsigned char** vanchor;
    size_t vsp, vcap;
    size_t vbase[512]; } PpStmt__ctx;
static __attribute__((noinline)) int PpStmt__tgrow(PpStmt__ctx* c) {
    size_t nc = c->cap * 2;
    PpStmtNode* nt = (PpStmtNode*)cc_arena_realloc(c->arena, c->arena, c->tape,
        c->cap * sizeof(PpStmtNode), nc * sizeof(PpStmtNode), _Alignof(PpStmtNode));
    if (!nt) return 0;
    c->tape = nt;
    c->cap = nc; return 1;
}
static __attribute__((noinline)) void PpStmt__unwind(PpStmt__ctx* c, const unsigned char* sv) {
    while (c->total > 1 && (const unsigned char*)c->tape[c->total - 1].u.bytes >= sv)
        c->total--;
    while (c->bdepth > 0 && c->bstack[c->bdepth - 1] >= c->total) c->bdepth--;
    if (c->vanchor)
        while (c->vsp > 0 && c->vanchor[c->vsp - 1] >= sv) c->vsp--;
}
#define PpStmt_KEEP_ident 4
#define PpStmt_KEEP_break_stmt 5
#define PpStmt_KEEP_continue_stmt 6
#define PpStmt_KEEP_goto_stmt 7
#define PpStmt_KEEP_label_stmt 8
#define PpStmt_KEEP_raw_line 9
static const unsigned char PpStmt__pool[185] = {98,114,101,97,107,95,115,116,109,116,0,99,111,110,116,105,110,117,101,95,115,116,109,116,0,103,111,116,111,95,115,116,109,116,0,108,97,98,101,108,95,115,116,109,116,0,114,97,119,95,108,105,110,101,0,110,111,110,100,105,103,105,116,0,105,100,99,111,110,116,0,98,114,101,97,107,0,119,115,0,59,0,99,111,110,116,105,110,117,101,0,119,115,0,59,0,103,111,116,111,0,119,115,0,105,100,101,110,116,0,119,115,0,59,0,105,100,101,110,116,0,119,115,0,58,0,59,0,98,114,101,97,107,95,115,116,109,116,0,99,111,110,116,105,110,117,101,95,115,116,109,116,0,103,111,116,111,95,115,116,109,116,0,108,97,98,101,108,95,115,116,109,116,0,115,116,114,117,99,116,117,114,101,100,0,};
#if defined(__SSE2__)
#include <emmintrin.h>
#endif
static const unsigned char PpStmt__cs[4][32] = {
  /* cs0: tab-nl cr ' ' */
  {0,38,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,},
  /* cs1: 'A'-'Z' '_' 'a'-'z' */
  {0,0,0,0,0,0,0,0,254,255,255,135,254,255,255,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,},
  /* cs2: '0'-'9' 'A'-'Z' '_' 'a'-'z' */
  {0,0,0,0,0,0,255,3,254,255,255,135,254,255,255,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,},
  /* cs3: 0x00-tab 0x0B-':' '<'-'z' '|' '~'-0xFF */
  {255,251,255,255,255,255,255,247,255,255,255,255,255,255,255,215,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,},
};
static int PpStmt__m_stmt(const unsigned char* s, size_t n, size_t* io);
static int PpStmt__b_stmt(PpStmt__ctx* c, const unsigned char* s, size_t n, size_t* io);
static int PpStmt__m_stmt(const unsigned char* s, size_t n, size_t* io) {
    size_t p = *io;
    (void)s; (void)n;
    { size_t sv1 = p;
    if (!(p + 5 <= n)) goto La1_0;
    { unsigned int w; memcpy(&w, s + p, 4); if (w != 1634038370u || s[p + 4] != 107) goto La1_0; }  /* "break" */
    p += 5;
    while (p < n && (s[p] == 9 || s[p] == 10 || s[p] == 13 || s[p] == 32)) p++;
    if (!(p < n && s[p] == 59 /*';'*/)) goto La1_0;
    p++;
    goto Lok1;
La1_0: p = sv1;
    if (!(p + 8 <= n && memcmp(s + p, PpStmt__pool + 82, 8) == 0)) goto La1_1;   /* "continue" */
    p += 8;
    while (p < n && (s[p] == 9 || s[p] == 10 || s[p] == 13 || s[p] == 32)) p++;
    if (!(p < n && s[p] == 59 /*';'*/)) goto La1_1;
    p++;
    goto Lok1;
La1_1: p = sv1;
    if (!(p + 4 <= n)) goto La1_2;
    { unsigned int w; memcpy(&w, s + p, 4); if (w != 1869901671u) goto La1_2; }  /* "goto" */
    p += 4;
    while (p < n && (s[p] == 9 || s[p] == 10 || s[p] == 13 || s[p] == 32)) p++;
    if (!(p < n && (PpStmt__cs[1][((unsigned char)s[p]) >> 3] & (1u << (((unsigned char)s[p]) & 7u))))) goto La1_2;
    p++;
    while (p < n && (PpStmt__cs[2][((unsigned char)s[p]) >> 3] & (1u << (((unsigned char)s[p]) & 7u)))) p++;
    while (p < n && (s[p] == 9 || s[p] == 10 || s[p] == 13 || s[p] == 32)) p++;
    if (!(p < n && s[p] == 59 /*';'*/)) goto La1_2;
    p++;
    goto Lok1;
La1_2: p = sv1;
    if (!(p < n && (PpStmt__cs[1][((unsigned char)s[p]) >> 3] & (1u << (((unsigned char)s[p]) & 7u))))) goto La1_3;
    p++;
    while (p < n && (PpStmt__cs[2][((unsigned char)s[p]) >> 3] & (1u << (((unsigned char)s[p]) & 7u)))) p++;
    while (p < n && (s[p] == 9 || s[p] == 10 || s[p] == 13 || s[p] == 32)) p++;
    if (!(p < n && s[p] == 58 /*':'*/)) goto La1_3;
    p++;
    goto Lok1;
La1_3: p = sv1;
    if (!(p < n && (PpStmt__cs[3][((unsigned char)s[p]) >> 3] & (1u << (((unsigned char)s[p]) & 7u))))) goto La1_4;
    p++;
#if defined(__SSE2__)
    { while (p + 16 <= n) {
        __m128i w2 = _mm_loadu_si128((const __m128i*)(s + p));
        __m128i m2 = _mm_setzero_si128();
        m2 = _mm_cmpeq_epi8(_mm_min_epu8(w2, _mm_set1_epi8((char)59)), w2);
        m2 = _mm_or_si128(m2, _mm_cmpeq_epi8(w2, _mm_set1_epi8((char)123)));
        m2 = _mm_or_si128(m2, _mm_cmpeq_epi8(w2, _mm_set1_epi8((char)125)));
        { int hm2 = _mm_movemask_epi8(m2);
          if (hm2) { p += (size_t)__builtin_ctz((unsigned)hm2); break; } }
        p += 16;
    } }
#else
    { const unsigned long long L2 = 0x0101010101010101ULL, H2 = 0x8080808080808080ULL;
    while (p + 8 <= n) { unsigned long long w2; memcpy(&w2, s + p, 8);
        unsigned long long m2 = 0;
        m2 |= (w2 - L2 * 60) & ~w2 & H2;
        { unsigned long long x = w2 ^ (L2 * 123); m2 |= (x - L2) & ~x & H2; }
        { unsigned long long x = w2 ^ (L2 * 125); m2 |= (x - L2) & ~x & H2; }
        if (m2) { p += (size_t)(__builtin_ctzll(m2) >> 3); break; }
        p += 8;
    } }
#endif
    while (p < n && (PpStmt__cs[3][((unsigned char)s[p]) >> 3] & (1u << (((unsigned char)s[p]) & 7u)))) p++;
    if (!(p < n && s[p] == 59 /*';'*/)) goto La1_4;
    p++;
    goto Lok1;
La1_4: p = sv1;
    goto Lf0;
Lok1: ; }
    *io = p; return 1;
Lf0:
    return 0;
}
static int PpStmt__b_stmt(PpStmt__ctx* c, const unsigned char* s, size_t n, size_t* io) {
    size_t p = *io;
    (void)c; (void)s; (void)n;
    { size_t sv4 = p; size_t lt4 = c->total, ld4 = c->bdepth, lv4 = c->vsp;
    { size_t ka5 = p;
    if (!(p + 5 <= n)) goto La4_0;
    { unsigned int w; memcpy(&w, s + p, 4); if (w != 1634038370u || s[p + 4] != 107) goto La4_0; }  /* "break" */
    p += 5;
    while (p < n && (s[p] == 9 || s[p] == 10 || s[p] == 13 || s[p] == 32)) p++;
    if (!(p < n && s[p] == 59 /*';'*/)) goto La4_0;
    p++;
    if (c->total == c->cap && !PpStmt__tgrow(c)) goto La4_0;
    { PpStmtNode* nd5 = &c->tape[c->total++];
      nd5->meta = 5u | (((unsigned long long)(p - ka5)) << 10);
      nd5->u.bytes = (const char*)(s + ka5); }
    }
    goto Lok4;
La4_0: p = sv4; { c->total = lt4; c->bdepth = ld4; c->vsp = lv4; }
    { size_t ka6 = p;
    if (!(p + 8 <= n && memcmp(s + p, PpStmt__pool + 82, 8) == 0)) goto La4_1;   /* "continue" */
    p += 8;
    while (p < n && (s[p] == 9 || s[p] == 10 || s[p] == 13 || s[p] == 32)) p++;
    if (!(p < n && s[p] == 59 /*';'*/)) goto La4_1;
    p++;
    if (c->total == c->cap && !PpStmt__tgrow(c)) goto La4_1;
    { PpStmtNode* nd6 = &c->tape[c->total++];
      nd6->meta = 6u | (((unsigned long long)(p - ka6)) << 10);
      nd6->u.bytes = (const char*)(s + ka6); }
    }
    goto Lok4;
La4_1: p = sv4; { c->total = lt4; c->bdepth = ld4; c->vsp = lv4; }
    { size_t ka7 = p;
    if (!(p + 4 <= n)) goto La4_2;
    { unsigned int w; memcpy(&w, s + p, 4); if (w != 1869901671u) goto La4_2; }  /* "goto" */
    p += 4;
    while (p < n && (s[p] == 9 || s[p] == 10 || s[p] == 13 || s[p] == 32)) p++;
    { size_t ka8 = p;
    if (!(p < n && (PpStmt__cs[1][((unsigned char)s[p]) >> 3] & (1u << (((unsigned char)s[p]) & 7u))))) goto La4_2;
    p++;
    while (p < n && (PpStmt__cs[2][((unsigned char)s[p]) >> 3] & (1u << (((unsigned char)s[p]) & 7u)))) p++;
    if (c->total == c->cap && !PpStmt__tgrow(c)) goto La4_2;
    { PpStmtNode* nd8 = &c->tape[c->total++];
      nd8->meta = 4u | (((unsigned long long)(p - ka8)) << 10);
      nd8->u.bytes = (const char*)(s + ka8); }
    }
    while (p < n && (s[p] == 9 || s[p] == 10 || s[p] == 13 || s[p] == 32)) p++;
    if (!(p < n && s[p] == 59 /*';'*/)) goto La4_2;
    p++;
    if (c->total == c->cap && !PpStmt__tgrow(c)) goto La4_2;
    { PpStmtNode* nd7 = &c->tape[c->total++];
      nd7->meta = 7u | (((unsigned long long)(p - ka7)) << 10);
      nd7->u.bytes = (const char*)(s + ka7); }
    }
    goto Lok4;
La4_2: p = sv4; { c->total = lt4; c->bdepth = ld4; c->vsp = lv4; }
    { size_t ka9 = p;
    { size_t ka10 = p;
    if (!(p < n && (PpStmt__cs[1][((unsigned char)s[p]) >> 3] & (1u << (((unsigned char)s[p]) & 7u))))) goto La4_3;
    p++;
    while (p < n && (PpStmt__cs[2][((unsigned char)s[p]) >> 3] & (1u << (((unsigned char)s[p]) & 7u)))) p++;
    if (c->total == c->cap && !PpStmt__tgrow(c)) goto La4_3;
    { PpStmtNode* nd10 = &c->tape[c->total++];
      nd10->meta = 4u | (((unsigned long long)(p - ka10)) << 10);
      nd10->u.bytes = (const char*)(s + ka10); }
    }
    while (p < n && (s[p] == 9 || s[p] == 10 || s[p] == 13 || s[p] == 32)) p++;
    if (!(p < n && s[p] == 58 /*':'*/)) goto La4_3;
    p++;
    if (c->total == c->cap && !PpStmt__tgrow(c)) goto La4_3;
    { PpStmtNode* nd9 = &c->tape[c->total++];
      nd9->meta = 8u | (((unsigned long long)(p - ka9)) << 10);
      nd9->u.bytes = (const char*)(s + ka9); }
    }
    goto Lok4;
La4_3: p = sv4; { c->total = lt4; c->bdepth = ld4; c->vsp = lv4; }
    { size_t ka11 = p;
    if (!(p < n && (PpStmt__cs[3][((unsigned char)s[p]) >> 3] & (1u << (((unsigned char)s[p]) & 7u))))) goto La4_4;
    p++;
#if defined(__SSE2__)
    { while (p + 16 <= n) {
        __m128i w12 = _mm_loadu_si128((const __m128i*)(s + p));
        __m128i m12 = _mm_setzero_si128();
        m12 = _mm_cmpeq_epi8(_mm_min_epu8(w12, _mm_set1_epi8((char)59)), w12);
        m12 = _mm_or_si128(m12, _mm_cmpeq_epi8(w12, _mm_set1_epi8((char)123)));
        m12 = _mm_or_si128(m12, _mm_cmpeq_epi8(w12, _mm_set1_epi8((char)125)));
        { int hm12 = _mm_movemask_epi8(m12);
          if (hm12) { p += (size_t)__builtin_ctz((unsigned)hm12); break; } }
        p += 16;
    } }
#else
    { const unsigned long long L12 = 0x0101010101010101ULL, H12 = 0x8080808080808080ULL;
    while (p + 8 <= n) { unsigned long long w12; memcpy(&w12, s + p, 8);
        unsigned long long m12 = 0;
        m12 |= (w12 - L12 * 60) & ~w12 & H12;
        { unsigned long long x = w12 ^ (L12 * 123); m12 |= (x - L12) & ~x & H12; }
        { unsigned long long x = w12 ^ (L12 * 125); m12 |= (x - L12) & ~x & H12; }
        if (m12) { p += (size_t)(__builtin_ctzll(m12) >> 3); break; }
        p += 8;
    } }
#endif
    while (p < n && (PpStmt__cs[3][((unsigned char)s[p]) >> 3] & (1u << (((unsigned char)s[p]) & 7u)))) p++;
    if (!(p < n && s[p] == 59 /*';'*/)) goto La4_4;
    p++;
    if (c->total == c->cap && !PpStmt__tgrow(c)) goto La4_4;
    { PpStmtNode* nd11 = &c->tape[c->total++];
      nd11->meta = 9u | (((unsigned long long)(p - ka11)) << 10);
      nd11->u.bytes = (const char*)(s + ka11); }
    }
    goto Lok4;
La4_4: p = sv4; { c->total = lt4; c->bdepth = ld4; c->vsp = lv4; }
    goto Lf3;
Lok4: ; }
    *io = p; return 1;
Lf3:
    return 0;
}
#define PpStmt__ENTRY_M PpStmt__m_stmt
#define PpStmt__ENTRY_B PpStmt__b_stmt
#define PpStmt__ENTRY_PURE 0
static int PpStmtNode_id(const PpStmtNode* nd) { return (int)(nd->meta & 0xFFu); }
static int PpStmtNode_is_list(const PpStmtNode* nd) { return (int)((nd->meta >> 8) & 1u); }
static size_t PpStmtNode_len(const PpStmtNode* nd) { return (size_t)(nd->meta >> 10); }
static PpStmtNode* PpStmtNode_first(PpStmtNode* nd) {
    return PpStmtNode_is_list(nd) && (nd->meta >> 10) > 1 ? nd + 1 : 0; }
static PpStmtNode* PpStmtNode_next(PpStmtNode* nd, PpStmtNode* parent) {
    PpStmtNode* nx = nd + (PpStmtNode_is_list(nd) ? (size_t)(nd->meta >> 10) : 1);
    return nx < parent + (size_t)(parent->meta >> 10) ? nx : 0; }
static size_t PpStmtNode_count(PpStmtNode* nd) {
    size_t k = 0;
    for (PpStmtNode* ch = PpStmtNode_first(nd); ch; ch = PpStmtNode_next(ch, nd)) k++;
    return k; }
static void PpStmtNode_slice(const PpStmtNode* nd, CCSlice* out) {
    size_t l = (size_t)(nd->meta >> 10);
    if ((nd->meta >> 9) & 1u)
        *out = cc_slice_from_parts((void*)nd->u.bytes, l,
                   cc_slice_make_id(3ULL, true, false, false), l);
    else
        *out = cc_slice_from_buffer((void*)nd->u.bytes, l);
}
#include <ccc/vendor/ffc.h>
static inline double PpStmtNode_as_f64(const PpStmtNode* nd) {
    if ((nd->meta >> 8) & 1u) return 0.0;
    { double v = 0.0; size_t l = (size_t)(nd->meta >> 10);
      ffc_parse_options o; o.format = FFC_PRESET_JSON; o.decimal_point = '.';
      ffc_result r = ffc_from_chars_double_options(
          nd->u.bytes, nd->u.bytes + l, &v, o);
      return r.outcome == FFC_OUTCOME_OK ? v : 0.0; }
}
static int PpStmt_match(const char* s, size_t n) {
    size_t p = 0;
    if (!PpStmt__ENTRY_M((const unsigned char*)s, n, &p)) return 0;
    return p == n;
}
static PpStmtNode* PpStmt_parse(const char* s, size_t n, CCArena* arena) {
    PpStmt__ctx c0;
    size_t p = 0;
    c0.cap = 2048 + (n / 24);  /* ~real density; tgrow covers denser input */
    c0.tape = (PpStmtNode*)cc_arena_alloc_local(arena, c0.cap * sizeof(PpStmtNode), _Alignof(PpStmtNode));
    if (!c0.tape) return 0;
    c0.total = 1; c0.bdepth = 0; c0.arena = arena;
    c0.mat_ptr = 0; c0.mat_len = 0; c0.mat_cow = 0;
    c0.sb = 0; c0.vstack = 0; c0.vanchor = 0; c0.vsp = 0; c0.vcap = 0;
    c0.tape[0].meta = 0x100u | 0xFFu;   /* root list */
    c0.tape[0].u.bytes = s;             /* source anchor */
#if PpStmt__ENTRY_PURE
    if (!PpStmt__ENTRY_B((const unsigned char*)s, n, &p) || p != n) return 0;
#else
    if (!PpStmt__ENTRY_B(&c0, (const unsigned char*)s, n, &p) || p != n) return 0;
#endif
    c0.tape[0].meta |= ((unsigned long long)c0.total) << 10;
    return c0.tape;
}
static int PpStmt_collect(const char* s, size_t n, CCArena* arena,
        int (*cb)(void* env, int id, CCSlice v), void* env) {
    PpStmtNode* tape = PpStmt_parse(s, n, arena);
    if (!tape) return 0;
    { size_t total = (size_t)(tape[0].meta >> 10);
    for (size_t t = 1; t < total; t++) {
        if ((tape[t].meta >> 8) & 1u) continue;   /* interior */
        { CCSlice v; PpStmtNode_slice(&tape[t], &v);
          if (cb(env, (int)(tape[t].meta & 0xFFu), v)) return 0; }
    } }
    return 1;
}
#line 22 "examples/serdes/c/shadow_lower.ccs"
#include "c_pp_spike.h"
#include "shadow_build.h"

#include "shadow_tcc_compile.h"
#include "shadow_comptime.h"

static int shadow_keep_nop(void* env, int id, CCSlice v) {
    (void)env; (void)id; (void)v;
    return 0;
}

static void usage(const char* argv0) {
    fprintf(stderr,
            "usage: %s <input.ccs|.cch> [-o output] [--exe binary]\n"
            "          [--no-cache] [--verbose] [--release] [--debug] [--dry-run]\n"
            "          [--no-runtime] [--cc-flags=FLAGS] [--ld-flags=FLAGS]\n"
            "  -o out.c/.h  lowered text\n"
            "  -o binary    host-cc build with emit/obj cache\n"
            "  --exe        libtcc from emit buffer (no .c on disk)\n"
            "  Host opts are the ccc↔shadow_lower contract; unknown flags error.\n",
            argv0);
}

static const char* dirname_of(const char* path, char* buf, size_t buflen) {
    snprintf(buf, buflen, "%s", path);
    char* slash = strrchr(buf, '/');
    if (!slash) {
        snprintf(buf, buflen, ".");
        return buf;
    }
    *slash = 0;
    if (!buf[0]) snprintf(buf, buflen, "/");
    return buf;
}

static int ends_with(const char* s, const char* suf) {
    size_t n = strlen(s), m = strlen(suf);
    return n >= m && strcmp(s + n - m, suf) == 0;
}

static void pp_free_macros(Pp* pp) {
    for (int i = 0; i < pp->nmacros; i++) free(pp->macros[i].body);
    pp->nmacros = 0;
}

enum { SHADOW_DEP_CAP = 256 };

/* Deduping push into dep path table (borrowed C strings).
 * Returns 1 on success/dup, 0 if the table is full (fail-loud). */
static int shadow_dep_add(const char** paths, int* n, const char* path) {
    int i;
    if (!path || !path[0] || !paths || !n) return 1;
    if (*n >= SHADOW_DEP_CAP) {
        fprintf(stderr,
                "error: emit dep table full (%d paths); raise SHADOW_DEP_CAP\n",
                SHADOW_DEP_CAP);
        return 0;
    }
    for (i = 0; i < *n; i++) {
        if (strcmp(paths[i], path) == 0) return 1;
    }
    paths[(*n)++] = path;
    return 1;
}

/* `#pragma cc_depends("rel")` — resolve relative to in_path's directory.
 * Owned strings go into owned[]; caller frees.
 * Returns 0 on success, -1 if the dep table overflowed. */
static int shadow_dep_scan_cc_depends(const char* in_path, const char** paths,
                                      int* n, char** owned, int* nowned) {
    FILE* f;
    char* src = NULL;
    size_t rd = 0, i = 0;
    char dir[512];
    if (!in_path || !paths || !n || !owned || !nowned) return 0;
    dirname_of(in_path, dir, sizeof(dir));
    f = fopen(in_path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    {
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0 || sz > 16 * 1024 * 1024) {
            fclose(f);
            return 0;
        }
        src = (char*)malloc((size_t)sz + 1);
        if (!src) {
            fclose(f);
            return 0;
        }
        rd = fread(src, 1, (size_t)sz, f);
        src[rd] = 0;
    }
    fclose(f);
    while (i < rd) {
        size_t p = i;
        while (p < rd && (src[p] == ' ' || src[p] == '\t')) p++;
        if (p < rd && src[p] == '#') {
            p++;
            while (p < rd && (src[p] == ' ' || src[p] == '\t')) p++;
            if (p + 6 <= rd && memcmp(src + p, "pragma", 6) == 0) {
                p += 6;
                while (p < rd && (src[p] == ' ' || src[p] == '\t')) p++;
                if (p + 10 <= rd && memcmp(src + p, "cc_depends", 10) == 0) {
                    p += 10;
                    while (p < rd && (src[p] == ' ' || src[p] == '\t')) p++;
                    if (p < rd && src[p] == '(') {
                        p++;
                        while (p < rd && (src[p] == ' ' || src[p] == '\t')) p++;
                        if (p < rd && src[p] == '"') {
                            char rel[512];
                            char abs[1024];
                            size_t o = 0;
                            p++;
                            while (p < rd && src[p] != '"' && o + 1 < sizeof(rel))
                                rel[o++] = src[p++];
                            rel[o] = 0;
                            if (rel[0] == '/')
                                snprintf(abs, sizeof(abs), "%s", rel);
                            else
                                snprintf(abs, sizeof(abs), "%s/%s", dir, rel);
                            if (*nowned >= SHADOW_DEP_CAP) {
                                fprintf(stderr,
                                        "error: emit dep table full (%d paths); "
                                        "raise SHADOW_DEP_CAP\n",
                                        SHADOW_DEP_CAP);
                                free(src);
                                return -1;
                            }
                            owned[*nowned] = strdup(abs);
                            if (owned[*nowned]) {
                                if (!shadow_dep_add(paths, n, owned[*nowned])) {
                                    free(owned[*nowned]);
                                    owned[*nowned] = NULL;
                                    free(src);
                                    return -1;
                                }
                                (*nowned)++;
                            }
                        }
                    }
                }
            }
        }
        while (i < rd && src[i] != '\n') i++;
        if (i < rd) i++;
    }
    free(src);
    return 0;
}

/* Collect TapeCache + harvested .cch + cc_depends into paths[].
 * Returns 0 on success, -1 if the dep table overflowed. */
static int shadow_collect_emit_deps(TapeCache* cache, const char* in_path,
                                    const char** paths, int* n, char** owned,
                                    int* nowned) {
    int i;
    size_t ci, cn;
    *n = 0;
    *nowned = 0;
    if (cache) {
        for (i = 0; i < cache->n; i++) {
            if (cache->items[i] && cache->items[i]->path &&
                !shadow_dep_add(paths, n, cache->items[i]->path))
                return -1;
        }
    }
    cn = cc_included_cch_source_count();
    for (ci = 0; ci < cn; ci++) {
        if (!shadow_dep_add(paths, n, cc_included_cch_source_path(ci)))
            return -1;
    }
    if (shadow_dep_scan_cc_depends(in_path, paths, n, owned, nowned) != 0)
        return -1;
    return 0;
}

/* Directory containing libcc_runtime.{dylib,so}. Override: SHADOW_RUNTIME_LIBDIR. */
static int shadow_runtime_libdir(char* dst, size_t cap) {
    const char* env = getenv("SHADOW_RUNTIME_LIBDIR");
    const char* cands[4];
    char probe[1100];
    int i;
    int n = 0;
    if (env && env[0]) {
#if defined(__APPLE__)
        snprintf(probe, sizeof(probe), "%s/libcc_runtime.dylib", env);
#else
        snprintf(probe, sizeof(probe), "%s/libcc_runtime.so", env);
#endif
        if (access(probe, R_OK) != 0) {
            fprintf(stderr, "error: SHADOW_RUNTIME_LIBDIR missing libcc_runtime: %s\n",
                    env);
            return 0;
        }
        snprintf(dst, cap, "%s", env);
        return 1;
    }
    cands[n++] = "out/cc/lib";
    cands[n++] = "cc/lib";
    for (i = 0; i < n; i++) {
#if defined(__APPLE__)
        snprintf(probe, sizeof(probe), "%s/libcc_runtime.dylib", cands[i]);
#else
        snprintf(probe, sizeof(probe), "%s/libcc_runtime.so", cands[i]);
#endif
        if (access(probe, R_OK) != 0) continue;
        snprintf(dst, cap, "%s", cands[i]);
        return 1;
    }
    fprintf(stderr,
            "error: missing CC runtime shared lib "
            "(build: make -C cc ../out/cc/lib/libcc_runtime.*)\n");
    return 0;
}

int main(int argc, char** argv) {
    CCArena gate = cc_arena_create(4096);
    (void)PpTok_match(";", 1);
    (void)PpTok_collect(";", 1, &gate, shadow_keep_nop, NULL);
    /* Statement whitelist grammar gate (Phase 2). */
    if (!PpStmt_match("break;", 6) || !PpStmt_match("continue;", 9) ||
        !PpStmt_match("goto L;", 7) || !PpStmt_match("L:", 2)) {
        fprintf(stderr, "error: PpStmt selfcheck match failed\n");
        return 1;
    }
    if (!PpStmt_match("x = 1;", 6)) {
        fprintf(stderr, "error: PpStmt raw_line selfcheck failed\n");
        return 1;
    }
    if (PpStmt_parse("@@@", 3, &gate)) {
        fprintf(stderr, "error: PpStmt accepted non-statement\n");
        return 1;
    }
    cc_arena_reset(&gate);
    (void)PpStmt_collect("break;", 6, &gate, shadow_keep_nop, NULL);
    cc_arena_free(&gate);

    /* Typed emit-template schema check — fails the self-build, not user TUs. */
    if (!shadow_tpl_selfcheck()) return 1;

    const char* in_path = NULL;
    const char* out_path = NULL;
    const char* exe_path = NULL;
    int no_cache = 0;
    memset(&g_shadow_host_opts, 0, sizeof(g_shadow_host_opts));
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) { usage(argv[0]); return 2; }
            out_path = argv[++i];
        } else if (strcmp(argv[i], "--exe") == 0) {
            if (i + 1 >= argc) { usage(argv[0]); return 2; }
            exe_path = argv[++i];
        } else if (strcmp(argv[i], "--no-cache") == 0) {
            no_cache = 1;
            g_shadow_host_opts.no_cache = 1;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            g_shadow_host_opts.verbose = 1;
        } else if (strcmp(argv[i], "--release") == 0) {
            g_shadow_host_opts.opt_release = 1;
        } else if (strcmp(argv[i], "--debug") == 0) {
            g_shadow_host_opts.opt_debug = 1;
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            g_shadow_host_opts.dry_run = 1;
        } else if (strcmp(argv[i], "--no-runtime") == 0) {
            g_shadow_host_opts.no_runtime = 1;
        } else if (strncmp(argv[i], "--cc-flags=", 11) == 0) {
            g_shadow_host_opts.cc_flags = argv[i] + 11;
        } else if (strncmp(argv[i], "--ld-flags=", 11) == 0) {
            g_shadow_host_opts.ld_flags = argv[i] + 11;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr,
                    "error: unknown flag %s (shadow_lower options contract: "
                    "unimplemented ccc flags must not be forwarded silently)\n",
                    argv[i]);
            usage(argv[0]);
            return 2;
        } else if (!in_path) {
            in_path = argv[i];
        } else {
            fprintf(stderr, "error: unexpected arg %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }
    if (!in_path) { usage(argv[0]); return 2; }

    int build_bin = out_path && !shadow_out_is_text(out_path) && !exe_path;
    int write_text = out_path && shadow_out_is_text(out_path);
    int stdout_text = !out_path && !exe_path;

    ShadowEmitKind kind = ends_with(in_path, ".cch") ? SHADOW_EMIT_H : SHADOW_EMIT_C;
    if (kind == SHADOW_EMIT_H && build_bin) {
        fprintf(stderr, "error: -o binary requires a .ccs input (got .cch)\n");
        return 2;
    }

    /* Warm host build: skip parse/emit when emit.c + emit.deps hit. */
    if (build_bin) {
        char cached_c[600];
        if (shadow_emit_cache_hit(in_path, argv[0], no_cache, cached_c,
                                  sizeof(cached_c), NULL)) {
            if (shadow_build_host(in_path, out_path, NULL, 0, argv[0],
                                  no_cache, NULL, 0) != 0) {
                fprintf(stderr, "%s:1:1: error: host build failed\n", in_path);
                return 1;
            }
            return 0;
        }
    }

    char search_dir[512];
    dirname_of(in_path, search_dir, sizeof(search_dir));

    /* Legacy comptime prepare/exec before whitelist parse (fills fragment table).
     * Stage1 consumes the prepare+blank buffer so `@comptime if` pruning and
     * blanked `@comptime` blocks match the product path; diags keep in_path. */
    char* stage1_src = NULL;
    size_t stage1_len = 0;
    if (shadow_comptime_exec_file(in_path, &stage1_src, &stage1_len) != 0) {
        free(stage1_src);
        return 1;
    }

    /* Redis/pigz TUs tokenize many nested .cch headers through this arena. */
    CCArena arena = cc_arena_create(16 << 20);
    TapeCache cache = {0};
    FileTape* ft =
        tape_cache_load_bytes(&cache, in_path, stage1_src, stage1_len, &arena);
    stage1_src = NULL; /* owned by tape (or freed on load failure) */
    if (!ft) {
        fprintf(stderr, "%s:1:1: error: stage1 load failed\n", in_path);
        cc_arena_free(&arena);
        return 1;
    }

    /* Heap + grow: self-emit of shadow_lower.ccs splices the whole serdes
     * tree and exceeds a fixed 64k stage2 buffer. */
    enum { SHADOW_TOKBUF_INIT = 1 << 18 };
    Token* tokbuf = (Token*)malloc(sizeof(Token) * (size_t)SHADOW_TOKBUF_INIT);
    if (!tokbuf) {
        fprintf(stderr, "%s:1:1: error: stage2 token buffer alloc failed\n",
                in_path);
        tape_cache_free(&cache);
        cc_arena_free(&arena);
        return 1;
    }
    Pp pp = {0};
    pp.cache = &cache;
    pp.arena = &arena;
    pp.search_dir = search_dir;
    {
        char sys_cc[256], sys_out[256];
        const char* prefs[4];
        int pi;
        prefs[0] = ".";
        prefs[1] = "..";
        prefs[2] = "../..";
        prefs[3] = "../../..";
        sys_cc[0] = sys_out[0] = 0;
        for (pi = 0; pi < 4; pi++) {
            char probe[256];
            if (sys_cc[0] == 0) {
                snprintf(probe, sizeof(probe), "%s/cc/include", prefs[pi]);
                if (access(probe, R_OK) == 0)
                    snprintf(sys_cc, sizeof(sys_cc), "%s", probe);
            }
            if (sys_out[0] == 0) {
                snprintf(probe, sizeof(probe), "%s/out/include", prefs[pi]);
                if (access(probe, R_OK) == 0)
                    snprintf(sys_out, sizeof(sys_out), "%s", probe);
            }
        }
        pp_add_sys_inc(&pp, sys_cc[0] != 0 ? sys_cc : "cc/include");
        pp_add_sys_inc(&pp, sys_out[0] != 0 ? sys_out : "out/include");
    }
    pp.out = tokbuf;
    pp.out_cap = SHADOW_TOKBUF_INIT;
    pp.out_grow = 1;
    if (!pp_run(&pp, ft)) {
        if (!pp.err_msg[0])
            fprintf(stderr, "%s:1:1: error: stage2 preprocess failed\n",
                    in_path);
        pp_free_macros(&pp);
        free(pp.out);
        tape_cache_free(&cache);
        cc_arena_free(&arena);
        return 1;
    }
    tokbuf = pp.out;

    /* Tables are heap/TLS via parser_ensure_storage — stack Parser is small. */
    Parser p = {0};
    p.toks = tokbuf;
    p.n = pp.nout;
    p.cache = &cache;
    AstNode** items = NULL;
    int n = 0;
    if (!parse_tu(&p, &items, &n)) {
        if (!p.err_msg[0])
            fprintf(stderr, "%s:1:1: error: parse failed\n", in_path);
        pp_free_macros(&pp);
        free(tokbuf);
        tape_cache_free(&cache);
        cc_arena_free(&arena);
        return 1;
    }

    if (!shadow_safety_check(items, n, &cache)) {
        pp_free_macros(&pp);
        free(tokbuf);
        tape_cache_free(&cache);
        cc_arena_free(&arena);
        return 1;
    }

    CEmit emit = {0};
    if (!shadow_emit(items, n, &cache, &emit, kind, pp.pass_inc, pp.npass_inc,
                     pp.pass_def, pp.npass_def, in_path)) {
        fprintf(stderr, "%s:1:1: error: emit failed\n", in_path);
        pp_free_macros(&pp);
        free(emit.buf);
        free(tokbuf);
        tape_cache_free(&cache);
        cc_arena_free(&arena);
        return 1;
    }
    if (shadow_comptime_splice_emit(&emit.buf, &emit.len, in_path) != 0) {
        fprintf(stderr, "%s:1:1: error: comptime splice failed\n", in_path);
        pp_free_macros(&pp);
        free(emit.buf);
        free(tokbuf);
        tape_cache_free(&cache);
        cc_arena_free(&arena);
        return 1;
    }

    int rc = 0;
    if (exe_path) {
#if !defined(SHADOW_HAVE_LIBTCC)
        fprintf(stderr,
                "error: --exe requires a libtcc-linked shadow_lower\n");
        rc = 1;
#else
        char rt_dir[1024];
        const char* incs[2];
        if (!shadow_runtime_libdir(rt_dir, sizeof(rt_dir))) {
            rc = 1;
        } else {
            incs[0] = "out/include";
            incs[1] = "cc/include";
            if (shadow_tcc_compile_exe(emit.buf, exe_path, NULL, incs, 2, rt_dir) != 0) {
                fprintf(stderr, "%s:1:1: error: libtcc compile failed\n", in_path);
                rc = 1;
            }
        }
#endif
    } else if (build_bin) {
        const char* dep_paths[SHADOW_DEP_CAP];
        char* dep_owned[SHADOW_DEP_CAP];
        int ndeps = 0, nowned = 0, di;
        if (shadow_collect_emit_deps(&cache, in_path, dep_paths, &ndeps,
                                     dep_owned, &nowned) != 0) {
            rc = 1;
        } else if (shadow_build_host(in_path, out_path, emit.buf, emit.len,
                                     argv[0], no_cache, dep_paths,
                                     ndeps) != 0) {
            fprintf(stderr, "%s:1:1: error: host build failed\n", in_path);
            rc = 1;
        }
        for (di = 0; di < nowned; di++) free(dep_owned[di]);
    } else if (write_text) {
        FILE* f = fopen(out_path, "wb");
        if (!f) {
            fprintf(stderr, "error: cannot write %s\n", out_path);
            rc = 1;
        } else {
            fwrite(emit.buf, 1, emit.len, f);
            fclose(f);
        }
    } else if (stdout_text) {
        fwrite(emit.buf, 1, emit.len, stdout);
        if (emit.len == 0 || emit.buf[emit.len - 1] != '\n') fputc('\n', stdout);
    }

    free(emit.buf);
    free(tokbuf);
    pp_free_macros(&pp);
    tape_cache_free(&cache);
    cc_arena_free(&arena);
    return rc;
}
#line 41 "examples/serdes/c/pp_stage2.cch"
static const char* const pp_dir__keys[13] = {
    "if",
    "ifdef",
    "ifndef",
    "elif",
    "else",
    "endif",
    "define",
    "undef",
    "include",
    "pragma",
    "line",
    "warning",
    "error",
};
static const size_t pp_dir__key_lens[13] = {
    2,
    5,
    6,
    4,
    4,
    5,
    6,
    5,
    7,
    6,
    4,
    7,
    5,
};
static const PpDirSpec pp_dir__values[13] = {
    /* if */ { 1 },
    /* ifdef */ { 2 },
    /* ifndef */ { 3 },
    /* elif */ { 4 },
    /* else */ { 5 },
    /* endif */ { 6 },
    /* define */ { 7 },
    /* undef */ { 8 },
    /* include */ { 9 },
    /* pragma */ { 10 },
    /* line */ { 11 },
    /* warning */ { 12 },
    /* error */ { 13 },
};
static const int16_t pp_dir__slots[64] = {
    -1,
    9,
    -1,
    -1,
    4,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    11,
    -1,
    -1,
    6,
    3,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    5,
    -1,
    2,
    -1,
    12,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    1,
    -1,
    8,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    0,
    10,
    -1,
    7,
    -1,
    -1,
};
static uint32_t pp_dir__hash(CCSlice key) {
    uint32_t h = 1u;
    size_t i;
    const unsigned char* p = (const unsigned char*)key.ptr;
    for (i = 0; i < key.len; i++) {
        unsigned char c = p[i];
        h ^= c;
        h *= 16777619u;
    }
    return h;
}
static int pp_dir__key_eq(CCSlice key, size_t idx) {
    size_t i;
    const unsigned char* a;
    const unsigned char* b;
    if (key.len != pp_dir__key_lens[idx]) return 0;
    a = (const unsigned char*)key.ptr;
    b = (const unsigned char*)pp_dir__keys[idx];
    for (i = 0; i < key.len; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}
static const PpDirSpec* pp_dir_get(CCSlice key) {
    size_t slot = (size_t)(pp_dir__hash(key) & 63u);
    int16_t idx = pp_dir__slots[slot];
    if (idx < 0) return NULL;
    if (!pp_dir__key_eq(key, (size_t)idx)) return NULL;
    return &pp_dir__values[idx];
}
#line 42 "examples/serdes/c/pp_stage2.cch"
enum{__ccs78920=0};

#line 60 "examples/serdes/c/pp_ast_core.cch"
static const char* const shadow_kw__keys[35] = {
    "int",
    "void",
    "return",
    "typedef",
    "struct",
    "println",
    "errhandler",
    "destroy",
    "spawn",
    "err",
    "if",
    "string",
    "defer",
    "static",
    "char",
    "for",
    "while",
    "with_deadline",
    "as",
    "async",
    "await",
    "create",
    "detach",
    "bool",
    "size_t",
    "inline",
    "const",
    "ordered",
    "break",
    "continue",
    "else",
    "do",
    "eprintln",
    "enum",
    "switch",
};
static const size_t shadow_kw__key_lens[35] = {
    3,
    4,
    6,
    7,
    6,
    7,
    10,
    7,
    5,
    3,
    2,
    6,
    5,
    6,
    4,
    3,
    5,
    13,
    2,
    5,
    5,
    6,
    6,
    4,
    6,
    6,
    5,
    7,
    5,
    8,
    4,
    2,
    8,
    4,
    6,
};
static const ShadowKwSpec shadow_kw__values[35] = {
    /* int */ { 1 },
    /* void */ { 2 },
    /* return */ { 3 },
    /* typedef */ { 4 },
    /* struct */ { 5 },
    /* println */ { 6 },
    /* errhandler */ { 7 },
    /* destroy */ { 8 },
    /* spawn */ { 9 },
    /* err */ { 10 },
    /* if */ { 11 },
    /* string */ { 12 },
    /* defer */ { 13 },
    /* static */ { 14 },
    /* char */ { 15 },
    /* for */ { 16 },
    /* while */ { 17 },
    /* with_deadline */ { 18 },
    /* as */ { 19 },
    /* async */ { 20 },
    /* await */ { 21 },
    /* create */ { 22 },
    /* detach */ { 23 },
    /* bool */ { 24 },
    /* size_t */ { 25 },
    /* inline */ { 26 },
    /* const */ { 27 },
    /* ordered */ { 28 },
    /* break */ { 29 },
    /* continue */ { 30 },
    /* else */ { 31 },
    /* do */ { 32 },
    /* eprintln */ { 33 },
    /* enum */ { 34 },
    /* switch */ { 35 },
};
static const int16_t shadow_kw__slots[256] = {
    -1,
    -1,
    -1,
    11,
    -1,
    -1,
    -1,
    5,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    27,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    9,
    21,
    -1,
    -1,
    -1,
    -1,
    -1,
    7,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    17,
    -1,
    8,
    -1,
    29,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    26,
    -1,
    -1,
    -1,
    -1,
    18,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    0,
    -1,
    -1,
    -1,
    25,
    13,
    15,
    -1,
    33,
    -1,
    -1,
    -1,
    -1,
    -1,
    3,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    16,
    24,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    2,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    14,
    30,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    4,
    -1,
    28,
    -1,
    -1,
    1,
    -1,
    -1,
    -1,
    -1,
    -1,
    12,
    -1,
    20,
    -1,
    -1,
    31,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    23,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    10,
    32,
    -1,
    -1,
    -1,
    34,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    22,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    6,
    -1,
    -1,
    19,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
};
static uint32_t shadow_kw__hash(CCSlice key) {
    uint32_t h = 42u;
    size_t i;
    const unsigned char* p = (const unsigned char*)key.ptr;
    for (i = 0; i < key.len; i++) {
        unsigned char c = p[i];
        h ^= c;
        h *= 16777619u;
    }
    return h;
}
static int shadow_kw__key_eq(CCSlice key, size_t idx) {
    size_t i;
    const unsigned char* a;
    const unsigned char* b;
    if (key.len != shadow_kw__key_lens[idx]) return 0;
    a = (const unsigned char*)key.ptr;
    b = (const unsigned char*)shadow_kw__keys[idx];
    for (i = 0; i < key.len; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}
static const ShadowKwSpec* shadow_kw_get(CCSlice key) {
    size_t slot = (size_t)(shadow_kw__hash(key) & 255u);
    int16_t idx = shadow_kw__slots[slot];
    if (idx < 0) return NULL;
    if (!shadow_kw__key_eq(key, (size_t)idx)) return NULL;
    return &shadow_kw__values[idx];
}
#line 61 "examples/serdes/c/pp_ast_core.cch"
enum{__ccs79963=0};






































