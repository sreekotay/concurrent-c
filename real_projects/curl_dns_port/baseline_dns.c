/*
 * baseline_dns.c — libcurl DNS resolver baseline for the live prefix.
 *
 * Drives the multi interface so lookups go through the installed resolver
 * queue (stock or Concurrent-C Curl_thrdq). Link against out/prefix (Makefile).
 *
 * Usage:
 *   baseline_dns all [N]
 *   baseline_dns features
 *   baseline_dns sequential [N]
 *   baseline_dns concurrent [N]
 *   baseline_dns abort [N]
 *   baseline_dns nxdomain
 */
#include <curl/curl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Fixed public hosts — comparable across runs. Cycle if N > length. */
static const char *const k_hosts[] = {
    "https://example.com/",
    "https://example.org/",
    "https://example.net/",
    "https://www.cloudflare.com/",
    "https://one.one.one.one/",
    "https://dns.google/",
    "https://neverssl.com/",
    "https://www.wikipedia.org/",
    "https://www.kernel.org/",
    "https://curl.se/",
    "https://github.com/",
    "https://www.apple.com/",
    "https://www.microsoft.com/",
    "https://www.google.com/",
    "https://www.iana.org/",
    "https://www.rfc-editor.org/",
};
static const size_t k_nhosts = sizeof(k_hosts) / sizeof(k_hosts[0]);

static size_t discard_write(char *p, size_t s, size_t n, void *u) {
    (void)p;
    (void)u;
    return s * n;
}

static double monotonic_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void easy_dns_opts(CURL *e, const char *url) {
    curl_easy_setopt(e, CURLOPT_URL, url);
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, discard_write);
    curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(e, CURLOPT_DNS_CACHE_TIMEOUT, 0L);
    curl_easy_setopt(e, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 0L);
    /* Stop after DNS + TCP connect — isolates the resolver from HTTP/TLS
     * quirks. Post-DNS failures still count as DNS success (see dns_ok). */
    curl_easy_setopt(e, CURLOPT_CONNECT_ONLY, 1L);
    curl_easy_setopt(e, CURLOPT_USERAGENT, "cc-curl-dns-baseline/0");
}

/* True when the threaded resolver finished a lookup (even if connect/TLS
 * failed afterward). NXDOMAIN / proxy resolve failures are DNS fails. */
static int dns_ok(CURLcode rc, double dns) {
    if (rc == CURLE_COULDNT_RESOLVE_HOST || rc == CURLE_COULDNT_RESOLVE_PROXY)
        return 0;
    if (rc == CURLE_OK)
        return 1;
    return dns > 0.0 || rc == CURLE_GOT_NOTHING || rc == CURLE_RECV_ERROR ||
           rc == CURLE_SEND_ERROR || rc == CURLE_SSL_CONNECT_ERROR ||
           rc == CURLE_PEER_FAILED_VERIFICATION ||
           rc == CURLE_HTTP_RETURNED_ERROR || rc == CURLE_OPERATION_TIMEDOUT ||
           rc == CURLE_COULDNT_CONNECT;
}

static int cmd_features(void) {
    curl_version_info_data *vi = curl_version_info(CURLVERSION_NOW);
    int asyn = (vi->features & CURL_VERSION_ASYNCHDNS) != 0;
    printf("curl_version: %s\n", vi->version);
    printf("host: %s\n", vi->host);
    printf("AsynchDNS: %s\n", asyn ? "yes" : "no");
    printf("ssl_version: %s\n", vi->ssl_version ? vi->ssl_version : "(none)");
    printf("libz_version: %s\n", vi->libz_version ? vi->libz_version : "(none)");
    if (!asyn) {
        fprintf(stderr, "baseline: FAIL — libcurl built without AsynchDNS\n");
        return 1;
    }
    printf("features: OK\n");
    return 0;
}

static int cmd_nxdomain(void) {
    CURL *e = curl_easy_init();
    CURLcode rc;
    if (!e)
        return 2;
    easy_dns_opts(e, "https://no-such-host.invalid/");
    curl_easy_setopt(e, CURLOPT_TIMEOUT, 10L);
    rc = curl_easy_perform(e);
    curl_easy_cleanup(e);
    printf("nxdomain: curlcode=%d (%s)\n", (int)rc, curl_easy_strerror(rc));
    if (rc != CURLE_COULDNT_RESOLVE_HOST) {
        fprintf(stderr,
                "baseline: FAIL — expected CURLE_COULDNT_RESOLVE_HOST (%d), got %d\n",
                (int)CURLE_COULDNT_RESOLVE_HOST, (int)rc);
        return 1;
    }
    printf("nxdomain: OK\n");
    return 0;
}

static int cmd_sequential(int n) {
    double sum_dns = 0.0, max_dns = 0.0, t0, t1;
    int ok = 0, fail = 0;
    int i;

    t0 = monotonic_s();
    for (i = 0; i < n; i++) {
        CURL *e = curl_easy_init();
        CURLcode rc;
        double dns = 0.0;
        if (!e) {
            fail++;
            continue;
        }
        easy_dns_opts(e, k_hosts[(size_t)i % k_nhosts]);
        rc = curl_easy_perform(e);
        curl_easy_getinfo(e, CURLINFO_NAMELOOKUP_TIME, &dns);
        curl_easy_cleanup(e);
        if (dns_ok(rc, dns)) {
            ok++;
            sum_dns += dns;
            if (dns > max_dns)
                max_dns = dns;
            if (rc != CURLE_OK)
                printf("sequential[%d]: dns_ok post-dns rc=%d (%s) dns=%.4fs\n",
                       i, (int)rc, curl_easy_strerror(rc), dns);
        } else {
            fail++;
            printf("sequential[%d]: dns_fail %d (%s) dns=%.4fs url=%s\n", i,
                   (int)rc, curl_easy_strerror(rc), dns,
                   k_hosts[(size_t)i % k_nhosts]);
        }
    }
    t1 = monotonic_s();
    printf("sequential: n=%d ok=%d fail=%d wall=%.3fs "
           "dns_mean=%.4fs dns_max=%.4fs\n",
           n, ok, fail, t1 - t0, ok ? sum_dns / (double)ok : 0.0, max_dns);
    return fail ? 1 : 0;
}

static int cmp_double(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

static double median_of(double *v, int n) {
    if (n <= 0)
        return 0.0;
    qsort(v, (size_t)n, sizeof(double), cmp_double);
    if (n & 1)
        return v[n / 2];
    return 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

typedef struct {
    int ok;
    int fail;
    double wall;
    double dns_mean;
    double dns_max;
} ConcStats;

static int concurrent_once(int n, ConcStats *st, int quiet) {
    CURLM *m = curl_multi_init();
    CURL **es;
    int i, still, ok = 0, fail = 0;
    double sum_dns = 0.0, max_dns = 0.0, t0, t1;

    memset(st, 0, sizeof(*st));
    if (!m)
        return 2;
    es = calloc((size_t)n, sizeof(*es));
    if (!es) {
        curl_multi_cleanup(m);
        return 2;
    }

    curl_multi_setopt(m, CURLMOPT_MAX_TOTAL_CONNECTIONS, (long)n);
#ifdef CURLMOPT_RESOLVE_THREADS_MAX
    curl_multi_setopt(m, CURLMOPT_RESOLVE_THREADS_MAX, (long)(n < 64 ? n : 64));
#endif

    for (i = 0; i < n; i++) {
        es[i] = curl_easy_init();
        if (!es[i]) {
            fail = n;
            goto done;
        }
        easy_dns_opts(es[i], k_hosts[(size_t)i % k_nhosts]);
        curl_multi_add_handle(m, es[i]);
    }

    t0 = monotonic_s();
    still = 1;
    while (still) {
        CURLMcode mc = curl_multi_perform(m, &still);
        if (mc != CURLM_OK) {
            if (!quiet)
                fprintf(stderr, "concurrent: multi_perform %d\n", (int)mc);
            fail = n;
            goto done;
        }
        if (still) {
            int numfds = 0;
            curl_multi_poll(m, NULL, 0, 1000, &numfds);
        }
    }
    t1 = monotonic_s();

    for (;;) {
        int msgs = 0;
        CURLMsg *msg = curl_multi_info_read(m, &msgs);
        if (!msg)
            break;
        if (msg->msg == CURLMSG_DONE) {
            double dns = 0.0;
            curl_easy_getinfo(msg->easy_handle, CURLINFO_NAMELOOKUP_TIME, &dns);
            if (dns_ok(msg->data.result, dns)) {
                ok++;
                sum_dns += dns;
                if (dns > max_dns)
                    max_dns = dns;
                if (!quiet && msg->data.result != CURLE_OK)
                    printf("concurrent: dns_ok post-dns rc=%d (%s) dns=%.4fs\n",
                           (int)msg->data.result,
                           curl_easy_strerror(msg->data.result), dns);
            } else {
                fail++;
                if (!quiet)
                    printf("concurrent: dns_fail %d (%s) dns=%.4fs\n",
                           (int)msg->data.result,
                           curl_easy_strerror(msg->data.result), dns);
            }
        }
    }

    st->ok = ok;
    st->fail = fail;
    st->wall = t1 - t0;
    st->dns_mean = ok ? sum_dns / (double)ok : 0.0;
    st->dns_max = max_dns;

done:
    for (i = 0; i < n; i++) {
        if (es[i]) {
            curl_multi_remove_handle(m, es[i]);
            curl_easy_cleanup(es[i]);
        }
    }
    free(es);
    curl_multi_cleanup(m);
    return fail ? 1 : 0;
}

static int cmd_concurrent(int n, int rounds) {
    double *walls;
    double *dns_means;
    int r, rc = 0, fail_rounds = 0;
    ConcStats st;
    double wall_med, dns_med, wall_min, wall_max;

    if (rounds < 1)
        rounds = 1;
    walls = calloc((size_t)rounds, sizeof(double));
    dns_means = calloc((size_t)rounds, sizeof(double));
    if (!walls || !dns_means) {
        free(walls);
        free(dns_means);
        return 2;
    }

    for (r = 0; r < rounds; r++) {
        int once = concurrent_once(n, &st, /*quiet=*/1);
        walls[r] = st.wall;
        dns_means[r] = st.dns_mean;
        printf("concurrent[%d/%d]: n=%d ok=%d fail=%d wall=%.3fs "
               "dns_mean=%.4fs dns_max=%.4fs\n",
               r + 1, rounds, n, st.ok, st.fail, st.wall, st.dns_mean,
               st.dns_max);
        if (once) {
            fail_rounds++;
            rc = 1;
        }
    }

    wall_med = median_of(walls, rounds);
    dns_med = median_of(dns_means, rounds);
    /* median_of sorts in place */
    wall_min = walls[0];
    wall_max = walls[rounds - 1];
    printf("concurrent_summary: n=%d rounds=%d fail_rounds=%d "
           "wall_median=%.3fs wall_min=%.3fs wall_max=%.3fs "
           "dns_mean_median=%.4fs\n",
           n, rounds, fail_rounds, wall_med, wall_min, wall_max, dns_med);

    free(walls);
    free(dns_means);
    return rc;
}

/*
 * Start N concurrent resolves, then tear the multi down without waiting.
 * Two perform calls rarely leave workers inside getaddrinfo, so this is
 * "did not hang on the fast path," not a blocked join. The join that waits
 * for an in-flight process lives in thrdqueue_smoke.c (make queue-smoke).
 */
static int abort_once(int n, double *elapsed_out) {
    CURLM *m = curl_multi_init();
    CURL **es;
    int i, still;
    double t0, t1;

    if (!m)
        return 2;
    es = calloc((size_t)n, sizeof(*es));
    if (!es) {
        curl_multi_cleanup(m);
        return 2;
    }
#ifdef CURLMOPT_RESOLVE_THREADS_MAX
    curl_multi_setopt(m, CURLMOPT_RESOLVE_THREADS_MAX, (long)(n < 64 ? n : 64));
#endif

    for (i = 0; i < n; i++) {
        es[i] = curl_easy_init();
        if (!es[i])
            goto fail;
        easy_dns_opts(es[i], k_hosts[(size_t)i % k_nhosts]);
        curl_easy_setopt(es[i], CURLOPT_TIMEOUT, 60L);
        curl_multi_add_handle(m, es[i]);
    }

    t0 = monotonic_s();
    (void)curl_multi_perform(m, &still);
    (void)curl_multi_perform(m, &still);

    for (i = 0; i < n; i++) {
        if (es[i]) {
            curl_multi_remove_handle(m, es[i]);
            curl_easy_cleanup(es[i]);
            es[i] = NULL;
        }
    }
    curl_multi_cleanup(m);
    t1 = monotonic_s();
    *elapsed_out = t1 - t0;
    free(es);
    return 0;

fail:
    for (i = 0; i < n; i++) {
        if (es[i]) {
            curl_multi_remove_handle(m, es[i]);
            curl_easy_cleanup(es[i]);
        }
    }
    free(es);
    curl_multi_cleanup(m);
    return 2;
}

static int cmd_abort(int n, int rounds) {
    double *walls;
    int r, rc = 0, fail_rounds = 0;
    double wall_med, wall_min, wall_max;

    if (rounds < 1)
        rounds = 1;
    walls = calloc((size_t)rounds, sizeof(double));
    if (!walls)
        return 2;

    for (r = 0; r < rounds; r++) {
        double elapsed = 0.0;
        int once = abort_once(n, &elapsed);
        walls[r] = elapsed;
        printf("abort[%d/%d]: n=%d wall=%.3fs\n", r + 1, rounds, n, elapsed);
        if (once || elapsed > 5.0) {
            fail_rounds++;
            rc = 1;
            if (elapsed > 5.0)
                fprintf(stderr,
                        "baseline: FAIL — abort teardown took %.3fs (>5s)\n",
                        elapsed);
        }
    }

    wall_med = median_of(walls, rounds);
    wall_min = walls[0];
    wall_max = walls[rounds - 1];
    printf("abort_summary: n=%d rounds=%d fail_rounds=%d "
           "wall_median=%.3fs wall_min=%.3fs wall_max=%.3fs\n",
           n, rounds, fail_rounds, wall_med, wall_min, wall_max);
    if (!rc)
        printf("abort: OK\n");

    free(walls);
    return rc;
}

static int cmd_all(int n, int rounds) {
    int rc = 0;
    printf("=== baseline_dns features ===\n");
    rc |= cmd_features();
    printf("=== baseline_dns nxdomain ===\n");
    rc |= cmd_nxdomain();
    printf("=== baseline_dns sequential n=%d ===\n", n > 4 ? 4 : n);
    rc |= cmd_sequential(n > 4 ? 4 : n);
    printf("=== baseline_dns concurrent n=%d rounds=%d ===\n", n, rounds);
    rc |= cmd_concurrent(n, rounds);
    printf("=== baseline_dns abort n=%d rounds=%d ===\n", n, rounds);
    rc |= cmd_abort(n, rounds);
    printf("=== baseline_dns summary: %s ===\n", rc ? "FAIL" : "OK");
    return rc;
}

int main(int argc, char **argv) {
    const char *cmd;
    int n = 32;
    int rounds = 10;
    int rc;

    if (argc < 2) {
        fprintf(stderr,
                "usage: %s all|features|sequential|concurrent|abort|nxdomain "
                "[N [ROUNDS]]\n",
                argv[0]);
        return 2;
    }
    cmd = argv[1];
    if (argc >= 3)
        n = atoi(argv[2]);
    if (argc >= 4)
        rounds = atoi(argv[3]);
    if (n < 1)
        n = 1;
    if (rounds < 1)
        rounds = 1;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (strcmp(cmd, "features") == 0)
        rc = cmd_features();
    else if (strcmp(cmd, "nxdomain") == 0)
        rc = cmd_nxdomain();
    else if (strcmp(cmd, "sequential") == 0)
        rc = cmd_sequential(n);
    else if (strcmp(cmd, "concurrent") == 0)
        rc = cmd_concurrent(n, rounds);
    else if (strcmp(cmd, "abort") == 0)
        rc = cmd_abort(n, rounds);
    else if (strcmp(cmd, "all") == 0)
        rc = cmd_all(n, rounds);
    else {
        fprintf(stderr, "unknown command: %s\n", cmd);
        rc = 2;
    }
    curl_global_cleanup();
    return rc;
}
