#include "cc_async_runtime.h"
#include "cc_async_backend.h"
#include "cc_async_backend_poll.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

static CCExec *g_exec = NULL;
static const CCAsyncBackendOps* g_backend = NULL;
static void* g_backend_ctx = NULL;
static const char* g_backend_name = "executor";
static int g_backend_probed = 0;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

int cc_async_runtime_init(size_t workers, size_t queue_cap) {
    pthread_mutex_lock(&g_mu);
    if (g_exec) { pthread_mutex_unlock(&g_mu); return 0; }
    g_exec = cc_exec_create(workers, queue_cap);
    pthread_mutex_unlock(&g_mu);
    return g_exec ? 0 : -1;
}

int cc_async_runtime_set_backend(const CCAsyncBackendOps* ops, void* ctx, const char* name) {
    pthread_mutex_lock(&g_mu);
    g_backend = ops;
    g_backend_ctx = ctx;
    g_backend_name = name ? name : "custom";
    g_backend_probed = 1;
    pthread_mutex_unlock(&g_mu);
    return 0;
}

static void cc__probe_backend(void) {
    if (g_backend_probed) return;
    const char* env = getenv("CC_RUNTIME_BACKEND");
    if (env && strcmp(env, "executor") == 0) {
        pthread_mutex_lock(&g_mu);
        g_backend = NULL; g_backend_ctx = NULL; g_backend_name = "executor"; g_backend_probed = 1;
        pthread_mutex_unlock(&g_mu);
        return;
    }
    if (!env || strcmp(env, "poll") == 0) {
        cc_async_backend_poll_register(); // will set backend if succeeds
        pthread_mutex_lock(&g_mu);
        g_backend_probed = 1;
        if (!g_backend) {
            g_backend_name = "executor";
        }
        pthread_mutex_unlock(&g_mu);
        return;
    }
    // Unknown env value: fall back to executor
    pthread_mutex_lock(&g_mu);
    g_backend = NULL; g_backend_ctx = NULL; g_backend_name = "executor"; g_backend_probed = 1;
    pthread_mutex_unlock(&g_mu);
}

const CCAsyncBackendOps* cc_async_runtime_backend(void** out_ctx) {
    if (!g_backend_probed) cc__probe_backend();
    pthread_mutex_lock(&g_mu);
    const CCAsyncBackendOps* ops = g_backend;
    if (out_ctx) *out_ctx = g_backend_ctx;
    pthread_mutex_unlock(&g_mu);
    return ops;
}

const char* cc_async_runtime_backend_name(void) {
    if (!g_backend_probed) cc__probe_backend();
    pthread_mutex_lock(&g_mu);
    const char* name = g_backend_name;
    pthread_mutex_unlock(&g_mu);
    return name;
}

CCExec* cc_async_runtime_exec(void) {
    pthread_mutex_lock(&g_mu);
    CCExec* ex = g_exec;
    pthread_mutex_unlock(&g_mu);
    return ex;
}

void cc_async_runtime_shutdown(void) {
    pthread_mutex_lock(&g_mu);
    if (g_exec) {
        cc_exec_shutdown(g_exec);
        cc_exec_free(g_exec);
        g_exec = NULL;
    }
    g_backend = NULL;
    g_backend_ctx = NULL;
    g_backend_name = "executor";
    g_backend_probed = 0;
    pthread_mutex_unlock(&g_mu);
}

