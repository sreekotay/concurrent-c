/*
 * Concurrent-C HTTP Client Runtime
 *
 * libcurl-based implementation. Compiled as part of user code when 
 * CC_ENABLE_HTTP is defined and std/http.cch is included.
 * 
 * FUTURE: Investigate patching libcurl for zero-copy receive directly into
 * arena buffers. Currently requires one copy (curl's buffer → arena).
 * See: https://curl.se/mail/lib-2023-XX/XXXX.html (hypothetical)
 * Approach: Custom CURLOPT_WRITEFUNCTION that receives pointer to user buffer
 * instead of curl's internal buffer.
 */

#ifndef CC_HTTP_IMPL_GUARD
#define CC_HTTP_IMPL_GUARD 1

/* Note: This file is included from http.cch, so header types are already defined */
#include <curl/curl.h>
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * Internal: Write callback context
 * ============================================================================ */

typedef struct {
    CCArena* arena;
    char* data;
    size_t len;
    size_t cap;
    int error;
} CCHttpWriteCtx;

/* curl calls this with received data - we copy into arena */
static size_t cc__http_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    CCHttpWriteCtx* ctx = userdata;
    size_t bytes = size * nmemb;
    
    if (ctx->error) return 0;
    
    /* Grow buffer if needed */
    if (ctx->len + bytes > ctx->cap) {
        size_t new_cap = ctx->cap * 2;
        if (new_cap < ctx->len + bytes) new_cap = ctx->len + bytes + 4096;
        if (new_cap > 64 * 1024 * 1024) {  /* 64MB limit */
            ctx->error = 1;
            return 0;
        }
        char* new_data = cc_arena_alloc(ctx->arena, new_cap, 1);
        if (!new_data) {
            ctx->error = 1;
            return 0;
        }
        if (ctx->data && ctx->len > 0) {
            memcpy(new_data, ctx->data, ctx->len);
        }
        ctx->data = new_data;
        ctx->cap = new_cap;
    }
    
    memcpy(ctx->data + ctx->len, ptr, bytes);
    ctx->len += bytes;
    return bytes;
}

/* Header callback to capture headers */
typedef struct {
    CCArena* arena;
    char* data;
    size_t len;
    size_t cap;
} CCHttpHeaderCtx;

static size_t cc__http_header_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    CCHttpHeaderCtx* ctx = userdata;
    size_t bytes = size * nmemb;
    
    if (ctx->len + bytes > ctx->cap) {
        size_t new_cap = ctx->cap * 2;
        if (new_cap < ctx->len + bytes) new_cap = ctx->len + bytes + 1024;
        char* new_data = cc_arena_alloc(ctx->arena, new_cap, 1);
        if (!new_data) return 0;
        if (ctx->data && ctx->len > 0) {
            memcpy(new_data, ctx->data, ctx->len);
        }
        ctx->data = new_data;
        ctx->cap = new_cap;
    }
    
    memcpy(ctx->data + ctx->len, ptr, bytes);
    ctx->len += bytes;
    return bytes;
}

/* ============================================================================
 * Internal: Map curl errors
 * ============================================================================ */

static CCHttpError cc__curl_to_error(CURLcode code) {
    switch (code) {
        case CURLE_OK: return CC_HTTP_OK;
        case CURLE_URL_MALFORMAT: return CC_HTTP_INVALID_URL;
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_CONNECT:
            return CC_HTTP_NET_ERROR;
        case CURLE_OPERATION_TIMEDOUT: return CC_HTTP_TIMEOUT;
        case CURLE_TOO_MANY_REDIRECTS: return CC_HTTP_TOO_MANY_REDIRECTS;
        default: return CC_HTTP_NET_ERROR;
    }
}

/* ============================================================================
 * Internal: Core request implementation
 * ============================================================================ */

static CCHttpResponse cc__http_request(CCArena* arena, 
                                        const char* method,
                                        const char* url, size_t url_len,
                                        const char* body, size_t body_len,
                                        const CCHttpClientConfig* config,
                                        CCHttpErrorInfo* out_err) {
    CCHttpResponse resp = {0};
    *out_err = (CCHttpErrorInfo){0};
    
    /* Null-terminate URL */
    char* url_z = cc_arena_alloc(arena, url_len + 1, 1);
    if (!url_z) {
        out_err->code = CC_HTTP_NET_ERROR;
        return resp;
    }
    memcpy(url_z, url, url_len);
    url_z[url_len] = '\0';
    
    CURL* curl = curl_easy_init();
    if (!curl) {
        out_err->code = CC_HTTP_NET_ERROR;
        return resp;
    }
    
    /* Body write context */
    CCHttpWriteCtx write_ctx = {
        .arena = arena,
        .data = cc_arena_alloc(arena, 4096, 1),
        .len = 0,
        .cap = 4096,
        .error = 0,
    };
    
    /* Header write context */
    CCHttpHeaderCtx header_ctx = {
        .arena = arena,
        .data = cc_arena_alloc(arena, 1024, 1),
        .len = 0,
        .cap = 1024,
    };
    
    /* Configure request */
    curl_easy_setopt(curl, CURLOPT_URL, url_z);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cc__http_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &write_ctx);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, cc__http_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &header_ctx);
    
    /* Method */
    if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        if (body && body_len > 0) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
        }
    } else if (strcmp(method, "PUT") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        if (body && body_len > 0) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
        }
    } else if (strcmp(method, "DELETE") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    }
    /* GET is default */
    
    /* Config options */
    uint32_t timeout_ms = config ? config->timeout_ms : 30000;
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
    
    int follow = config ? config->follow_redirects : 1;
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, follow ? 1L : 0L);
    
    int max_redir = config ? config->max_redirects : 10;
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, (long)max_redir);
    
    /* User-Agent */
    if (config && config->user_agent.ptr && config->user_agent.len > 0) {
        char* ua = cc_arena_alloc(arena, config->user_agent.len + 1, 1);
        if (ua) {
            memcpy(ua, config->user_agent.ptr, config->user_agent.len);
            ua[config->user_agent.len] = '\0';
            curl_easy_setopt(curl, CURLOPT_USERAGENT, ua);
        }
    } else {
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "CC-HTTP/1.0");
    }
    
    /* Perform request */
    CURLcode res = curl_easy_perform(curl);
    
    if (res != CURLE_OK || write_ctx.error) {
        out_err->code = write_ctx.error ? CC_HTTP_BODY_TOO_LARGE : cc__curl_to_error(res);
        curl_easy_cleanup(curl);
        return resp;
    }
    
    /* Extract response info */
    long http_code;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    resp.status = (uint16_t)http_code;
    
    /* Get final URL (after redirects) */
    char* effective_url = NULL;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url);
    if (effective_url) {
        size_t elen = strlen(effective_url);
        char* url_copy = cc_arena_alloc(arena, elen, 1);
        if (url_copy) {
            memcpy(url_copy, effective_url, elen);
            resp.url = (CCSlice){ .ptr = url_copy, .len = elen };
        }
    }
    
    resp.headers = (CCSlice){ .ptr = header_ctx.data, .len = header_ctx.len };
    resp.body = (CCSlice){ .ptr = write_ctx.data, .len = write_ctx.len };
    
    curl_easy_cleanup(curl);
    return resp;
}

/* ============================================================================
 * Public API: Simple functions
 * ============================================================================ */

CCHttpResponse cc_http_get(CCArena* arena, const char* url, size_t url_len, 
                            CCHttpErrorInfo* out_err) {
    return cc__http_request(arena, "GET", url, url_len, NULL, 0, NULL, out_err);
}

CCHttpResponse cc_http_post(CCArena* arena, const char* url, size_t url_len,
                             const char* body, size_t body_len, 
                             CCHttpErrorInfo* out_err) {
    return cc__http_request(arena, "POST", url, url_len, body, body_len, NULL, out_err);
}

/* ============================================================================
 * Public API: Client with config
 * ============================================================================ */

CCHttpClient cc_http_client_new(CCHttpClientConfig config) {
    return (CCHttpClient){ .config = config };
}

CCHttpResponse cc_http_client_get(CCHttpClient* client, CCArena* arena,
                                   const char* url, size_t url_len, 
                                   CCHttpErrorInfo* out_err) {
    return cc__http_request(arena, "GET", url, url_len, NULL, 0, 
                            client ? &client->config : NULL, out_err);
}

CCHttpResponse cc_http_client_post(CCHttpClient* client, CCArena* arena,
                                    const char* url, size_t url_len,
                                    const char* body, size_t body_len, 
                                    CCHttpErrorInfo* out_err) {
    return cc__http_request(arena, "POST", url, url_len, body, body_len,
                            client ? &client->config : NULL, out_err);
}

CCHttpResponse cc_http_client_request(CCHttpClient* client, CCArena* arena,
                                        CCHttpRequest req, 
                                        CCHttpErrorInfo* out_err) {
    /* Null-terminate method */
    char method[16] = "GET";
    if (req.method.ptr && req.method.len > 0 && req.method.len < sizeof(method)) {
        memcpy(method, req.method.ptr, req.method.len);
        method[req.method.len] = '\0';
    }
    
    return cc__http_request(arena, method, 
                            req.url.ptr, req.url.len,
                            req.body.ptr, req.body.len,
                            client ? &client->config : NULL, out_err);
}

/* ============================================================================
 * Public API: URL parsing
 * ============================================================================ */

CCParsedUrl cc_url_parse(const char* url, size_t url_len, CCHttpError* out_err) {
    CCParsedUrl result = {0};
    *out_err = CC_HTTP_OK;
    
    if (!url || url_len == 0) {
        *out_err = CC_HTTP_INVALID_URL;
        return result;
    }
    
    const char* p = url;
    const char* end = url + url_len;
    
    /* Scheme */
    const char* scheme_end = memchr(p, ':', end - p);
    if (!scheme_end || scheme_end + 2 >= end || scheme_end[1] != '/' || scheme_end[2] != '/') {
        *out_err = CC_HTTP_INVALID_URL;
        return result;
    }
    result.scheme = (CCSlice){ .ptr = (char*)p, .len = scheme_end - p };
    p = scheme_end + 3;  /* skip :// */
    
    /* Host (and optional port) */
    const char* host_start = p;
    const char* host_end = NULL;
    const char* port_start = NULL;
    
    while (p < end && *p != '/' && *p != '?' && *p != '#') {
        if (*p == ':' && !port_start) {
            host_end = p;
            port_start = p + 1;
        }
        p++;
    }
    
    if (!host_end) host_end = p;
    result.host = (CCSlice){ .ptr = (char*)host_start, .len = host_end - host_start };
    
    if (port_start && p > port_start) {
        /* Parse port number */
        int port = 0;
        for (const char* pp = port_start; pp < p; pp++) {
            if (*pp < '0' || *pp > '9') break;
            port = port * 10 + (*pp - '0');
        }
        result.port = (uint16_t)port;
    }
    
    /* Path */
    if (p < end && *p == '/') {
        const char* path_start = p;
        while (p < end && *p != '?' && *p != '#') p++;
        result.path = (CCSlice){ .ptr = (char*)path_start, .len = p - path_start };
    }
    
    /* Query */
    if (p < end && *p == '?') {
        p++;  /* skip ? */
        const char* query_start = p;
        while (p < end && *p != '#') p++;
        result.query = (CCSlice){ .ptr = (char*)query_start, .len = p - query_start };
    }
    
    /* Fragment */
    if (p < end && *p == '#') {
        p++;  /* skip # */
        result.fragment = (CCSlice){ .ptr = (char*)p, .len = end - p };
    }
    
    return result;
}

#endif /* CC_HTTP_IMPL_GUARD */
