/*
 * json.h — hand-lowered zero-copy JSON DOM: the C a `@grammar(rules) Json`
 * engine would emit. Whole-buffer (no resume; the @async version gets resumption
 * for free from the state-machine lowering — see the // @await markers).
 *
 * COMPACT NODE (yyjson-inspired, 24 B):
 *   struct JsonNode { uint64_t meta; union{const char* bytes; JsonNode* head}; JsonNode* next; }
 *   meta = tag(3b) | cow(1b) | length_or_count(56b)   -- like yyjson's packed tag word
 *
 * The node does NOT store a full CCSlice. It keeps only what a DOM needs hot:
 * a pointer to the bytes, the length (packed in meta), and the Cow bit
 * (borrowed-from-source vs materialized-in-arena). The full 32 B CCSlice — with
 * provenance id + sub-slice bound — is RECONSTRUCTED on demand by JsonNode_slice():
 * pay-per-use provenance, the same demand-driven idea as lazy numbers. Nothing
 * is lost: the borrow/own distinction lives in the Cow bit; a caller who wants a
 * witnessed slice gets a correct one at the API boundary.
 *
 *   - strings WITHOUT escapes BORROW the source bytes (cow=0); WITH escapes /
 *     \uXXXX MATERIALIZE decoded bytes into the arena (cow=1).
 *   - numbers BORROW their text and parse LAZILY (JsonNode_as_f64 on demand).
 *   - object members are laid down as (key,value) PAIRS in the child list, so a
 *     value/array element carries no key — no 32 B key field per node.
 *
 * Nodes allocate with cc_arena_alloc_local (single-owner request tier); nodes and
 * materialized strings share one arena, reset wholesale between parses.
 *
 * Host-C oracle: include lowered <ccc/*.h> (not raw .cch — those carry @as).
 * Link with out/runtime/arena_state.c (defines cc_arena_prov_counter + FFC_IMPL).
 */
#ifndef CC_JSON_H
#define CC_JSON_H
#include <ccc/cc_arena.h>            /* pulls cc_slice.h */
#include <ccc/vendor/ffc.h>          /* JsonNode_as_f64 — link arena_state.c (FFC_IMPL) */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* provenance allocation classes for reconstructed slices */
#define JSON_SRC_ID    2ULL
#define JSON_ARENA_ID  3ULL

/* char-class table: 1 load + 1 test replaces the branch chains in ws/number/
 * string scanning — a clean static the engine would emit. */
enum { CLS_WS=1, CLS_NUM=2, CLS_STRSPEC=4 };
static const unsigned char json_cls[256] = {
  [' ']=CLS_WS,['\t']=CLS_WS,['\n']=CLS_WS,['\r']=CLS_WS,
  ['0']=CLS_NUM,['1']=CLS_NUM,['2']=CLS_NUM,['3']=CLS_NUM,['4']=CLS_NUM,
  ['5']=CLS_NUM,['6']=CLS_NUM,['7']=CLS_NUM,['8']=CLS_NUM,['9']=CLS_NUM,
  ['-']=CLS_NUM,['+']=CLS_NUM,['.']=CLS_NUM,['e']=CLS_NUM,['E']=CLS_NUM,
  ['"']=CLS_STRSPEC,['\\']=CLS_STRSPEC,
};
#define CLS(c) json_cls[(unsigned char)(c)]

typedef enum { JSON_NULL, JSON_BOOL, JSON_NUM, JSON_STR, JSON_ARR, JSON_OBJ } JsonTag;

/* meta packing */
#define JSON_META(tag,cow,len) ((uint64_t)(tag) | ((uint64_t)((cow)&1u)<<3) | ((uint64_t)(len)<<8))
#define JSON_TAG(m)  ((JsonTag)((m) & 7u))
#define JSON_COW(m)  (((m) >> 3) & 1u)          /* 1 = materialized/owned, 0 = borrowed */
#define JSON_LEN(m)  ((m) >> 8)                 /* string/number byte length, or child count */

typedef struct JsonNode JsonNode;
struct JsonNode {
    uint64_t meta;                              /* tag(3) | cow(1) | len_or_count(56) */
    union { const char* bytes;                  /* STR/NUM: borrowed source or arena bytes */
            JsonNode*   head; } u;              /* ARR/OBJ: first child (members are key,val pairs) */
    JsonNode* next;                             /* sibling link */
};
/* LP64: 8+8+8=24; ILP32: 8+4+4=16 (meta keeps 8-byte align, no hole). */
_Static_assert(sizeof(JsonNode) == sizeof(uint64_t) + 2 * sizeof(void*),
               "compact node is meta + two pointers");

typedef enum { JSON_OK = 1, JSON_BAD = -1 } JsonStatus;

typedef struct {
    const char* src; size_t len, pos;
    CCArena* arena;                             /* nodes + materialized strings */
    JsonNode* node_batch;                       /* carved from arena in slabs */
    size_t node_batch_left;
    long borrowed_strs, materialized_strs;      /* the copy-rate story */
} JsonParser;

/* factory: source data first, arena LAST */
static inline JsonParser json_parser(const char* src, size_t len, CCArena* arena) {
    JsonParser p = {0}; p.src=src; p.len=len; p.arena=arena; return p;
}

/* Slab bump: one arena hit per ~64 nodes (cuts atomic live_allocs on big arrays). */
static inline JsonNode* json__nodes(JsonParser* p, size_t n) {
    if (p->node_batch_left < n) {
        size_t got = n > 64 ? n : 64;
        JsonNode* b = (JsonNode*)cc_arena_alloc_local(p->arena, sizeof(JsonNode) * got,
                                                       _Alignof(JsonNode));
        if (!b) return NULL;
        p->node_batch = b;
        p->node_batch_left = got;
    }
    JsonNode* out = p->node_batch;
    p->node_batch += n;
    p->node_batch_left -= n;
    return out;
}
static inline JsonNode* json__node(JsonParser* p) { return json__nodes(p, 1); }

/* Advance past a number literal (corpus tokens are ~10 B — table scan wins). */
static inline size_t json__scan_num(const char* s, size_t i, size_t n) {
    const unsigned char* u = (const unsigned char*)s;
    while (i < n && (json_cls[u[i]] & CLS_NUM)) i++;
    return i;
}

/* On minified input this is almost always a one-byte reject; cost is call
 * density at structural boundaries, not bytes skipped. */
static inline void json__skip_ws(JsonParser* p) {
    const char* s = p->src; size_t i = p->pos, n = p->len;
    while (i < n && (CLS(s[i]) & CLS_WS)) i++;          // @await here
    p->pos = i;
}

/* Scan to closing '"'. No backslash -> BORROW source bytes (cow=0); any escape ->
 * MATERIALIZE decoded bytes into the arena (cow=1). Returns bytes/len/cow.
 *
 * Note: memchr('"')+memchr('\\') and short scalar probes were tried; both lost
 * ~20–35% on twitter_min vs this SWAR — libc call tax / scalar setup dominate
 * when the median string is ~11 bytes. */
static JsonStatus json__string(JsonParser* p, const char** bytes, uint64_t* len, int* cow) {
    size_t i = p->pos + 1; int has_escape = 0;
    { const uint64_t L=0x0101010101010101ULL,H=0x8080808080808080ULL;   /* SWAR to '"'/'\\' */
      const uint64_t Q=0x2222222222222222ULL,B=0x5C5C5C5C5C5C5C5CULL;
      while (i+8<=p->len){ uint64_t w; memcpy(&w,p->src+i,8);
        uint64_t hq=w^Q; hq=(hq-L)&~hq&H; uint64_t hb=w^B; hb=(hb-L)&~hb&H;
        uint64_t m=hq|hb;
        if(m){ i+=(size_t)(__builtin_ctzll(m)>>3); break; }  /* hit offset, not word restart */
        i+=8; } }
    for(; i<p->len; i++){ if(!(CLS(p->src[i])&CLS_STRSPEC)) continue;
        if(p->src[i]=='"') break;
        has_escape=1; i++; }
    if(i>=p->len) return JSON_BAD;                     /* unterminated */
    size_t raw_start=p->pos+1, raw_len=i-raw_start;
    if(!has_escape){
        *bytes=p->src+raw_start; *len=raw_len; *cow=0;  /* BORROW */
        p->borrowed_strs++;
    } else {
        char* dst=(char*)cc_arena_alloc_local(p->arena, raw_len?raw_len:1, 1);   /* decoded <= raw */
        if(!dst) return JSON_BAD;
        size_t o=0;
        for(size_t k=raw_start;k<i;k++){ char c=p->src[k];
            if(c!='\\'){ dst[o++]=c; continue; } char e=p->src[++k];
            switch(e){ case 'n':dst[o++]='\n';break; case 't':dst[o++]='\t';break;
              case 'r':dst[o++]='\r';break; case '"':dst[o++]='"';break;
              case '\\':dst[o++]='\\';break; case '/':dst[o++]='/';break;
              case 'b':dst[o++]='\b';break; case 'f':dst[o++]='\f';break;
              case 'u':{ unsigned cp=0; for(int h=0;h<4;h++){char x=p->src[++k];cp<<=4;
                  cp|=(x>='0'&&x<='9')?x-'0':(x|32)-'a'+10;}
                  if(cp<0x80)dst[o++]=(char)cp;
                  else if(cp<0x800){dst[o++]=(char)(0xC0|(cp>>6));dst[o++]=(char)(0x80|(cp&0x3F));}
                  else{dst[o++]=(char)(0xE0|(cp>>12));dst[o++]=(char)(0x80|((cp>>6)&0x3F));dst[o++]=(char)(0x80|(cp&0x3F));}
                  break; }
              default:dst[o++]=e;break; } }
        *bytes=dst; *len=o; *cow=1;                     /* MATERIALIZE */
        p->materialized_strs++;
    }
    p->pos=i+1; return JSON_OK;
}

/* Value at p->pos — caller has already skipped ws (avoids double skip_ws on
 * every recursive edge: after ':', after '[', after ','). */
static JsonStatus json__parse_value(JsonParser* p, JsonNode* nd) {
    if(p->pos>=p->len) return JSON_BAD;                 // @await: would suspend here
    char c=p->src[p->pos];
    switch(c){                                          /* first-set dispatch */
    case '"': { const char* b; uint64_t l; int cow;
        if(json__string(p,&b,&l,&cow)!=JSON_OK) return JSON_BAD;
        nd->meta=JSON_META(JSON_STR,cow,l); nd->u.bytes=b; return JSON_OK; }
    case '[': {
        p->pos++; json__skip_ws(p);
        JsonNode* head=NULL; JsonNode** tail=&head; uint64_t cnt=0;
        if(p->pos<p->len && p->src[p->pos]==']'){ p->pos++; nd->meta=JSON_META(JSON_ARR,0,0); nd->u.head=NULL; return JSON_OK; }
        for(;;){
            JsonNode* ch=json__node(p); if(!ch) return JSON_BAD;
            if(json__parse_value(p,ch)!=JSON_OK) return JSON_BAD;
            ch->next=NULL; *tail=ch; tail=&ch->next; cnt++;
            json__skip_ws(p); if(p->pos>=p->len) return JSON_BAD;
            char d=p->src[p->pos++]; if(d==']') break; if(d!=',') return JSON_BAD;
            json__skip_ws(p);                           /* one skip before next elt */
        }
        nd->meta=JSON_META(JSON_ARR,0,cnt); nd->u.head=head; return JSON_OK; }
    case '{': {
        p->pos++; json__skip_ws(p);
        JsonNode* head=NULL; JsonNode** tail=&head; uint64_t cnt=0;
        if(p->pos<p->len && p->src[p->pos]=='}'){ p->pos++; nd->meta=JSON_META(JSON_OBJ,0,0); nd->u.head=NULL; return JSON_OK; }
        for(;;){
            /* key+value in one bump; list is kn -> vn -> ... */
            JsonNode* pair=json__nodes(p,2); if(!pair) return JSON_BAD;
            JsonNode* kn=pair; JsonNode* vn=pair+1;
            json__skip_ws(p); if(p->pos>=p->len||p->src[p->pos]!='"') return JSON_BAD;
            { const char* b; uint64_t l; int cow;
              if(json__string(p,&b,&l,&cow)!=JSON_OK) return JSON_BAD;
              kn->meta=JSON_META(JSON_STR,cow,l); kn->u.bytes=b; }
            json__skip_ws(p); if(p->pos>=p->len||p->src[p->pos++]!=':') return JSON_BAD;
            json__skip_ws(p);
            if(json__parse_value(p,vn)!=JSON_OK) return JSON_BAD;
            kn->next=vn; vn->next=NULL; *tail=kn; tail=&vn->next; cnt++;
            json__skip_ws(p); if(p->pos>=p->len) return JSON_BAD;
            char d=p->src[p->pos++]; if(d=='}') break; if(d!=',') return JSON_BAD;
        }
        nd->meta=JSON_META(JSON_OBJ,0,cnt); nd->u.head=head; return JSON_OK; }
    case 't': {
        if(p->pos+4<=p->len){ uint32_t w; memcpy(&w,p->src+p->pos,4);
          if(w==0x65757274u){ nd->meta=JSON_META(JSON_BOOL,0,1); p->pos+=4; return JSON_OK; } }
        return JSON_BAD; }
    case 'f': {
        if(p->pos+5<=p->len){ uint32_t w; memcpy(&w,p->src+p->pos,4);
          if(w==0x736C6166u && p->src[p->pos+4]=='e'){ nd->meta=JSON_META(JSON_BOOL,0,0); p->pos+=5; return JSON_OK; } }
        return JSON_BAD; }
    case 'n': {
        if(p->pos+4<=p->len){ uint32_t w; memcpy(&w,p->src+p->pos,4);
          if(w==0x6C6C756Eu){ nd->meta=JSON_META(JSON_NULL,0,0); p->pos+=4; return JSON_OK; } }
        return JSON_BAD; }
    default: {                                        /* NUM: borrow text */
        size_t s=p->pos, e=json__scan_num(p->src, s, p->len);
        if(e==s) return JSON_BAD;
        nd->meta=JSON_META(JSON_NUM,0,e-s); nd->u.bytes=p->src+s; p->pos=e; return JSON_OK; }
    }
}

/* Public entry: skip once, then parse. Nested values use json__parse_value. */
static inline JsonStatus JsonParser_parse(JsonParser* p, JsonNode* nd) {
    p->node_batch = NULL; p->node_batch_left = 0;       /* invalidate across arena reset */
    json__skip_ws(p);
    return json__parse_value(p, nd);
}


/* ---- accessors (receiver first) ---- */
static inline JsonTag  JsonNode_tag(const JsonNode* n){ return JSON_TAG(n->meta); }
static inline uint64_t JsonNode_len(const JsonNode* n){ return JSON_LEN(n->meta); }   /* str/num bytes or child count */
static inline bool     JsonNode_is_borrowed(const JsonNode* n){ return !JSON_COW(n->meta); }

/* Reconstruct the full witnessed CCSlice on demand — pay-per-use provenance. */
static inline CCSlice JsonNode_slice(const JsonNode* n){
    uint64_t l = JSON_LEN(n->meta);
    uint64_t id = JSON_COW(n->meta) ? cc_slice_make_id(JSON_ARENA_ID,true,false,false)
                                    : cc_slice_make_id(JSON_SRC_ID,false,false,true);
    return cc_slice_from_parts((void*)n->u.bytes, l, id, l);
}

static inline double JsonNode_as_f64(const JsonNode* n){
    if (JSON_TAG(n->meta) != JSON_NUM) return 0;
    uint64_t l = JSON_LEN(n->meta);
    double v = 0;
    ffc_parse_options o; o.format = FFC_PRESET_JSON; o.decimal_point = '.';
    ffc_result r = ffc_from_chars_double_options(n->u.bytes, n->u.bytes + (size_t)l, &v, o);
    return r.outcome == FFC_OUTCOME_OK ? v : 0;
}

/* object lookup: child list is key0->val0->key1->val1->... (every key has a value) */
static inline JsonNode* JsonNode_get(JsonNode* v, const char* key){
    if(JSON_TAG(v->meta)!=JSON_OBJ) return NULL;
    size_t kl=strlen(key);
    for(JsonNode* k=v->u.head; k; ){
        JsonNode* val=k->next;
        if(JSON_LEN(k->meta)==kl && !memcmp(k->u.bytes,key,kl)) return val;
        k=val->next;
    }
    return NULL;
}
#endif /* CC_JSON_H */
