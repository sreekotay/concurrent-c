#include "unit_header.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CCC_VERSION_BASE
#define CCC_VERSION_BASE "0.3.4"
#endif
#ifndef CCC_BOOTSTRAP_SEED
#define CCC_BOOTSTRAP_SEED 0
#endif

const char* cc_unit_kind_name(CCUnitKind k) {
    switch (k) {
    case CC_UNIT_KIND_CCS: return "ccs";
    case CC_UNIT_KIND_CCH: return "cch";
    case CC_UNIT_KIND_SHCC: return "shcc";
    default: return "unknown";
    }
}

CCUnitKind cc_unit_kind_from_name(const char* s) {
    if (!s) return CC_UNIT_KIND_UNKNOWN;
    if (strcmp(s, "ccs") == 0) return CC_UNIT_KIND_CCS;
    if (strcmp(s, "cch") == 0) return CC_UNIT_KIND_CCH;
    if (strcmp(s, "shcc") == 0) return CC_UNIT_KIND_SHCC;
    return CC_UNIT_KIND_UNKNOWN;
}

static int cc__ends_with(const char* s, const char* suf) {
    size_t n, m;
    if (!s || !suf) return 0;
    n = strlen(s);
    m = strlen(suf);
    return n >= m && strcmp(s + n - m, suf) == 0;
}

CCUnitKind cc_unit_kind_from_ext(const char* path) {
    if (!path) return CC_UNIT_KIND_UNKNOWN;
    if (cc__ends_with(path, ".shcc")) return CC_UNIT_KIND_SHCC;
    if (cc__ends_with(path, ".ccs")) return CC_UNIT_KIND_CCS;
    if (cc__ends_with(path, ".cch")) return CC_UNIT_KIND_CCH;
    return CC_UNIT_KIND_UNKNOWN;
}

static const char* cc__basename(const char* tok) {
    const char* slash;
    if (!tok || !tok[0]) return tok;
    slash = strrchr(tok, '/');
    return slash ? slash + 1 : tok;
}

static int cc__is_ccc_interp(const char* tok) {
    const char* b = cc__basename(tok);
    return b && (strcmp(b, "ccc") == 0 || strcmp(b, ".ccc-bin") == 0);
}

static const char cc__ver_pin_form[] =
    "MAJOR.MINOR[.PATCH[-SEED]] (usual: MAJOR.MINOR), a bound "
    "(>=X, >X, <=X, <X), or both (e.g. >=0.3,<0.4)";

typedef struct {
    int bare;
    int has_lo;
    int has_hi;
    int lo_incl;
    int hi_incl;
    int lo[4];
    int hi[4];
} CCCccVersionSpec;

static void cc__skip_ws(const char** pp) {
    const char* p;
    if (!pp || !*pp) return;
    p = *pp;
    while (*p == ' ' || *p == '\t') p++;
    *pp = p;
}

static int cc__parse_ver_num(const char** pp, int* out) {
    const char* p;
    char* end;
    long v;
    if (!pp || !*pp || !out) return -1;
    p = *pp;
    if (!isdigit((unsigned char)*p)) return -1;
    v = strtol(p, &end, 10);
    if (end == p || v < 0 || v > 1000000000L) return -1;
    *out = (int)v;
    *pp = end;
    return 0;
}

static int cc__parse_ver_fields(const char* s, const char** end, int* major,
                               int* minor, int* patch, int* seed) {
    int ma = -1, mi = -1, pa = -1, se = -1;
    const char* p;
    if (!s || !s[0]) return -1;
    p = s;
    if (cc__parse_ver_num(&p, &ma) != 0) return -1;
    if (*p == '.') {
        p++;
        if (cc__parse_ver_num(&p, &mi) != 0) return -1;
        if (*p == '.') {
            p++;
            if (cc__parse_ver_num(&p, &pa) != 0) return -1;
            if (*p == '-') {
                p++;
                if (cc__parse_ver_num(&p, &se) != 0) return -1;
            }
        }
    }
    if (end) *end = p;
    if (major) *major = ma;
    if (minor) *minor = mi;
    if (patch) *patch = pa;
    if (seed) *seed = se;
    return 0;
}

int cc_ccc_version_parse(const char* s, int* major, int* minor, int* patch,
                         int* seed) {
    const char* end;
    if (cc__parse_ver_fields(s, &end, major, minor, patch, seed) != 0)
        return -1;
    if (*end != '\0') return -1;
    return 0;
}

static int cc__parse_bound_op(const char** pp, int* is_lo, int* incl) {
    const char* p;
    if (!pp || !*pp || !is_lo || !incl) return -1;
    p = *pp;
    if (p[0] == '>' && p[1] == '=') {
        *is_lo = 1;
        *incl = 1;
        *pp = p + 2;
        return 0;
    }
    if (p[0] == '>') {
        *is_lo = 1;
        *incl = 0;
        *pp = p + 1;
        return 0;
    }
    if (p[0] == '<' && p[1] == '=') {
        *is_lo = 0;
        *incl = 1;
        *pp = p + 2;
        return 0;
    }
    if (p[0] == '<') {
        *is_lo = 0;
        *incl = 0;
        *pp = p + 1;
        return 0;
    }
    return -1;
}

static int cc_ccc_version_parse_spec(const char* s, CCCccVersionSpec* out) {
    const char* p;
    int n;
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->lo[0] = out->lo[1] = out->lo[2] = out->lo[3] = -1;
    out->hi[0] = out->hi[1] = out->hi[2] = out->hi[3] = -1;
    if (!s || !s[0]) return -1;
    p = s;
    cc__skip_ws(&p);
    if (!*p) return -1;

    if (*p != '>' && *p != '<') {
        const char* end;
        if (cc__parse_ver_fields(p, &end, &out->lo[0], &out->lo[1], &out->lo[2],
                                &out->lo[3]) != 0)
            return -1;
        cc__skip_ws(&end);
        if (*end != '\0') return -1;
        memcpy(out->hi, out->lo, sizeof(out->lo));
        out->bare = 1;
        out->has_lo = 1;
        out->has_hi = 1;
        out->lo_incl = 1;
        out->hi_incl = 1;
        return 0;
    }

    for (n = 0; n < 2; n++) {
        int is_lo = 0, incl = 0;
        int v[4];
        const char* end;
        cc__skip_ws(&p);
        if (cc__parse_bound_op(&p, &is_lo, &incl) != 0) return -1;
        cc__skip_ws(&p);
        if (cc__parse_ver_fields(p, &end, &v[0], &v[1], &v[2], &v[3]) != 0)
            return -1;
        p = end;
        if (is_lo) {
            if (out->has_lo) return -1;
            memcpy(out->lo, v, sizeof(v));
            out->lo_incl = incl;
            out->has_lo = 1;
        } else {
            if (out->has_hi) return -1;
            memcpy(out->hi, v, sizeof(v));
            out->hi_incl = incl;
            out->has_hi = 1;
        }
        cc__skip_ws(&p);
        if (*p == '\0') break;
        if (*p != ',') return -1;
        p++;
        if (n == 1) return -1;
    }
    cc__skip_ws(&p);
    if (*p != '\0') return -1;
    if (!out->has_lo && !out->has_hi) return -1;
    return 0;
}

static int cc__prefix_order(const int bound[4], const int cand[4]) {
    int i;
    for (i = 0; i < 4; i++) {
        if (bound[i] < 0) return 0;
        if (cand[i] < 0) return -1;
        if (cand[i] < bound[i]) return -1;
        if (cand[i] > bound[i]) return 1;
    }
    return 0;
}

static int cc__spec_matches(const CCCccVersionSpec* spec, const char* candidate) {
    int c[4];
    int o;
    if (!spec || !candidate) return 0;
    if (cc_ccc_version_parse(candidate, &c[0], &c[1], &c[2], &c[3]) != 0)
        return 0;
    if (spec->has_lo) {
        o = cc__prefix_order(spec->lo, c);
        if (spec->lo_incl) {
            if (o < 0) return 0;
        } else if (o <= 0) {
            return 0;
        }
    }
    if (spec->has_hi) {
        o = cc__prefix_order(spec->hi, c);
        if (spec->hi_incl) {
            if (o > 0) return 0;
        } else if (o >= 0) {
            return 0;
        }
    }
    return 1;
}

static int cc__each_pin_clause(const char* pin, int match, const char* candidate) {
    char buf[CC_CCC_VERSION_PIN_CAP];
    char* start;
    int any = 0;
    if (!pin || !pin[0] || strlen(pin) >= sizeof(buf)) return 0;
    snprintf(buf, sizeof(buf), "%s", pin);
    start = buf;
    while (*start) {
        CCCccVersionSpec spec;
        char* semi = strchr(start, ';');
        if (semi) *semi = '\0';
        if (!start[0] || cc_ccc_version_parse_spec(start, &spec) != 0) return 0;
        if (match && !cc__spec_matches(&spec, candidate)) return 0;
        any = 1;
        if (!semi) break;
        start = semi + 1;
    }
    return any;
}

int cc_ccc_version_spec_ok(const char* pin) {
    return cc__each_pin_clause(pin, 0, NULL);
}

int cc_ccc_version_current_seed(void) {
    return CCC_BOOTSTRAP_SEED;
}

void cc_ccc_version_current(char* dst, size_t cap) {
    if (!dst || !cap) return;
    snprintf(dst, cap, "%s-%d", CCC_VERSION_BASE, CCC_BOOTSTRAP_SEED);
}

int cc_ccc_version_equal(const char* a, const char* b) {
    int a0, a1, a2, a3, b0, b1, b2, b3;
    if (!a || !b) return 0;
    if (cc_ccc_version_parse(a, &a0, &a1, &a2, &a3) != 0) return 0;
    if (cc_ccc_version_parse(b, &b0, &b1, &b2, &b3) != 0) return 0;
    return a0 == b0 && a1 == b1 && a2 == b2 && a3 == b3;
}

int cc_ccc_version_matches(const char* pin, const char* candidate) {
    if (!pin || !candidate) return 0;
    return cc__each_pin_clause(pin, 1, candidate);
}

int cc_ccc_version_cmp(const char* a, const char* b) {
    int av[4], bv[4], i;
    if (!a || !b) return 0;
    if (cc_ccc_version_parse(a, &av[0], &av[1], &av[2], &av[3]) != 0) return 0;
    if (cc_ccc_version_parse(b, &bv[0], &bv[1], &bv[2], &bv[3]) != 0) return 0;
    for (i = 0; i < 4; i++) {
        if (av[i] < bv[i]) return -1;
        if (av[i] > bv[i]) return 1;
    }
    return 0;
}

int cc_unit_cli_is_as(const char* arg) {
    if (!arg) return 0;
    return strcmp(arg, "--as") == 0 || strncmp(arg, "--as=", 5) == 0;
}

int cc_unit_cli_parse_as_value(const char* val, CCUnitKind* out, char* err,
                               size_t err_cap) {
    CCUnitKind k;
    if (!val || !val[0]) {
        if (err && err_cap)
            snprintf(err, err_cap, "cc: --as requires ccs, cch, or shcc");
        return -1;
    }
    k = cc_unit_kind_from_name(val);
    if (k == CC_UNIT_KIND_UNKNOWN) {
        if (err && err_cap)
            snprintf(err, err_cap, "cc: --as must be ccs, cch, or shcc (got %s)",
                     val);
        return -1;
    }
    if (out) *out = k;
    return 0;
}

int cc_unit_cli_is_version(const char* arg) {
    if (!arg) return 0;
    if (strncmp(arg, "version=", 8) == 0) return 1;
    if (strcmp(arg, "--ccc-version") == 0) return 1;
    if (strncmp(arg, "--ccc-version=", 14) == 0) return 1;
    return 0;
}

int cc_unit_cli_parse_version_value(const char* val, char* pin, size_t cap,
                                    char* err, size_t err_cap) {
    if (!val || !val[0]) {
        if (err && err_cap)
            snprintf(err, err_cap, "cc: version pin requires %s",
                     cc__ver_pin_form);
        return -1;
    }
    if (!cc_ccc_version_spec_ok(val)) {
        if (err && err_cap)
            snprintf(err, err_cap, "cc: version pin must be %s (got %s)",
                     cc__ver_pin_form, val);
        return -1;
    }
    if (pin && cap) {
        snprintf(pin, cap, "%s", val);
    }
    return 0;
}

static void cc__hdr_fail(CCUnitHeader* out, const char* msg) {
    if (!out) return;
    out->kind = CC_UNIT_KIND_UNKNOWN;
    out->ill_formed = 1;
    snprintf(out->err, sizeof(out->err), "%s", msg ? msg : "ill-formed unit header");
}

static int cc__apply_as_tok(const char* tok, CCUnitHeader* out, int* have_as) {
    const char* val = NULL;
    CCUnitKind k;
    if (!tok) return -1;
    if (strncmp(tok, "--as=", 5) == 0) val = tok + 5;
    else return 1; /* not an --as token */
    k = cc_unit_kind_from_name(val);
    if (k == CC_UNIT_KIND_UNKNOWN) {
        cc__hdr_fail(out, "unit header --as must be ccs, cch, or shcc");
        return -1;
    }
    if (*have_as && out->kind != k) {
        cc__hdr_fail(out, "unit header has conflicting --as kinds");
        return -1;
    }
    out->kind = k;
    *have_as = 1;
    return 0;
}

static int cc__apply_ver_tok(const char* tok, CCUnitHeader* out) {
    const char* val = NULL;
    if (!tok) return -1;
    if (strncmp(tok, "version=", 8) == 0) val = tok + 8;
    else if (strncmp(tok, "--ccc-version=", 14) == 0) val = tok + 14;
    else return 1;
    if (!cc_ccc_version_spec_ok(val)) {
        cc__hdr_fail(out,
                     "unit header version= must be a version or bound "
                     "(>=X, >X, <=X, <X, or both)");
        return -1;
    }
    if (out->version[0] && strcmp(out->version, val) != 0) {
        cc__hdr_fail(out, "unit header has conflicting version pins");
        return -1;
    }
    snprintf(out->version, sizeof(out->version), "%s", val);
    return 0;
}

int cc_unit_header_parse_line(const char* line, size_t n, CCUnitHeader* out) {
    char buf[512];
    char* toks[32];
    int nt = 0;
    size_t i = 0;
    int t;
    int have_as = 0;
    int saw_ccc = 0;
    int magic = 0;

    if (out) memset(out, 0, sizeof(*out));
    if (!line || !out) return -1;
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, line, n);
    buf[n] = '\0';

    if (n < 2 || buf[0] != '#' || buf[1] != '!') {
        return 0;
    }

    /* Split remaining text after #! on whitespace. */
    i = 2;
    while (buf[i] && isspace((unsigned char)buf[i])) i++;
    while (buf[i] && nt < (int)(sizeof(toks) / sizeof(toks[0]))) {
        toks[nt++] = buf + i;
        while (buf[i] && !isspace((unsigned char)buf[i])) i++;
        if (buf[i]) {
            buf[i++] = '\0';
            while (buf[i] && isspace((unsigned char)buf[i])) i++;
        }
    }
    if (nt == 0) {
        cc__hdr_fail(out, "empty unit header after #!");
        return 0;
    }

    /* Magic form: first token is exactly "ccc" (no slash). */
    if (strcmp(toks[0], "ccc") == 0) {
        magic = 1;
        if (nt < 2) {
            cc__hdr_fail(out, "#!ccc requires a kind (ccs or cch)");
            return 0;
        }
        out->kind = cc_unit_kind_from_name(toks[1]);
        if (out->kind == CC_UNIT_KIND_UNKNOWN) {
            cc__hdr_fail(out, "#!ccc kind must be ccs or cch");
            return 0;
        }
        if (out->kind == CC_UNIT_KIND_SHCC) {
            cc__hdr_fail(out,
                         "scripts must use #!/usr/bin/env -S ./cc/bin/ccc "
                         "(not #!ccc shcc)");
            return 0;
        }
        for (t = 2; t < nt; t++) {
            int r = cc__apply_ver_tok(toks[t], out);
            if (r < 0) return 0;
            if (r == 0) continue;
            cc__hdr_fail(out, "#!ccc accepts only kind and optional version=");
            return 0;
        }
        out->is_os_shebang = 0;
        return 0;
    }

    /* OS shebang: interpreter basename is ccc (possibly via env -S). */
    for (t = 0; t < nt; t++) {
        if (cc__is_ccc_interp(toks[t])) {
            saw_ccc = 1;
            break;
        }
    }
    if (!saw_ccc) {
        /* Some other #! — not a Concurrent-C unit header. */
        return 0;
    }

    out->is_os_shebang = 1;
    out->kind = CC_UNIT_KIND_SHCC;
    t = 0;
    /* Skip env / -S / interpreter; then parse --as / version=. */
    while (t < nt) {
        const char* tok = toks[t];
        int r;
        if (strcmp(tok, "/usr/bin/env") == 0 || strcmp(tok, "env") == 0 ||
            cc__ends_with(tok, "/env")) {
            t++;
            if (t < nt && strcmp(toks[t], "-S") == 0) t++;
            continue;
        }
        if (cc__is_ccc_interp(tok)) {
            t++;
            continue;
        }
        if (strcmp(tok, "--as") == 0) {
            if (t + 1 >= nt) {
                cc__hdr_fail(out, "shebang --as requires ccs, cch, or shcc");
                return 0;
            }
            t++;
            {
                CCUnitKind k = cc_unit_kind_from_name(toks[t]);
                if (k == CC_UNIT_KIND_UNKNOWN) {
                    cc__hdr_fail(out, "shebang --as must be ccs, cch, or shcc");
                    return 0;
                }
                if (have_as && out->kind != k) {
                    cc__hdr_fail(out, "shebang has conflicting --as kinds");
                    return 0;
                }
                out->kind = k;
                have_as = 1;
            }
            t++;
            continue;
        }
        r = cc__apply_as_tok(tok, out, &have_as);
        if (r < 0) return 0;
        if (r == 0) {
            t++;
            continue;
        }
        if (strcmp(tok, "--ccc-version") == 0) {
            if (t + 1 >= nt) {
                cc__hdr_fail(out, "shebang --ccc-version requires a pin");
                return 0;
            }
            t++;
            if (!cc_ccc_version_spec_ok(toks[t])) {
                cc__hdr_fail(out,
                             "shebang version pin must be a version or bound "
                             "(>=X, >X, <=X, <X, or both)");
                return 0;
            }
            snprintf(out->version, sizeof(out->version), "%s", toks[t]);
            t++;
            continue;
        }
        r = cc__apply_ver_tok(tok, out);
        if (r < 0) return 0;
        if (r == 0) {
            t++;
            continue;
        }
        /* Other shebang argv is driver flags (e.g. --verbose), not header
         * metadata. Kind defaults to shcc unless --as named one. */
        t++;
    }
    (void)magic;
    return 0;
}

int cc_unit_header_from_file(const char* path, CCUnitHeader* out) {
    FILE* f;
    char line[512];
    size_t n;
    if (out) memset(out, 0, sizeof(*out));
    if (!path || !out) return -1;
    f = fopen(path, "rb");
    if (!f) return -1;
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    n = strlen(line);
    return cc_unit_header_parse_line(line, n, out);
}

size_t cc_unit_header_skip(const char* src, size_t n) {
    size_t i = 0;
    CCUnitHeader h;
    if (!src || n < 2 || src[0] != '#' || src[1] != '!') return 0;
    while (i < n && src[i] != '\n') i++;
    if (i < n) i++; /* eat newline */
    if (cc_unit_header_parse_line(src, i, &h) != 0) return 0;
    if (h.ill_formed) return 0;
    if (h.kind == CC_UNIT_KIND_UNKNOWN) return 0;
    return i;
}

int cc_unit_resolve(const char* path, CCUnitKind cli_kind,
                    const char* cli_version, CCUnitKind* kind_out,
                    char version_out[CC_CCC_VERSION_PIN_CAP], char* err,
                    size_t err_cap) {
    CCUnitHeader h;
    CCUnitKind ext;
    CCUnitKind kind;
    char pin[CC_CCC_VERSION_PIN_CAP];

    pin[0] = '\0';
    if (kind_out) *kind_out = CC_UNIT_KIND_UNKNOWN;
    if (version_out) version_out[0] = '\0';
    memset(&h, 0, sizeof(h));
    ext = cc_unit_kind_from_ext(path);

    if (path && path[0]) {
        if (cc_unit_header_from_file(path, &h) != 0) {
            memset(&h, 0, sizeof(h)); /* missing file: suffix / --as only */
        }
        if (h.ill_formed) {
            if (err && err_cap)
                snprintf(err, err_cap, "cc: %s: %s", path, h.err);
            return -1;
        }
    }

    if (cli_kind != CC_UNIT_KIND_UNKNOWN && h.kind != CC_UNIT_KIND_UNKNOWN &&
        cli_kind != h.kind) {
        if (err && err_cap)
            snprintf(err, err_cap,
                     "cc: --as=%s disagrees with unit header kind %s (%s)",
                     cc_unit_kind_name(cli_kind), cc_unit_kind_name(h.kind),
                     path ? path : "");
        return -1;
    }
    if (cli_kind != CC_UNIT_KIND_UNKNOWN) kind = cli_kind;
    else if (h.kind != CC_UNIT_KIND_UNKNOWN) kind = h.kind;
    else kind = ext;

    if (kind == CC_UNIT_KIND_UNKNOWN) {
        if (kind_out) *kind_out = CC_UNIT_KIND_UNKNOWN;
        if (version_out && pin[0])
            snprintf(version_out, CC_CCC_VERSION_PIN_CAP, "%s", pin);
        return 0;
    }

    if (h.kind != CC_UNIT_KIND_UNKNOWN && ext != CC_UNIT_KIND_UNKNOWN &&
        h.kind != ext) {
        if (err && err_cap)
            snprintf(err, err_cap,
                     "cc: %s: unit header kind %s disagrees with suffix "
                     "(.%s)",
                     path, cc_unit_kind_name(h.kind), cc_unit_kind_name(ext));
        return -1;
    }
    if (cli_kind != CC_UNIT_KIND_UNKNOWN && ext != CC_UNIT_KIND_UNKNOWN &&
        h.kind == CC_UNIT_KIND_UNKNOWN && cli_kind != ext) {
        if (err && err_cap)
            snprintf(err, err_cap,
                     "cc: --as=%s disagrees with suffix (.%s) on %s",
                     cc_unit_kind_name(cli_kind), cc_unit_kind_name(ext),
                     path ? path : "");
        return -1;
    }

    if (cli_version && cli_version[0]) {
        if (!cc_ccc_version_spec_ok(cli_version)) {
            if (err && err_cap)
                snprintf(err, err_cap, "cc: version pin must be %s (got %s)",
                         cc__ver_pin_form, cli_version);
            return -1;
        }
        snprintf(pin, sizeof(pin), "%s", cli_version);
    }
    if (h.version[0]) {
        if (pin[0] && strcmp(pin, h.version) != 0) {
            int cli_ver =
                cc_ccc_version_parse(pin, NULL, NULL, NULL, NULL) == 0;
            int hdr_ver =
                cc_ccc_version_parse(h.version, NULL, NULL, NULL, NULL) == 0;
            if (cc_ccc_version_matches(pin, h.version) && hdr_ver) {
                snprintf(pin, sizeof(pin), "%s", h.version);
            } else if (cc_ccc_version_matches(h.version, pin) && cli_ver) {
                /* CLI is a concrete version inside the header bound. */
            } else if (cli_ver && hdr_ver) {
                if (err && err_cap)
                    snprintf(err, err_cap,
                             "cc: version=%s disagrees with unit header pin %s "
                             "(%s)",
                             pin, h.version, path ? path : "");
                return -1;
            } else if (strlen(pin) + 1 + strlen(h.version) + 1 > sizeof(pin)) {
                if (err && err_cap)
                    snprintf(err, err_cap,
                             "cc: version pin too long when combining CLI and "
                             "header (%s)",
                             path ? path : "");
                return -1;
            } else {
                char combined[CC_CCC_VERSION_PIN_CAP];
                snprintf(combined, sizeof(combined), "%s;%s", pin, h.version);
                snprintf(pin, sizeof(pin), "%s", combined);
            }
        }
        if (!pin[0]) snprintf(pin, sizeof(pin), "%s", h.version);
    }

    if (kind_out) *kind_out = kind;
    if (version_out) snprintf(version_out, CC_CCC_VERSION_PIN_CAP, "%s", pin);
    return 0;
}
