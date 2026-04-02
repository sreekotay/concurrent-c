#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_PORT 6545
#define DEFAULT_CLIENTS 100
#define DEFAULT_REQUESTS 100000

typedef struct {
    int port;
    int requests;
    int errors;
} worker_args;

static int env_int_or_default(const char* name, int fallback, int min_value) {
    const char* v = getenv(name);
    if (!v || !v[0]) return fallback;
    int parsed = atoi(v);
    return parsed < min_value ? fallback : parsed;
}

static double time_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static int send_all(int fd, const void* data, size_t len) {
    const char* p = (const char*)data;
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, p + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static int recv_all(int fd, void* data, size_t len) {
    char* p = (char*)data;
    size_t off = 0;
    while (off < len) {
        ssize_t n = recv(fd, p + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static void* worker_main(void* arg) {
    worker_args* wa = (worker_args*)arg;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        wa->errors = 1;
        return NULL;
    }

    int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)wa->port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
        close(fd);
        wa->errors = 1;
        return NULL;
    }

    const char req = 'Q';
    char resp = 0;
    for (int i = 0; i < wa->requests; i++) {
        if (send_all(fd, &req, 1) != 0 || recv_all(fd, &resp, 1) != 0 || resp != 'P') {
            wa->errors = 1;
            break;
        }
    }

    close(fd);
    return NULL;
}

int main(int argc, char** argv) {
    int port = (argc > 1) ? atoi(argv[1]) : DEFAULT_PORT;
    if (port <= 0) port = DEFAULT_PORT;
    int clients = env_int_or_default("CC_PING_CLIENTS", DEFAULT_CLIENTS, 1);
    int requests = env_int_or_default("CC_PING_REQUESTS", DEFAULT_REQUESTS, 1);

    pthread_t* threads = calloc((size_t)clients, sizeof(*threads));
    worker_args* args = calloc((size_t)clients, sizeof(*args));
    if (!threads || !args) {
        fprintf(stderr, "ping_bench_client: allocation failed\n");
        free(threads);
        free(args);
        return 1;
    }

    double start = time_now_ms();
    for (int i = 0; i < clients; i++) {
        args[i].port = port;
        args[i].requests = requests;
        args[i].errors = 0;
        if (pthread_create(&threads[i], NULL, worker_main, &args[i]) != 0) {
            fprintf(stderr, "ping_bench_client: pthread_create failed\n");
            free(threads);
            free(args);
            return 1;
        }
    }
    int errors = 0;
    for (int i = 0; i < clients; i++) {
        pthread_join(threads[i], NULL);
        errors += args[i].errors;
    }
    double elapsed = time_now_ms() - start;

    long long total_requests = (long long)clients * (long long)requests;
    double req_per_sec = total_requests / (elapsed / 1000.0);
    double usec_per_req = (elapsed * 1000.0) / (double)total_requests;
    printf("ping_bench_client: %.2f req/s (clients=%d requests=%d total=%lld elapsed_ms=%.2f usec_per_req=%.2f errors=%d)\n",
           req_per_sec, clients, requests, total_requests, elapsed, usec_per_req, errors);

    free(threads);
    free(args);
    return errors ? 1 : 0;
}
