/* Hidden-class (shapes) DOM vs pair-tape DOM — same scanning core, the data
 * structure is the only variable.
 *
 *   parse:  json.h tape DOM  vs  json_shape.h shaped DOM (records reset per
 *           parse; SHAPES persist across parses, as in a server)
 *   access: per tweet, get id / text / user.screen_name —
 *           tape linear (key,value) walk  vs  per-shape hash table
 *
 * Build: gcc -O2 -I ../../../cc/include bench_shape.c \
 *        ../../../cc/runtime/arena_state.c -o bench_shape
 * Run:   ./bench_shape twitter.json [K] [T]
 */
#include "json.h"
#include "json_shape.h"
#include <time.h>

static double now_s(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}

static long jsh_sum(const JShVal* v) {
    long len = (long)JSON_LEN(v->meta);
    switch (JSON_TAG(v->meta)) {
    case JSON_NUM: case JSON_STR: return len + 1;
    case JSON_ARR: { long s = len; const JShArr* a = v->u.arr;
        for (uint64_t i = 0; i < a->n; i++) s += jsh_sum(&a->items[i]); return s; }
    case JSON_OBJ: { long s = len; const JShObj* o = v->u.obj; const JShShape* sh = o->shape;
        if (sh->is_dict) {
            const JShDict* d = (const JShDict*)o->slots;
            for (uint32_t i = 0; i < d->cap; i++)
                if (d->ents[i].key) s += (long)d->ents[i].klen + jsh_sum(&d->ents[i].v);
            return s;
        }
        /* walk shape chain for key lens (deepest slot first) */
        for (const JShShape* e = sh; e->parent; e = e->parent)
            s += (long)e->klen + jsh_sum(&o->slots[e->nslots - 1]);
        return s; }
    default: return JSON_TAG(v->meta) + 1;
    }
}

static long node_sum(const JsonNode* n) {
    long len = (long)JsonNode_len(n);
    switch (JsonNode_tag(n)) {
    case JSON_NUM: case JSON_STR: return len + 1;
    case JSON_ARR: { long s = len; for (JsonNode* c = n->u.head; c; c = c->next) s += node_sum(c); return s; }
    case JSON_OBJ: { long s = len; for (JsonNode* k = n->u.head; k; ) { JsonNode* v = k->next;
        s += (long)JsonNode_len(k) + node_sum(v); k = v->next; } return s; }
    default: return JsonNode_tag(n) + 1;
    }
}

int main(int argc, char** argv) {
    const char* fn = "twitter.json";
    int K = 200, T = 3, pos = 0;
    for (int i = 1; i < argc; i++) {
        if (pos == 0) fn = argv[i]; else if (pos == 1) K = atoi(argv[i]); else if (pos == 2) T = atoi(argv[i]);
        pos++;
    }
    FILE* f = fopen(fn, "rb"); if (!f) { perror(fn); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char* b = malloc(n + 1); size_t rd = fread(b, 1, n, f); b[rd] = 0; fclose(f);

    CCArena tape_ar = cc_arena_create((size_t)n * 24 + (48 << 20));
    CCArena rec_ar  = cc_arena_create((size_t)n * 24 + (48 << 20));
    CCArena shp_ar  = cc_arena_create(16 << 20);

    /* correctness: both DOMs must digest identically (untimed) */
    {
        cc_arena_reset(&tape_ar);
        JsonParser tp = json_parser(b, n, &tape_ar); JsonNode tv;
        if (JsonParser_parse(&tp, &tv) != JSON_OK) { fprintf(stderr, "tape parse failed\n"); return 1; }
        JShParser* sp = jsh_parser(&shp_ar); JShVal sv;
        if (JShParser_parse(sp, b, n, &rec_ar, &sv) != JSON_OK) { fprintf(stderr, "shape parse failed\n"); return 1; }
        long a = node_sum(&tv), c = jsh_sum(&sv);
        if (a != c) { fprintf(stderr, "DIGEST MISMATCH tape=%ld shape=%ld\n", a, c); return 1; }
    }

    /* parse throughput: tape */
    double best = 1e18;
    for (int t = 0; t < T; t++) { double t0 = now_s();
        for (int i = 0; i < K; i++) {
            cc_arena_reset(&tape_ar);
            JsonParser p = json_parser(b, n, &tape_ar); JsonNode v;
            if (JsonParser_parse(&p, &v) != JSON_OK) return 1;
        }
        double dt = now_s() - t0; if (dt < best) best = dt; }
    printf("parse tape    %6.0f MB/s  %7.1f us/parse  node=%zuB/member-pair=48B\n",
           n * (double)K / best / 1e6, best / K * 1e6, sizeof(JsonNode));

    /* parse throughput: shapes (persistent shapes, records reset per parse) */
    JShParser* sp = jsh_parser(&shp_ar);
    best = 1e18;
    long hits = 0, misses = 0; uint32_t nshapes = 0;
    for (int t = 0; t < T; t++) { double t0 = now_s();
        for (int i = 0; i < K; i++) {
            cc_arena_reset(&rec_ar);
            JShVal v;
            if (JShParser_parse(sp, b, n, &rec_ar, &v) != JSON_OK) return 1;
        }
        double dt = now_s() - t0; if (dt < best) best = dt; }
    hits = sp->shape_hits; misses = sp->shape_misses; nshapes = sp->nshapes;
    {
        /* trie sharing: prefix nodes are shared structure; only shapes some
         * object actually finalized as carry a key->slot table */
        uint32_t fin = 0;
        for (uint32_t i = 0; i < JSH_MAX_SHAPES * 2; i++)
            if (sp->ttab[i].to && sp->ttab[i].to->lookup) fin++;
        printf("parse shapes  %6.0f MB/s  %7.1f us/parse  slot=16B  shapes=%u (%u finalized)  trans-hit=%.2f%%\n",
               n * (double)K / best / 1e6, best / K * 1e6, nshapes, fin,
               hits + misses ? 100.0 * hits / (hits + misses) : 100.0);
    }

    /* ---- access: id/text/user.screen_name per tweet ---- */
    cc_arena_reset(&tape_ar);
    JsonParser tp = json_parser(b, n, &tape_ar); JsonNode tv;
    if (JsonParser_parse(&tp, &tv) != JSON_OK) return 1;
    cc_arena_reset(&rec_ar);
    JShVal sv;
    if (JShParser_parse(sp, b, n, &rec_ar, &sv) != JSON_OK) return 1;

    JsonNode* tstat = JsonNode_get(&tv, "statuses");
    JShVal*   sstat = JShVal_get(&sv, "statuses");
    if (!tstat || !sstat) { printf("no statuses — access bench skipped\n"); return 0; }

    const int AK = 20000;
    unsigned long long tsum = 0, ssum = 0;
    long nlk = 0;

    /* keys: id/text sit EARLY in a tweet object (flatters a linear walk);
     * retweet_count sits near the end (~key 20 of ~26) — the arbitrary-key
     * case the hash model is for */
    best = 1e18;
    for (int t = 0; t < T; t++) { double t0 = now_s(); tsum = 0; nlk = 0;
        for (int i = 0; i < AK; i++) {
            for (JsonNode* tw = tstat->u.head; tw; tw = tw->next) {
                JsonNode* id = JsonNode_get(tw, "id");
                JsonNode* tx = JsonNode_get(tw, "text");
                JsonNode* rc = JsonNode_get(tw, "retweet_count");
                JsonNode* us = JsonNode_get(tw, "user");
                JsonNode* sn = us ? JsonNode_get(us, "screen_name") : 0;
                if (id) tsum += JsonNode_len(id);
                if (tx) tsum += JsonNode_len(tx);
                if (rc) tsum += JsonNode_len(rc);
                if (sn) tsum += JsonNode_len(sn);
                nlk += 5;
            }
        }
        double dt = now_s() - t0; if (dt < best) best = dt; }
    printf("access tape   %7.1f Mlookups/s  (linear pair walk, sum=%llu)\n",
           (double)nlk / best / 1e6, tsum);

    best = 1e18;
    for (int t = 0; t < T; t++) { double t0 = now_s(); ssum = 0; nlk = 0;
        for (int i = 0; i < AK; i++) {
            const JShArr* arr = sstat->u.arr;
            for (uint64_t j = 0; j < arr->n; j++) {
                JShVal* tw = &arr->items[j];
                JShVal* id = JShVal_get(tw, "id");
                JShVal* tx = JShVal_get(tw, "text");
                JShVal* rc = JShVal_get(tw, "retweet_count");
                JShVal* us = JShVal_get(tw, "user");
                JShVal* sn = us ? JShVal_get(us, "screen_name") : 0;
                if (id) ssum += JSON_LEN(id->meta);
                if (tx) ssum += JSON_LEN(tx->meta);
                if (rc) ssum += JSON_LEN(rc->meta);
                if (sn) ssum += JSON_LEN(sn->meta);
                nlk += 5;
            }
        }
        double dt = now_s() - t0; if (dt < best) best = dt; }
    printf("access shapes %7.1f Mlookups/s  (per-shape hash, sum=%llu)\n",
           (double)nlk / best / 1e6, ssum);
    if (tsum != ssum) { fprintf(stderr, "ACCESS SUM MISMATCH %llu vs %llu\n", tsum, ssum); return 1; }

    /* prepared keys: hash once, probe many — interned-key / atom analog */
    JShKey kid = jsh_key("id"), ktx = jsh_key("text"), krc = jsh_key("retweet_count"),
           kus = jsh_key("user"), ksn = jsh_key("screen_name");
    {
        unsigned long long psum = 0;
        best = 1e18;
        for (int t = 0; t < T; t++) { double t0 = now_s(); psum = 0; nlk = 0;
            for (int i = 0; i < AK; i++) {
                const JShArr* arr = sstat->u.arr;
                for (uint64_t j = 0; j < arr->n; j++) {
                    JShVal* tw = &arr->items[j];
                    if (JSON_TAG(tw->meta) != JSON_OBJ) continue;
                    JShVal* id = JShObj_get_k(tw->u.obj, kid);
                    JShVal* tx = JShObj_get_k(tw->u.obj, ktx);
                    JShVal* rc = JShObj_get_k(tw->u.obj, krc);
                    JShVal* us = JShObj_get_k(tw->u.obj, kus);
                    JShVal* sn = us && JSON_TAG(us->meta) == JSON_OBJ
                                     ? JShObj_get_k(us->u.obj, ksn) : 0;
                    if (id) psum += JSON_LEN(id->meta);
                    if (tx) psum += JSON_LEN(tx->meta);
                    if (rc) psum += JSON_LEN(rc->meta);
                    if (sn) psum += JSON_LEN(sn->meta);
                    nlk += 5;
                }
            }
            double dt = now_s() - t0; if (dt < best) best = dt; }
        printf("access shapes %7.1f Mlookups/s  (prepared keys, sum=%llu)\n",
               (double)nlk / best / 1e6, psum);
        if (psum != tsum) { fprintf(stderr, "PREPARED SUM MISMATCH\n"); return 1; }
    }

    /* inline cache: shape-pointer check + direct slot index — the V8 IC
     * pattern, and the runtime analog of a comptime schema's fixed offset */
    {
        struct { JShShape* sh; JShVal* (*f)(void); uint32_t idx; int ok; } _;
        (void)_;
        JShShape* c_sh[5] = {0}; uint32_t c_idx[5] = {0};
        unsigned long long csum = 0;
        best = 1e18;
        for (int t = 0; t < T; t++) { double t0 = now_s(); csum = 0; nlk = 0;
            for (int i = 0; i < AK; i++) {
                const JShArr* arr = sstat->u.arr;
                for (uint64_t j = 0; j < arr->n; j++) {
                    JShVal* tw = &arr->items[j];
                    if (JSON_TAG(tw->meta) != JSON_OBJ) continue;
                    JShObj* o = tw->u.obj;
                    JShVal *id, *tx, *rc, *us, *sn = 0;
                    #define IC(slot, obj, K) ({ JShVal* r__; JShObj* o__ = (obj); \
                        if (o__->shape == c_sh[slot]) r__ = &o__->slots[c_idx[slot]]; \
                        else { r__ = JShObj_get_k(o__, K); \
                               if (r__) { c_sh[slot] = o__->shape; c_idx[slot] = (uint32_t)(r__ - o__->slots); } } \
                        r__; })
                    id = IC(0, o, kid);
                    tx = IC(1, o, ktx);
                    rc = IC(2, o, krc);
                    us = IC(3, o, kus);
                    if (us && JSON_TAG(us->meta) == JSON_OBJ) sn = IC(4, us->u.obj, ksn);
                    #undef IC
                    if (id) csum += JSON_LEN(id->meta);
                    if (tx) csum += JSON_LEN(tx->meta);
                    if (rc) csum += JSON_LEN(rc->meta);
                    if (sn) csum += JSON_LEN(sn->meta);
                    nlk += 5; sn = 0;
                }
            }
            double dt = now_s() - t0; if (dt < best) best = dt; }
        printf("access shapes %7.1f Mlookups/s  (inline cache: shape check + slot)\n",
               (double)nlk / best / 1e6);
        if (csum != tsum) { fprintf(stderr, "IC SUM MISMATCH\n"); return 1; }
    }

    /* ---- dictionary fallback: map-shaped data must not explode shapes ----
     * (fresh parser: divergence marks are sticky training state) */
    {
        CCArena m_shp = cc_arena_create(16 << 20);
        CCArena m_rec = cc_arena_create(48 << 20);
        JShParser* mp = jsh_parser(&m_shp);

        /* DEEP: one 3000-key map object (unique keys) -> depth cap trips */
        size_t mcap = 1 << 20, mn = 0;
        char* mj = malloc(mcap);
        mn += (size_t)snprintf(mj + mn, mcap - mn, "{");
        for (int i = 0; i < 3000; i++)
            mn += (size_t)snprintf(mj + mn, mcap - mn, "%s\"user%05d\":{\"a\":%d}", i ? "," : "", i, i);
        mn += (size_t)snprintf(mj + mn, mcap - mn, "}");

        cc_arena_reset(&tape_ar);
        JsonParser mtp = json_parser(mj, mn, &tape_ar); JsonNode mtv;
        if (JsonParser_parse(&mtp, &mtv) != JSON_OK) { fprintf(stderr, "map tape parse failed\n"); return 1; }
        JShVal msv;
        if (JShParser_parse(mp, mj, mn, &m_rec, &msv) != JSON_OK) { fprintf(stderr, "map shape parse failed\n"); return 1; }
        if (node_sum(&mtv) != jsh_sum(&msv)) { fprintf(stderr, "MAP DIGEST MISMATCH\n"); return 1; }
        if (mp->dict_objs < 1) { fprintf(stderr, "map did not dict\n"); return 1; }
        {
            JShVal* u = JShVal_get(&msv, "user01500");
            JShVal* a = u ? JShVal_get(u, "a") : 0;
            if (!a || JSON_TAG(a->meta) != JSON_NUM) { fprintf(stderr, "dict get failed\n"); return 1; }
            if (JShVal_get(&msv, "user09999")) { fprintf(stderr, "dict phantom key\n"); return 1; }
        }
        uint32_t deep_shapes = mp->nshapes;
        long deep_dicts = mp->dict_objs;

        /* WIDE: 500 one-key objects with unique keys -> width cap marks the
         * site diverging; new keys dict, struct-shaped neighbors unaffected */
        mn = 0;
        mn += (size_t)snprintf(mj + mn, mcap - mn, "[");
        for (int i = 0; i < 500; i++)
            mn += (size_t)snprintf(mj + mn, mcap - mn, "%s{\"u%04d\":%d,\"x\":1}", i ? "," : "", i, i);
        mn += (size_t)snprintf(mj + mn, mcap - mn, "]");
        cc_arena_reset(&tape_ar); cc_arena_reset(&m_rec);
        mtp = json_parser(mj, mn, &tape_ar);
        if (JsonParser_parse(&mtp, &mtv) != JSON_OK) { fprintf(stderr, "wide tape parse failed\n"); return 1; }
        if (JShParser_parse(mp, mj, mn, &m_rec, &msv) != JSON_OK) { fprintf(stderr, "wide shape parse failed\n"); return 1; }
        if (node_sum(&mtv) != jsh_sum(&msv)) { fprintf(stderr, "WIDE DIGEST MISMATCH\n"); return 1; }

        printf("dict fallback  deep: shapes=%u dicts=%ld   wide: shapes=%u dicts=%ld  (bounded, digests agree)\n",
               deep_shapes, deep_dicts, mp->nshapes - deep_shapes, mp->dict_objs - deep_dicts);
        free(mj);
        cc_arena_free(&m_shp); cc_arena_free(&m_rec);
    }

    cc_arena_free(&tape_ar); cc_arena_free(&rec_ar); cc_arena_free(&shp_ar);
    free(b);
    return 0;
}
