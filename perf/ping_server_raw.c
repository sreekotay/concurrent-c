#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define DEFAULT_PORT 6546
#define MAX_EVENTS 256

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    return 0;
}

static int set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0) return -1;
    if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) return -1;
    return 0;
}

static int add_read_event(int kq, int fd, void *udata) {
    struct kevent ev;
    EV_SET(&ev, fd, EVFILT_READ, EV_ADD, 0, 0, udata);
    return kevent(kq, &ev, 1, NULL, 0, NULL);
}

static int del_events(int kq, int fd) {
    struct kevent evs[2];
    EV_SET(&evs[0], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&evs[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    return kevent(kq, evs, 2, NULL, 0, NULL);
}

static void close_client(int kq, int fd) {
    (void)del_events(kq, fd);
    close(fd);
}

int main(int argc, char **argv) {
    int port = (argc > 1) ? atoi(argv[1]) : DEFAULT_PORT;
    if (port <= 0) port = DEFAULT_PORT;

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }
    int one = 1;
    (void)setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    (void)setsockopt(listen_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (set_nonblocking(listen_fd) != 0 || set_cloexec(listen_fd) != 0) {
        perror("fcntl");
        close(listen_fd);
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(listen_fd);
        return 1;
    }
    if (listen(listen_fd, 128) != 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }

    int kq = kqueue();
    if (kq < 0) {
        perror("kqueue");
        close(listen_fd);
        return 1;
    }
    if (set_cloexec(kq) != 0) {
        perror("kqueue cloexec");
        close(kq);
        close(listen_fd);
        return 1;
    }
    if (add_read_event(kq, listen_fd, NULL) != 0) {
        perror("kevent listen");
        close(kq);
        close(listen_fd);
        return 1;
    }

    printf("ping_server_raw listening on 127.0.0.1:%d\n", port);
    fflush(stdout);

    struct kevent events[MAX_EVENTS];
    char buf[1024];
    char reply[1024];
    memset(reply, 'P', sizeof(reply));

    for (;;) {
        int n = kevent(kq, NULL, 0, events, MAX_EVENTS, NULL);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("kevent wait");
            break;
        }
        for (int i = 0; i < n; i++) {
            int fd = (int)events[i].ident;
            if (fd == listen_fd) {
                for (;;) {
                    int client_fd = accept(listen_fd, NULL, NULL);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        perror("accept");
                        break;
                    }
                    (void)setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                    if (set_nonblocking(client_fd) != 0 || set_cloexec(client_fd) != 0) {
                        close(client_fd);
                        continue;
                    }
                    if (add_read_event(kq, client_fd, NULL) != 0) {
                        close(client_fd);
                        continue;
                    }
                }
                continue;
            }

            for (;;) {
                ssize_t r = read(fd, buf, sizeof(buf));
                if (r > 0) {
                    ssize_t off = 0;
                    while (off < r) {
                        ssize_t w = write(fd, reply + off, (size_t)(r - off));
                        if (w > 0) {
                            off += w;
                            continue;
                        }
                        if (w < 0 && errno == EINTR) continue;
                        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                            break;
                        }
                        close_client(kq, fd);
                        off = r;
                        break;
                    }
                    if (off < r) {
                        break;
                    }
                    continue;
                }
                if (r == 0) {
                    close_client(kq, fd);
                    break;
                }
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                close_client(kq, fd);
                break;
            }
        }
    }

    close(kq);
    close(listen_fd);
    return 1;
}
