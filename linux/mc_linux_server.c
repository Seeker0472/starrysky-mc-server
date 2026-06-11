#define _POSIX_C_SOURCE 200809L

#include "mc_config.h"
#include "mc_ringbuf.h"
#include "mc_server.h"
#include "mc_world_compressed.h"
#include "mc_world_compressed_assets.h"

#if !MC_PROTOCOL_COMPRESSION_ENABLE || !MC_USE_PSRAM_COMPRESSED_MAP
#error "linux/mc_linux_server.c requires MC_PROTOCOL_COMPRESSION_ENABLE=1 and MC_USE_PSRAM_COMPRESSED_MAP=1"
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MC_LINUX_DEFAULT_LISTEN "127.0.0.1:25565"
#define MC_LINUX_BACKLOG 1
#define MC_LINUX_IO_BUF 2048u
#define MC_LINUX_POLL_TIMEOUT_MS 10

typedef struct {
    struct sockaddr_in addr;
    const char *listen_text;
    int verbose;
} mc_linux_config_t;

typedef enum {
    MC_LINUX_CLIENT_PEER_CLOSE = 0,
    MC_LINUX_CLIENT_RECV_ERROR,
    MC_LINUX_CLIENT_SEND_ERROR,
    MC_LINUX_CLIENT_PROTOCOL_RECEIVE_FAILURE,
    MC_LINUX_CLIENT_BACKPRESSURE_OR_TICK_FAILURE,
    MC_LINUX_CLIENT_SHUTDOWN,
    MC_LINUX_CLIENT_TX_DRAIN_WRITE_FAILURE,
    MC_LINUX_CLIENT_WORLD_INIT_FAILURE
} mc_linux_client_reason_t;

static volatile sig_atomic_t shutdown_requested;

static void handle_shutdown_signal(int signo)
{
    (void)signo;
    shutdown_requested = 1;
}

static void print_usage(const char *argv0)
{
    fprintf(stdout,
            "Usage: %s [--listen IPv4:PORT] [--verbose] [--help]\n"
            "\n"
            "Options:\n"
            "  --listen IPv4:PORT  Numeric IPv4 listen address (default %s)\n"
            "  --verbose           Print compact core trace events to stderr\n"
            "  -h, --help          Show this help\n",
            argv0,
            MC_LINUX_DEFAULT_LISTEN);
}

static int parse_listen(const char *text, struct sockaddr_in *out)
{
    char host[INET_ADDRSTRLEN];
    char *end = 0;
    const char *colon = strrchr(text, ':');
    unsigned long port;
    size_t host_len;

    if (text == 0 || out == 0 || colon == 0 || colon == text || colon[1] == '\0') {
        return 0;
    }

    host_len = (size_t)(colon - text);
    if (host_len >= sizeof(host)) {
        return 0;
    }
    memcpy(host, text, host_len);
    host[host_len] = '\0';

    errno = 0;
    port = strtoul(colon + 1, &end, 10);
    if (errno != 0 || end == colon + 1 || *end != '\0' || port == 0ul || port > 65535ul) {
        return 0;
    }

    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &out->sin_addr) != 1) {
        return 0;
    }

    return 1;
}

static int parse_args(int argc, char **argv, mc_linux_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config->listen_text = MC_LINUX_DEFAULT_LISTEN;
    if (!parse_listen(config->listen_text, &config->addr)) {
        fprintf(stderr, "invalid default listen address: %s\n", MC_LINUX_DEFAULT_LISTEN);
        return 0;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--listen") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--listen requires IPv4:PORT\n");
                return 0;
            }
            config->listen_text = argv[++i];
            if (!parse_listen(config->listen_text, &config->addr)) {
                fprintf(stderr, "invalid --listen numeric IPv4:PORT value: %s\n", config->listen_text);
                return 0;
            }
        } else if (strcmp(argv[i], "--verbose") == 0) {
            config->verbose = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 0;
        }
    }

    return 1;
}

static int install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_shutdown_signal;
    if (sigemptyset(&action.sa_mask) != 0) {
        return 0;
    }
    if (sigaction(SIGINT, &action, 0) != 0 || sigaction(SIGTERM, &action, 0) != 0) {
        return 0;
    }
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        return 0;
    }

    return 1;
}

static int create_listener(const struct sockaddr_in *addr)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;

    if (fd < 0) {
        perror("socket");
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        perror("setsockopt(SO_REUSEADDR)");
        close(fd);
        return -1;
    }
    if (bind(fd, (const struct sockaddr *)addr, sizeof(*addr)) != 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, MC_LINUX_BACKLOG) != 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0) {
        return 0;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static uint32_t monotonic_ticks(void)
{
    struct timespec now;
    uint64_t ticks;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        perror("clock_gettime(CLOCK_MONOTONIC)");
        return 0u;
    }

    ticks = (uint64_t)now.tv_sec * (uint64_t)MC_SERVER_TICKS_PER_SECOND;
    ticks += ((uint64_t)now.tv_nsec * (uint64_t)MC_SERVER_TICKS_PER_SECOND) / 1000000000ull;
    return (uint32_t)ticks;
}

static const char *trace_type_name(mc_trace_event_type_t type)
{
    switch (type) {
    case MC_TRACE_FRAME_READY:
        return "frame";
    case MC_TRACE_HANDSHAKE:
        return "handshake";
    case MC_TRACE_STATUS_REQUEST:
        return "status_request";
    case MC_TRACE_STATUS_PING:
        return "status_ping";
    case MC_TRACE_LOGIN_START:
        return "login_start";
    case MC_TRACE_PLAY_ENTER:
        return "play_enter";
    case MC_TRACE_BOOTSTRAP_STAGE:
        return "bootstrap_stage";
    case MC_TRACE_BOOTSTRAP_DONE:
        return "bootstrap_done";
    case MC_TRACE_QUEUE_FULL:
        return "queue_full";
    case MC_TRACE_KEEPALIVE_SEND:
        return "keepalive_send";
    case MC_TRACE_KEEPALIVE_ACK:
        return "keepalive_ack";
    case MC_TRACE_PLAY_UNHANDLED:
        return "play_unhandled";
    default:
        return "unknown";
    }
}

static const char *client_reason_text(mc_linux_client_reason_t reason)
{
    switch (reason) {
    case MC_LINUX_CLIENT_PEER_CLOSE:
        return "peer close";
    case MC_LINUX_CLIENT_RECV_ERROR:
        return "recv error";
    case MC_LINUX_CLIENT_SEND_ERROR:
        return "send error";
    case MC_LINUX_CLIENT_PROTOCOL_RECEIVE_FAILURE:
        return "protocol receive failure";
    case MC_LINUX_CLIENT_BACKPRESSURE_OR_TICK_FAILURE:
        return "backpressure/tick failure";
    case MC_LINUX_CLIENT_SHUTDOWN:
        return "shutdown";
    case MC_LINUX_CLIENT_TX_DRAIN_WRITE_FAILURE:
        return "TX drain/write failure";
    case MC_LINUX_CLIENT_WORLD_INIT_FAILURE:
        return "world init failure";
    default:
        return "unknown";
    }
}

static void trace_sink(void *user, const mc_trace_event_t *event)
{
    (void)user;
    fprintf(stderr,
            "trace type=%s v0=%ld v1=%ld len=%lu",
            trace_type_name(event->type),
            (long)event->value0,
            (long)event->value1,
            (unsigned long)event->text_len);
    if (event->text != 0 && event->text_len > 0u) {
        fprintf(stderr, " text=\"%.*s\"", (int)event->text_len, event->text);
    }
    fputc('\n', stderr);
}

static int wait_for_fd(int fd, short events)
{
    struct pollfd pfd;

    pfd.fd = fd;
    pfd.events = events;
    pfd.revents = 0;

    for (;;) {
        int rc = poll(&pfd, 1u, MC_LINUX_POLL_TIMEOUT_MS);
        if (rc > 0) {
            if ((pfd.revents & POLLNVAL) != 0) {
                return -1;
            }
            if ((pfd.revents & events) != 0) {
                return 1;
            }
            if ((pfd.revents & (POLLERR | POLLHUP)) != 0) {
                return -1;
            }
        } else if (rc == 0) {
            return 0;
        } else if (errno != EINTR) {
            perror("poll");
            return -1;
        }

        if (shutdown_requested) {
            return -1;
        }
    }
}

static int drain_tx(int client_fd, mc_ringbuf_t *tx, mc_linux_client_reason_t *reason)
{
    uint8_t out[MC_LINUX_IO_BUF];

    while (mc_ringbuf_len(tx) > 0u && !shutdown_requested) {
        size_t out_len;
        size_t tx_len;
        ssize_t written;
        int ready = wait_for_fd(client_fd, POLLOUT);

        if (ready < 0) {
            *reason = shutdown_requested ? MC_LINUX_CLIENT_SHUTDOWN : MC_LINUX_CLIENT_TX_DRAIN_WRITE_FAILURE;
            return 0;
        }
        if (ready == 0) {
            return 1;
        }

        tx_len = mc_ringbuf_len(tx);
        out_len = tx_len < sizeof(out) ? tx_len : sizeof(out);
        if (out_len == 0u) {
            return 1;
        }
        for (size_t i = 0; i < out_len; i++) {
            if (!mc_ringbuf_peek(tx, i, &out[i])) {
                *reason = MC_LINUX_CLIENT_TX_DRAIN_WRITE_FAILURE;
                return 0;
            }
        }

        for (;;) {
            written = send(client_fd, out, out_len, 0);
            if (written > 0) {
                mc_ringbuf_drop(tx, (size_t)written);
                break;
            }
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }
            if (written < 0) {
                if (errno != EPIPE && errno != ECONNRESET) {
                    perror("send");
                }
                *reason = MC_LINUX_CLIENT_SEND_ERROR;
                return 0;
            }
            *reason = MC_LINUX_CLIENT_TX_DRAIN_WRITE_FAILURE;
            return 0;
        }
    }

    return 1;
}

static mc_linux_client_reason_t run_client(int client_fd, int verbose)
{
    mc_server_t server;
    uint8_t tx_storage[MC_TX_RING_CAP];
    mc_ringbuf_t tx;
    uint8_t in[MC_LINUX_IO_BUF];
    void *world_arena = malloc((size_t)mc_world_compressed_total_bytes);
    mc_linux_client_reason_t reason = MC_LINUX_CLIENT_SHUTDOWN;

    if (world_arena == 0 ||
        !mc_world_compressed_init(world_arena, (size_t)mc_world_compressed_total_bytes)) {
        free(world_arena);
        return MC_LINUX_CLIENT_WORLD_INIT_FAILURE;
    }

    mc_server_init(&server);
    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    if (verbose) {
        mc_server_set_trace(&server, trace_sink, 0);
    }

    while (!shutdown_requested) {
        int ready;

        if (!drain_tx(client_fd, &tx, &reason)) {
            goto done;
        }
        if (!mc_server_tick_at(&server, &tx, monotonic_ticks())) {
            fprintf(stderr, "server tick backpressure\n");
            if (!drain_tx(client_fd, &tx, &reason)) {
                goto done;
            }
            continue;
        }
        if (!drain_tx(client_fd, &tx, &reason)) {
            goto done;
        }

        ready = wait_for_fd(client_fd, POLLIN);
        if (ready < 0) {
            reason = shutdown_requested ? MC_LINUX_CLIENT_SHUTDOWN : MC_LINUX_CLIENT_RECV_ERROR;
            goto done;
        }
        if (ready > 0) {
            ssize_t received;

            for (;;) {
                received = recv(client_fd, in, sizeof(in), 0);
                if (received >= 0 || errno != EINTR) {
                    break;
                }
            }

            if (received == 0) {
                reason = MC_LINUX_CLIENT_PEER_CLOSE;
                goto done;
            }
            if (received < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                if (errno != ECONNRESET) {
                    perror("recv");
                }
                reason = MC_LINUX_CLIENT_RECV_ERROR;
                goto done;
            }
            if (!mc_server_receive(&server, in, (size_t)received, &tx)) {
                fprintf(stderr, "server receive failed\n");
                reason = MC_LINUX_CLIENT_PROTOCOL_RECEIVE_FAILURE;
                goto done;
            }
            /* Core already drops stale TX at reset boundaries.  Consume the
             * flag so later checks only observe new resets. */
            (void)mc_server_take_tx_reset(&server);
            if (!drain_tx(client_fd, &tx, &reason)) {
                goto done;
            }
        }
    }

    reason = MC_LINUX_CLIENT_SHUTDOWN;

done:
    free(world_arena);
    return reason;
}

static void accept_loop(int listener_fd, const mc_linux_config_t *config)
{
    while (!shutdown_requested) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int ready = wait_for_fd(listener_fd, POLLIN);
        int client_fd;

        if (ready < 0) {
            break;
        }
        if (ready == 0) {
            continue;
        }

        client_fd = accept(listener_fd, (struct sockaddr *)&peer, &peer_len);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED) {
                continue;
            }
            perror("accept");
            break;
        }
        if (!set_nonblocking(client_fd)) {
            perror("fcntl(O_NONBLOCK)");
            close(client_fd);
            continue;
        }

        fprintf(stderr, "client connected\n");
        fprintf(stderr, "client disconnected: %s\n",
                client_reason_text(run_client(client_fd, config->verbose)));
        close(client_fd);
    }
}

int main(int argc, char **argv)
{
    mc_linux_config_t config;
    int listener_fd;

    if (!parse_args(argc, argv, &config)) {
        print_usage(argv[0]);
        return 2;
    }
    if (!install_signal_handlers()) {
        perror("install_signal_handlers");
        return 1;
    }

    listener_fd = create_listener(&config.addr);
    if (listener_fd < 0) {
        return 1;
    }
    if (!set_nonblocking(listener_fd)) {
        perror("fcntl(O_NONBLOCK)");
        close(listener_fd);
        return 1;
    }

    fprintf(stderr, "listening on %s\n", config.listen_text);
    accept_loop(listener_fd, &config);
    close(listener_fd);
    fprintf(stderr, "stopped\n");
    return 0;
}
