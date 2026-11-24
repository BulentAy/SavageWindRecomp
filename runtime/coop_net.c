// Native v5 pose transport. Only the worker touches sockets or wire bytes.
#include "coop_net.h"
#include "update_crypto.h"
#include <locale.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET Sock;
#define BAD_SOCKET INVALID_SOCKET
#define net_close closesocket
#define SEND_FLAGS 0
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int Sock;
#define BAD_SOCKET (-1)
#define net_close close
#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif
#endif

enum { LINE_MAX_BYTES = 400, QUEUE_SIZE = 32, TIMEOUT_MS = 5000, POSE_MS = 100, STALE_MS = 2000 };
typedef struct {
    atomic_uint head, tail;
    CoopNetEvent data[QUEUE_SIZE];
} Queue;

static struct {
    pthread_mutex_t lifecycle;
    pthread_t thread;
    atomic_bool running, connected;
    atomic_uint sent, received;
    struct sockaddr_in address;
    char password[1025];
    Queue tx, rx;
} net = {.lifecycle = PTHREAD_MUTEX_INITIALIZER};

static unsigned room(Queue *q) {
    unsigned head = atomic_load_explicit(&q->head, memory_order_relaxed);
    return QUEUE_SIZE - (head - atomic_load_explicit(&q->tail, memory_order_acquire));
}

static void enqueue(Queue *q, const CoopNetEvent *event) {
    unsigned head = atomic_load_explicit(&q->head, memory_order_relaxed);
    q->data[head % QUEUE_SIZE] = *event;
    atomic_store_explicit(&q->head, head + 1, memory_order_release);
}

static bool dequeue(Queue *q, CoopNetEvent *event) {
    unsigned tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    if (tail == atomic_load_explicit(&q->head, memory_order_acquire)) return false;
    *event = q->data[tail % QUEUE_SIZE];
    atomic_store_explicit(&q->tail, tail + 1, memory_order_release);
    return true;
}

static uint64_t now_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
#endif
}

bool coop_net_connected(void) { return atomic_load(&net.connected); }

void coop_net_counts(unsigned *sent, unsigned *received) {
    *sent = atomic_load(&net.sent);
    *received = atomic_load(&net.received);
}

bool coop_net_push(const float m[12]) {
    if (!atomic_load(&net.running) || !room(&net.tx)) return false;
    CoopNetEvent event = {.received_ms = now_ms()};
    memcpy(event.m, m, sizeof event.m);
    enqueue(&net.tx, &event);
    return true;
}

bool coop_net_pop(CoopNetEvent *event) { return dequeue(&net.rx, event); }

static bool digits(const char *s) { return *s && s[strspn(s, "0123456789")] == 0; }

static uint32_t id_of(const char *s) {
    if (!digits(s) || strlen(s) > 10) return 0;
    unsigned long long v = strtoull(s, NULL, 10);
    return v <= UINT32_MAX ? (uint32_t)v : 0;
}

static bool valid_pose(const float m[12]) {
    for (int i = 0; i < 12; i++)
        if (!(fabsf(m[i]) <= (i % 4 == 3 ? 100000.0f : 1.1f))) return false;
    for (int i = 0; i < 12; i += 4) {
        for (int j = 0; j <= i; j += 4) {
            float dot = m[i]*m[j] + m[i+1]*m[j+1] + m[i+2]*m[j+2];
            if (fabsf(dot - (i == j)) > 0.25f) return false;
        }
    }
    return m[0]*(m[5]*m[10] - m[6]*m[9]) + m[1]*(m[6]*m[8] - m[4]*m[10]) +
           m[2]*(m[4]*m[9] - m[5]*m[8]) > 0;
}

typedef struct {
    enum { CONNECTING, HELLO, WELCOME, ONLINE } phase;
    uint32_t self, peer;
    bool present;
    uint64_t deadline, last_pose, next_tx, tx_deadline;
    unsigned used, sent, pending;
    char line[LINE_MAX_BYTES + 1], out[LINE_MAX_BYTES + 2];
#ifdef _WIN32
    _locale_t numeric;
#endif
} Session;

static void leave(Session *s, uint64_t now) {
    if (!s->present) return;
    CoopNetEvent event = {.type = COOP_NET_LEFT, .id = s->peer, .received_ms = now};
    enqueue(&net.rx, &event);
    s->present = false;
}

static void auth(Session *s, const char *challenge) {
    static const char hex[] = "0123456789abcdef";
    UcSha256 sha;
    uint8_t digest[32];
    uc_sha256_init(&sha);
    uc_sha256_update(&sha, challenge, 16);
    uc_sha256_update(&sha, net.password, strlen(net.password));
    uc_sha256_final(&sha, digest);
    const char prefix[] = "VER 5\nAUTH ";
    unsigned n = sizeof prefix - 1;
    memcpy(s->out, prefix, n);
    for (unsigned i = 0; i < sizeof digest; i++) {
        s->out[n++] = hex[digest[i] >> 4];
        s->out[n++] = hex[digest[i] & 15];
    }
    s->out[n++] = '\n';
    s->sent = 0;
    s->pending = n;
}

static bool record(Session *s, uint64_t now) {
    char *f[15], *p = s->line;
    const char *ws = " \t\v\f\r";
    unsigned n = 0;
    while (*(p += strspn(p, ws))) {
        if (n == sizeof f / sizeof *f) return s->phase == ONLINE;
        f[n++] = p;
        p += strcspn(p, ws);
        if (*p) *p++ = 0;
    }
    if (s->phase == HELLO) {
        if (n != 4 || strcmp(f[0], "HELLO") || !(s->self = id_of(f[1])) || strcmp(f[2], "5") ||
            strlen(f[3]) != 16 || strspn(f[3], "0123456789abcdefABCDEF") != 16) return false;
        auth(s, f[3]);
        s->phase = WELCOME;
        return true;
    }
    if (s->phase == WELCOME) {
        if (n != 1 || strcmp(f[0], "WELCOME") || s->pending) return false;
        s->phase = ONLINE;
        atomic_store(&net.connected, true);
        fprintf(stderr, "[coop] WELCOME id=%u\n", s->self);
        return true;
    }
    if (n == 2 && !strcmp(f[0], "BYE") && s->peer && id_of(f[1]) == s->peer) {
        leave(s, now);
        s->peer = 0;
    }
    if ((n != 14 && n != 15) || strcmp(f[0], "MTX")) return true;
    uint32_t id = id_of(f[1]);
    // shortcut: one remote at a time, replace with a peer table for multiple actors.
    if (!id || id == s->self || (s->peer && s->peer != id)) return true;
    if (n == 15) {
        const char *a = f[14];
        if (!digits(a + (*a == '+' || *a == '-'))) return true;
        long long v = strtoll(a, NULL, 10);
        if (v < INT32_MIN || v > INT32_MAX) return true;
    }
    CoopNetEvent event = {.type = COOP_NET_POSE, .id = id, .received_ms = now};
    for (unsigned i = 0; i < 12; i++) {
        const char *num = f[i + 2];
        if (num[strspn(num, "0123456789.eE+-")]) return true;
        char *end;
#ifdef _WIN32
        double v = _strtod_l(num, &end, s->numeric);
#else
        double v = strtod(num, &end);
#endif
        if (*end || end == num || !(fabs(v) <= (i % 4 == 3 ? 100000.0 : 1.1))) return true;
        event.m[i] = (float)v;
    }
    if (!valid_pose(event.m)) return true;
    s->last_pose = now;
    // Reserve one slot for LEFT even when the consumer stops draining poses.
    if (room(&net.rx) < (s->present ? 2u : 3u)) return true;
    s->peer = id;
    if (!s->present) {
        CoopNetEvent joined = {.type = COOP_NET_JOIN, .id = id, .received_ms = now};
        enqueue(&net.rx, &joined);
        s->present = true;
    }
    enqueue(&net.rx, &event);
    atomic_fetch_add(&net.received, 1);
    return true;
}

static bool retryable(void) {
#ifdef _WIN32
    int err = WSAGetLastError();
    return err == WSAEWOULDBLOCK || err == WSAEINTR;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == EINPROGRESS;
#endif
}

static Sock open_socket(void) {
    Sock fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == BAD_SOCKET) return fd;
#ifdef _WIN32
    u_long on = 1;
    bool ok = ioctlsocket(fd, FIONBIO, &on) == 0;
#else
    bool ok = fcntl(fd, F_SETFL, O_NONBLOCK) == 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
#ifdef SO_NOSIGPIPE
    int on = 1;
    ok = ok && setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on) == 0;
#endif
#endif
    if (ok && (!connect(fd, (struct sockaddr *)&net.address, sizeof net.address) || retryable())) return fd;
    net_close(fd);
    return BAD_SOCKET;
}

static int check_connect(Sock fd) {
#ifdef _WIN32
    fd_set writable, failed;
    FD_ZERO(&writable);
    FD_ZERO(&failed);
    FD_SET(fd, &writable);
    FD_SET(fd, &failed);
    struct timeval zero = {0};
    int ready = select(0, NULL, &writable, &failed, &zero);
    int size = sizeof(int);
#else
    struct pollfd p = {.fd = fd, .events = POLLOUT};
    int ready = poll(&p, 1, 0);
    socklen_t size = sizeof(int);
#endif
    if (ready <= 0) return ready == 0 || retryable() ? 0 : -1;
    int error = 0;
    return getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&error, &size) == 0 && !error ? 1 : -1;
}

static bool step(Session *s, Sock fd, uint64_t now, CoopNetEvent *local) {
    if (s->phase != ONLINE && now >= s->deadline) return false;
    if (s->phase == CONNECTING) {
        int status = check_connect(fd);
        if (status <= 0) return status == 0;
        s->phase = HELLO;
    }
    if (s->present && now - s->last_pose >= STALE_MS) leave(s, now);
    if (s->phase == ONLINE && !s->pending && now >= s->next_tx && local->received_ms &&
        now - local->received_ms <= STALE_MS && valid_pose(local->m)) {
        const float *m = local->m;
        const char *fmt = "MTX %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n";
#define POSE_ARGS m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]
#ifdef _WIN32
        int len = _snprintf_l(s->out, sizeof s->out, fmt, s->numeric, POSE_ARGS);
#else
        int len = snprintf(s->out, sizeof s->out, fmt, POSE_ARGS);
#endif
#undef POSE_ARGS
        if (len <= 0 || len > LINE_MAX_BYTES + 1) return false;
        s->sent = 0;
        s->pending = (unsigned)len;
        s->next_tx = now + POSE_MS;
        atomic_fetch_add(&net.sent, 1);
        s->tx_deadline = now + TIMEOUT_MS;
        local->received_ms = 0;
    }
    if (s->pending) {
        if (s->phase == ONLINE && now >= s->tx_deadline) return false;
        int sent = send(fd, s->out + s->sent, s->pending - s->sent, SEND_FLAGS);
        if (sent > 0) {
            s->sent += (unsigned)sent;
            if (s->sent == s->pending) s->pending = 0;
        } else if (!sent || !retryable()) return false;
    }
    char buf[2048];
    int len = recv(fd, buf, sizeof buf, 0);
    if (len <= 0) return len < 0 && retryable();
    for (int i = 0; i < len; i++) {
        unsigned char ch = buf[i];
        if (ch == '\n') {
            s->line[s->used] = 0;
            s->used = 0;
            if (!record(s, now)) return false;
        } else {
            if (s->used == LINE_MAX_BYTES || (!(ch >= 32 && ch <= 126) &&
                ch != '\t' && ch != '\v' && ch != '\f' && ch != '\r')) return false;
            s->line[s->used++] = (char)ch;
        }
    }
    return true;
}

static void *worker(void *unused) {
    (void)unused;
#ifdef _WIN32
    _locale_t numeric = _create_locale(LC_NUMERIC, "C");
#else
    locale_t numeric = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
#endif
    if (!numeric) { fprintf(stderr, "[coop] cannot initialize wire number locale\n"); return NULL; }
#ifndef _WIN32
    uselocale(numeric);
#endif
    unsigned backoff = 250;
    uint64_t retry_at = 0;
    Sock fd = BAD_SOCKET;
    Session session = {0};
    CoopNetEvent local = {0};
    while (atomic_load(&net.running)) {
        uint64_t now = now_ms();
        for (unsigned i = 0; i < QUEUE_SIZE && dequeue(&net.tx, &local); i++) {}
        if (fd == BAD_SOCKET && now >= retry_at) {
            fd = open_socket();
            session = (Session){.deadline = now + TIMEOUT_MS
#ifdef _WIN32
                                , .numeric = numeric
#endif
            };
            retry_at = now + backoff;
            if (backoff < 4000) backoff *= 2;
        }
        if (fd != BAD_SOCKET && !step(&session, fd, now, &local)) {
            atomic_store(&net.connected, false);
            leave(&session, now);
            net_close(fd);
            fd = BAD_SOCKET;
            retry_at = now + backoff;
            local.received_ms = 0;
        } else if (session.phase == ONLINE && fd != BAD_SOCKET) backoff = 250;
#ifdef _WIN32
        Sleep(10);
#else
        nanosleep(&(struct timespec){.tv_nsec = 10000000}, NULL);
#endif
    }
    if (fd != BAD_SOCKET) net_close(fd);
    atomic_store(&net.connected, false);
#ifdef _WIN32
    _free_locale(numeric);
#else
    uselocale(LC_GLOBAL_LOCALE);
    freelocale(numeric);
#endif
    return NULL;
}

bool coop_net_start(void) {
    const char *endpoint = getenv("SAVAGEWIND_COOP_ENDPOINT");
    if (!endpoint || !*endpoint) return false;
    pthread_mutex_lock(&net.lifecycle);
    bool ok = atomic_load(&net.running);
    if (ok) goto done;
#ifdef _WIN32
    // The Windows narrow environment is not necessarily UTF-8.
    wchar_t wide[sizeof net.password];
    DWORD n = GetEnvironmentVariableW(L"SAVAGEWIND_COOP_PASSWORD", wide, sizeof wide / sizeof *wide);
    if (!n || n >= sizeof wide / sizeof *wide ||
        !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, net.password, sizeof net.password, NULL, NULL)) {
#else
    const char *password = getenv("SAVAGEWIND_COOP_PASSWORD");
    if (!password || !*password || strlen(password) >= sizeof net.password) {
#endif
        fprintf(stderr, "[coop] networking needs SAVAGEWIND_COOP_PASSWORD (1..1024 UTF-8 bytes)\n");
        goto done;
    }
#ifndef _WIN32
    strcpy(net.password, password);
#endif
    char host[16];
    size_t size = strcspn(endpoint, ":");
    if (!size || size >= sizeof host || endpoint[size] != ':' || !digits(endpoint + size + 1)) goto bad_endpoint;
    unsigned long port = strtoul(endpoint + size + 1, NULL, 10);
    memcpy(host, endpoint, size);
    host[size] = 0;
    net.address = (struct sockaddr_in){.sin_family = AF_INET};
    if (port < 1 || port > 65535 || inet_pton(AF_INET, host, &net.address.sin_addr) != 1) goto bad_endpoint;
    net.address.sin_port = htons((uint16_t)port);
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) goto bad_worker;
#endif
    atomic_store(&net.tx.head, 0);
    atomic_store(&net.tx.tail, 0);
    atomic_store(&net.rx.head, 0);
    atomic_store(&net.rx.tail, 0);
    atomic_store(&net.sent, 0);
    atomic_store(&net.received, 0);
    atomic_store(&net.running, true);
    if (!pthread_create(&net.thread, NULL, worker, NULL)) { ok = true; goto done; }
    atomic_store(&net.running, false);
#ifdef _WIN32
    WSACleanup();
bad_worker:
#endif
    fprintf(stderr, "[coop] cannot start network worker\n");
    goto done;
bad_endpoint:
    fprintf(stderr, "[coop] invalid SAVAGEWIND_COOP_ENDPOINT; use numeric IPv4:port\n");
done:
    if (!ok) memset(net.password, 0, sizeof net.password);
    pthread_mutex_unlock(&net.lifecycle);
    return ok;
}

void coop_net_stop(void) {
    pthread_mutex_lock(&net.lifecycle);
    if (atomic_exchange(&net.running, false)) {
        pthread_join(net.thread, NULL);
#ifdef _WIN32
        WSACleanup();
#endif
        memset(net.password, 0, sizeof net.password);
    }
    pthread_mutex_unlock(&net.lifecycle);
}
