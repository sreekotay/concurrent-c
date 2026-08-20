/* String-literal case labels on switch (CCSlice subject).
 * case "GET": … → MPH lookup → switch (ordinal). Opaque body rewrite;
 * no new keyword. Requires pp_emit_core.cch. */
#pragma once

enum { SHADOW_STRSW_MAX_KEYS = 256, SHADOW_STRSW_KEY_CAP = 128 };

static int g_shadow_strsw_seq;

/* 1 = CCSlice-shaped, -1 = known non-slice, 0 = unknown. */
static int shadow_strsw_ty_kind(const char* ty) {
    const char* t;
    size_t n;
    if (!ty || !ty[0]) return 0;
    t = ty;
    for (;;) {
        while (*t == ' ' || *t == '\t') t++;
        if (strncmp(t, "const", 5) == 0 && (t[5] == ' ' || t[5] == '\t')) {
            t += 5;
            continue;
        }
        if (strncmp(t, "volatile", 8) == 0 &&
            (t[8] == ' ' || t[8] == '\t')) {
            t += 8;
            continue;
        }
        break;
    }
    n = strlen(t);
    while (n && (t[n - 1] == ' ' || t[n - 1] == '\t')) n--;
    if (n == 7 && strncmp(t, "CCSlice", 7) == 0) return 1;
    if (n == 13 && strncmp(t, "CCSliceUnique", 13) == 0) return 1;
    if (n == 13 && strncmp(t, "CCSliceShared", 13) == 0) return 1;
    if (n == 13 && strncmp(t, "CCSlicePacked", 13) == 0) return 1;
    if (n == 10 && strncmp(t, "charslice", 10) == 0) return 1;
    /* Pointer-to-slice is not a switch subject we lower. */
    if (n && t[n - 1] == '*') return -1;
    if (n == 3 && strncmp(t, "int", 3) == 0) return -1;
    if (n == 4 && strncmp(t, "char", 4) == 0) return -1;
    if (n == 4 && strncmp(t, "long", 4) == 0) return -1;
    if (n == 5 && strncmp(t, "short", 5) == 0) return -1;
    if (n == 4 && strncmp(t, "bool", 4) == 0) return -1;
    if (n == 5 && strncmp(t, "_Bool", 5) == 0) return -1;
    if (n == 6 && strncmp(t, "size_t", 6) == 0) return -1;
    return 0;
}

/* Leading identifier in a switch subject (skip outer parens / space). */
static int shadow_strsw_subj_ident(const char* expr, char* out, size_t cap) {
    const char* s;
    size_t n = 0;
    if (!expr || !out || cap < 2) return 0;
    s = expr;
    while (*s == '(' || *s == ' ' || *s == '\t') s++;
    while (s[n] && shadow_is_id(s[n]) && n + 1 < cap) n++;
    if (!n) {
        out[0] = 0;
        return 0;
    }
    memcpy(out, s, n);
    out[n] = 0;
    return 1;
}

/* Skip whitespace and line/block comments; return new index. */
static size_t shadow_strsw_skip_ws_comments(const char* s, size_t i) {
    for (;;) {
        while (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')
            i++;
        if (s[i] == '/' && s[i + 1] == '/') {
            i += 2;
            while (s[i] && s[i] != '\n') i++;
            continue;
        }
        if (s[i] == '/' && s[i + 1] == '*') {
            i += 2;
            while (s[i] && !(s[i] == '*' && s[i + 1] == '/')) i++;
            if (s[i] == '*' && s[i + 1] == '/') i += 2;
            continue;
        }
        break;
    }
    return i;
}

/* Word-boundary `case` / `default` at s[i]. */
static int shadow_strsw_at_kw(const char* s, size_t i, const char* kw) {
    size_t n = strlen(kw);
    if (strncmp(s + i, kw, n) != 0) return 0;
    if (i > 0 && shadow_is_id(s[i - 1])) return 0;
    if (s[i + n] && shadow_is_id(s[i + n])) return 0;
    return 1;
}

/* Collect string case keys from opaque switch body text.
 * Sets *has_nonstring if any non-string `case` (not default) appears.
 * Returns 0 on hard error (fills err). */
static int shadow_strsw_collect(const char* body, char keys[][SHADOW_STRSW_KEY_CAP],
                                size_t* key_lens, int* nkeys, int* has_nonstring,
                                char* err, size_t errcap) {
    size_t i = 0;
    int in_dq = 0, in_sq = 0, in_bt = 0;
    if (!body || !keys || !key_lens || !nkeys || !has_nonstring) return 0;
    *nkeys = 0;
    *has_nonstring = 0;
    if (err && errcap) err[0] = 0;
    while (body[i]) {
        char c = body[i];
        if (in_bt) {
            if (c == '\\' && body[i + 1]) {
                i += 2;
                continue;
            }
            if (c == '`') in_bt = 0;
            i++;
            continue;
        }
        if (in_dq) {
            if (c == '\\' && body[i + 1]) {
                i += 2;
                continue;
            }
            if (c == '"') in_dq = 0;
            i++;
            continue;
        }
        if (in_sq) {
            if (c == '\\' && body[i + 1]) {
                i += 2;
                continue;
            }
            if (c == '\'') in_sq = 0;
            i++;
            continue;
        }
        if (c == '/' && body[i + 1] == '/') {
            i += 2;
            while (body[i] && body[i] != '\n') i++;
            continue;
        }
        if (c == '/' && body[i + 1] == '*') {
            i += 2;
            while (body[i] && !(body[i] == '*' && body[i + 1] == '/')) i++;
            if (body[i] == '*' && body[i + 1] == '/') i += 2;
            continue;
        }
        if (c == '`') {
            in_bt = 1;
            i++;
            continue;
        }
        if (c == '"') {
            in_dq = 1;
            i++;
            continue;
        }
        if (c == '\'') {
            in_sq = 1;
            i++;
            continue;
        }
        if (shadow_strsw_at_kw(body, i, "default")) {
            i += 7;
            continue;
        }
        if (shadow_strsw_at_kw(body, i, "case")) {
            size_t j = shadow_strsw_skip_ws_comments(body, i + 4);
            if (body[j] == '"') {
                size_t k0 = j + 1;
                size_t k = k0;
                char key[SHADOW_STRSW_KEY_CAP];
                size_t kn = 0;
                int dup;
                int di;
                while (body[k] && body[k] != '"') {
                    if (body[k] == '\\') {
                        if (err && errcap)
                            snprintf(err, errcap,
                                     "string switch: case labels must be "
                                     "simple string literals (no escapes)");
                        return 0;
                    }
                    if (kn + 1 >= sizeof(key)) {
                        if (err && errcap)
                            snprintf(err, errcap,
                                     "string switch: case label too long");
                        return 0;
                    }
                    key[kn++] = body[k++];
                }
                if (body[k] != '"') {
                    if (err && errcap)
                        snprintf(err, errcap,
                                 "string switch: unterminated case label");
                    return 0;
                }
                key[kn] = 0;
                j = shadow_strsw_skip_ws_comments(body, k + 1);
                if (body[j] != ':') {
                    if (err && errcap)
                        snprintf(err, errcap,
                                 "string switch: expected ':' after case "
                                 "label");
                    return 0;
                }
                dup = 0;
                for (di = 0; di < *nkeys; di++) {
                    if (key_lens[di] == kn &&
                        memcmp(keys[di], key, kn) == 0) {
                        dup = 1;
                        break;
                    }
                }
                if (dup) {
                    if (err && errcap)
                        snprintf(err, errcap,
                                 "string switch: duplicate case \"%s\"", key);
                    return 0;
                }
                if (*nkeys >= SHADOW_STRSW_MAX_KEYS) {
                    if (err && errcap)
                        snprintf(err, errcap,
                                 "string switch: too many case labels");
                    return 0;
                }
                memcpy(keys[*nkeys], key, kn + 1);
                key_lens[*nkeys] = kn;
                (*nkeys)++;
                i = j + 1;
                continue;
            }
            /* Non-string case label. */
            *has_nonstring = 1;
            i = j;
            continue;
        }
        i++;
    }
    return 1;
}

/* FNV-1a case-sensitive perfect hash (same shape as static_map). */
static int shadow_strsw_mph(const char* const* keys, const size_t* key_lens,
                            int nkeys, size_t* out_table_size, uint32_t* out_seed,
                            int16_t* slots, size_t slots_cap, char* err,
                            size_t errcap) {
    size_t table_size = 0;
    uint32_t seed = 0;
    int found = 0;
    int i, j;
    if (!keys || !key_lens || nkeys <= 0 || !out_table_size || !out_seed ||
        !slots)
        return 0;
    for (table_size = 1; table_size < (size_t)nkeys; table_size <<= 1) {
    }
    if (table_size < 4) table_size = 4;
    while (table_size <= slots_cap && !found) {
        for (seed = 1; seed < 100000u; seed++) {
            int ok = 1;
            for (i = 0; i < (int)table_size; i++) slots[i] = -1;
            for (i = 0; i < nkeys; i++) {
                uint32_t h = seed;
                size_t slot;
                for (j = 0; j < (int)key_lens[i]; j++) {
                    unsigned char c = (unsigned char)keys[i][j];
                    h ^= c;
                    h *= 16777619u;
                }
                slot = (size_t)(h & (uint32_t)(table_size - 1));
                if (slots[slot] >= 0) {
                    ok = 0;
                    break;
                }
                slots[slot] = (int16_t)i;
            }
            if (ok) {
                found = 1;
                break;
            }
        }
        if (!found) table_size <<= 1;
    }
    if (!found) {
        if (err && errcap)
            snprintf(err, errcap, "string switch: failed to build perfect hash");
        return 0;
    }
    *out_table_size = table_size;
    *out_seed = seed;
    return 1;
}

/* Rewrite `case "KEY":` → `case N:` in place. */
static int shadow_strsw_rewrite_body(char* body, size_t cap, char keys[][SHADOW_STRSW_KEY_CAP],
                                     const size_t* key_lens, int nkeys) {
    char tmp[8192];
    size_t i = 0;
    size_t o = 0;
    int in_dq = 0, in_sq = 0, in_bt = 0;
    if (!body) return 0;
    /* Empty string-switch (default-only / grammar Type__fk with no fields):
     * nothing to rewrite. */
    if (nkeys == 0) return 1;
    if (!keys || !key_lens) return 0;
    tmp[0] = 0;
    while (body[i] && o + 1 < sizeof(tmp)) {
        char c = body[i];
        if (in_bt) {
            tmp[o++] = c;
            if (c == '\\' && body[i + 1] && o + 1 < sizeof(tmp)) {
                tmp[o++] = body[i + 1];
                i += 2;
                continue;
            }
            if (c == '`') in_bt = 0;
            i++;
            continue;
        }
        if (in_dq) {
            tmp[o++] = c;
            if (c == '\\' && body[i + 1] && o + 1 < sizeof(tmp)) {
                tmp[o++] = body[i + 1];
                i += 2;
                continue;
            }
            if (c == '"') in_dq = 0;
            i++;
            continue;
        }
        if (in_sq) {
            tmp[o++] = c;
            if (c == '\\' && body[i + 1] && o + 1 < sizeof(tmp)) {
                tmp[o++] = body[i + 1];
                i += 2;
                continue;
            }
            if (c == '\'') in_sq = 0;
            i++;
            continue;
        }
        if (c == '/' && body[i + 1] == '/') {
            tmp[o++] = body[i++];
            tmp[o++] = body[i++];
            while (body[i] && body[i] != '\n' && o + 1 < sizeof(tmp))
                tmp[o++] = body[i++];
            continue;
        }
        if (c == '/' && body[i + 1] == '*') {
            tmp[o++] = body[i++];
            tmp[o++] = body[i++];
            while (body[i] && !(body[i] == '*' && body[i + 1] == '/') &&
                   o + 1 < sizeof(tmp))
                tmp[o++] = body[i++];
            if (body[i] == '*' && body[i + 1] == '/' && o + 2 < sizeof(tmp)) {
                tmp[o++] = body[i++];
                tmp[o++] = body[i++];
            }
            continue;
        }
        if (c == '`') {
            in_bt = 1;
            tmp[o++] = body[i++];
            continue;
        }
        if (c == '"') {
            in_dq = 1;
            tmp[o++] = body[i++];
            continue;
        }
        if (c == '\'') {
            in_sq = 1;
            tmp[o++] = body[i++];
            continue;
        }
        if (shadow_strsw_at_kw(body, i, "case")) {
            size_t j = shadow_strsw_skip_ws_comments(body, i + 4);
            if (body[j] == '"') {
                size_t k = j + 1;
                char key[SHADOW_STRSW_KEY_CAP];
                size_t kn = 0;
                int idx = -1;
                int di;
                int n;
                while (body[k] && body[k] != '"' && kn + 1 < sizeof(key))
                    key[kn++] = body[k++];
                key[kn] = 0;
                if (body[k] != '"') return 0;
                for (di = 0; di < nkeys; di++) {
                    if (key_lens[di] == kn && memcmp(keys[di], key, kn) == 0) {
                        idx = di;
                        break;
                    }
                }
                if (idx < 0) return 0;
                /* Copy `case` + spaces/comments up to the opening quote. */
                while (i < j && o + 1 < sizeof(tmp)) tmp[o++] = body[i++];
                n = snprintf(tmp + o, sizeof(tmp) - o, "%d", idx);
                if (n < 0 || (size_t)n >= sizeof(tmp) - o) return 0;
                o += (size_t)n;
                i = k + 1; /* skip closing quote; leave ':' and rest */
                continue;
            }
        }
        tmp[o++] = body[i++];
    }
    tmp[o] = 0;
    if (o + 1 > cap) return 0;
    memcpy(body, tmp, o + 1);
    return 1;
}

/* Emit MPH tables + discriminant temp, then `switch (temp) {`. */
static int shadow_strsw_emit_head(CEmit* out, ShadowCtx* ctx, AstNode* st,
                                  const char* indent, const char* expr,
                                  char keys[][SHADOW_STRSW_KEY_CAP],
                                  const size_t* key_lens, int nkeys,
                                  size_t table_size, uint32_t seed,
                                  const int16_t* slots) {
    int id;
    int i;
    char pref[32];
    char num[32];
    int use_linear = 0;
    size_t max_len = 0;
    (void)ctx;
    (void)st;
    id = ++g_shadow_strsw_seq;
    snprintf(pref, sizeof(pref), "__cc_ss%d", id);
    for (i = 0; i < nkeys; i++)
        if (key_lens[i] > max_len) max_len = key_lens[i];
    /* Tiny tables: length+memcmp beats FNV MPH (JSON schema keys, short
     * command names). Larger sets keep the perfect-hash probe. */
    if (nkeys <= 8 && max_len <= 16) use_linear = 1;
    if (!cemit_fmt(out, "%sint %s;\n", indent, pref)) return 0;
    if (!cemit_fmt(out, "%s{\n", indent)) return 0;
    if (!cemit_fmt(out, "%s  CCSlice %s_k = (%s);\n", indent, pref, expr))
        return 0;
    if (!cemit_fmt(out, "%s  int %s_idx = -1;\n", indent, pref)) return 0;
    if (use_linear) {
        (void)table_size;
        (void)seed;
        (void)slots;
        if (!cemit_fmt(out, "%s  const unsigned char* %s_p =\n", indent, pref))
            return 0;
        if (!cemit_fmt(out, "%s      (const unsigned char*)%s_k.ptr;\n", indent,
                       pref))
            return 0;
        for (i = 0; i < nkeys; i++) {
            const char* sep = (i == 0) ? "if" : "else if";
            snprintf(num, sizeof(num), "%u", (unsigned)key_lens[i]);
            if (!cemit_fmt(out,
                           "%s  %s (%s_k.len == %s && memcmp(%s_p, \"%s\", %s) "
                           "== 0) %s_idx = %d;\n",
                           indent, sep, pref, num, pref, keys[i], num, pref, i))
                return 0;
        }
    } else {
        if (!cemit_fmt(out, "%s  static const char* const %s_keys[%d] = {\n",
                       indent, pref, nkeys))
            return 0;
        for (i = 0; i < nkeys; i++) {
            if (!cemit_fmt(out, "%s    \"%s\",\n", indent, keys[i])) return 0;
        }
        if (!cemit_fmt(out, "%s  };\n", indent)) return 0;
        if (!cemit_fmt(out, "%s  static const size_t %s_lens[%d] = {\n", indent,
                       pref, nkeys))
            return 0;
        for (i = 0; i < nkeys; i++) {
            snprintf(num, sizeof(num), "%u", (unsigned)key_lens[i]);
            if (!cemit_fmt(out, "%s    %s,\n", indent, num)) return 0;
        }
        if (!cemit_fmt(out, "%s  };\n", indent)) return 0;
        snprintf(num, sizeof(num), "%u", (unsigned)table_size);
        if (!cemit_fmt(out, "%s  static const int16_t %s_slots[%s] = {\n", indent,
                       pref, num))
            return 0;
        for (i = 0; i < (int)table_size; i++) {
            if (!cemit_fmt(out, "%s    %d,\n", indent, (int)slots[i])) return 0;
        }
        if (!cemit_fmt(out, "%s  };\n", indent)) return 0;
        snprintf(num, sizeof(num), "%u", (unsigned)seed);
        if (!cemit_fmt(out, "%s  uint32_t %s_h = %su;\n", indent, pref, num))
            return 0;
        if (!cemit_fmt(out, "%s  size_t %s_i;\n", indent, pref)) return 0;
        if (!cemit_fmt(out, "%s  const unsigned char* %s_p =\n", indent, pref))
            return 0;
        if (!cemit_fmt(out, "%s      (const unsigned char*)%s_k.ptr;\n", indent,
                       pref))
            return 0;
        if (!cemit_fmt(out, "%s  for (%s_i = 0; %s_i < %s_k.len; %s_i++) {\n",
                       indent, pref, pref, pref, pref))
            return 0;
        if (!cemit_fmt(out, "%s    unsigned char %s_c = %s_p[%s_i];\n", indent,
                       pref, pref, pref))
            return 0;
        if (!cemit_fmt(out, "%s    %s_h ^= %s_c;\n", indent, pref, pref))
            return 0;
        if (!cemit_fmt(out, "%s    %s_h *= 16777619u;\n", indent, pref))
            return 0;
        if (!cemit_fmt(out, "%s  }\n", indent)) return 0;
        if (!cemit_fmt(out, "%s  {\n", indent)) return 0;
        snprintf(num, sizeof(num), "%u", (unsigned)(table_size - 1));
        if (!cemit_fmt(out, "%s    size_t %s_slot = (size_t)(%s_h & %su);\n",
                       indent, pref, pref, num))
            return 0;
        if (!cemit_fmt(out, "%s    int16_t %s_si = %s_slots[%s_slot];\n", indent,
                       pref, pref, pref))
            return 0;
        if (!cemit_fmt(out,
                       "%s    if (%s_si >= 0 && %s_k.len == %s_lens[%s_si]) {\n",
                       indent, pref, pref, pref, pref))
            return 0;
        if (!cemit_fmt(out, "%s      const unsigned char* %s_b =\n", indent,
                       pref))
            return 0;
        if (!cemit_fmt(out,
                       "%s          (const unsigned char*)%s_keys[%s_si];\n",
                       indent, pref, pref))
            return 0;
        if (!cemit_fmt(out, "%s      size_t %s_j;\n", indent, pref)) return 0;
        if (!cemit_fmt(out, "%s      int %s_ok = 1;\n", indent, pref)) return 0;
        if (!cemit_fmt(out, "%s      for (%s_j = 0; %s_j < %s_k.len; %s_j++)\n",
                       indent, pref, pref, pref, pref))
            return 0;
        if (!cemit_fmt(out,
                       "%s        if (%s_p[%s_j] != %s_b[%s_j]) { %s_ok = 0; "
                       "break; }\n",
                       indent, pref, pref, pref, pref, pref))
            return 0;
        if (!cemit_fmt(out, "%s      if (%s_ok) %s_idx = (int)%s_si;\n", indent,
                       pref, pref, pref))
            return 0;
        if (!cemit_fmt(out, "%s    }\n", indent)) return 0;
        if (!cemit_fmt(out, "%s  }\n", indent)) return 0;
    }
    if (!cemit_fmt(out, "%s  %s = %s_idx;\n", indent, pref, pref)) return 0;
    if (!cemit_fmt(out, "%s}\n", indent)) return 0;
    return cemit_fmt(out, "%sswitch (%s) {\n", indent, pref);
}

/* Try string-switch lower. Returns:
 *   1 — lowered (caller must emit body + closing brace, or already done)
 *   0 — not a string switch (caller uses ordinary path)
 *  -1 — hard error already diagnosed */
static int shadow_strsw_try(AstNode* st, CEmit* out, ShadowCtx* ctx,
                            const char* indent, char* body, size_t body_cap,
                            char* expr, size_t expr_cap) {
    char keys[SHADOW_STRSW_MAX_KEYS][SHADOW_STRSW_KEY_CAP];
    size_t key_lens[SHADOW_STRSW_MAX_KEYS];
    const char* key_ptrs[SHADOW_STRSW_MAX_KEYS];
    int16_t slots[512];
    int nkeys = 0;
    int has_nonstring = 0;
    char err[320];
    size_t table_size = 0;
    uint32_t seed = 0;
    int i;
    char subj[128];
    int tyk;
    (void)expr_cap;
    if (!st || !out || !body || !expr) return 0;
    if (!shadow_strsw_collect(body, keys, key_lens, &nkeys, &has_nonstring, err,
                             sizeof(err))) {
        shadow_variant_err_loc(ctx, st, out, "switch", 0,
                               err[0] ? err : "string switch: invalid case");
        return -1;
    }
    /* Subject type: known non-slice is a hard error when string cases exist;
     * known slice with zero string cases (default-only / empty grammar __fk)
     * must still lower — host C cannot `switch (CCSlice)`. */
    tyk = 0;
    if (shadow_strsw_subj_ident(expr, subj, sizeof(subj))) {
        const ShadowBind* b = shadow_bind_lookup(subj);
        tyk = b ? shadow_strsw_ty_kind(b->ty) : 0;
        if (tyk < 0 && nkeys > 0) {
            snprintf(err, sizeof(err),
                     "string switch: subject '%s' has type '%s' (need "
                     "CCSlice)",
                     subj, b && b->ty[0] ? b->ty : "?");
            shadow_variant_err_loc(ctx, st, out, subj, 0, err);
            return -1;
        }
    }
    if (nkeys == 0) {
        /* Ordinary int/enum switch (possibly default-only): leave alone. */
        if (has_nonstring || tyk <= 0) return 0;
        if (!shadow_strsw_rewrite_body(body, body_cap, keys, key_lens, 0)) {
            shadow_variant_err_loc(ctx, st, out, "switch", 0,
                                   "string switch: failed to rewrite case labels");
            return -1;
        }
        if (!shadow_strsw_emit_head(out, ctx, st, indent, expr, keys, key_lens, 0,
                                   0, 0, NULL))
            return -1;
        return 1;
    }
    if (has_nonstring) {
        shadow_variant_err_loc(
            ctx, st, out, "switch", 0,
            "string switch: cannot mix string and non-string case labels");
        return -1;
    }
    for (i = 0; i < nkeys; i++) key_ptrs[i] = keys[i];
    if (!shadow_strsw_mph(key_ptrs, key_lens, nkeys, &table_size, &seed, slots,
                          sizeof(slots) / sizeof(slots[0]), err, sizeof(err))) {
        shadow_variant_err_loc(ctx, st, out, "switch", 0,
                               err[0] ? err : "string switch: mph failed");
        return -1;
    }
    if (!shadow_strsw_rewrite_body(body, body_cap, keys, key_lens, nkeys)) {
        shadow_variant_err_loc(ctx, st, out, "switch", 0,
                               "string switch: failed to rewrite case labels");
        return -1;
    }
    if (!shadow_strsw_emit_head(out, ctx, st, indent, expr, keys, key_lens, nkeys,
                               table_size, seed, slots))
        return -1;
    return 1;
}
