/*
 * appsandbox-agent -- Guest-side macOS LaunchDaemon for AppSandbox.
 *
 * Mirrors the Windows agent (tools/agent/agent.c): listens on a
 * virtio-vsock socket for the host to connect, sends "hello" on
 * accept, then periodic "heartbeat" messages every 5s. Processes
 * line-protocol commands with optional sequence tags (SEQ:cmd).
 *
 * Port 1 matches the low 16 bits of the Windows service GUID
 * (a5b0cafe-0001-...) so the "service identity" is symmetric.
 */

#import <Foundation/Foundation.h>
#import "clipboard_xpc.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <signal.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#if __has_include(<sys/vsock.h>)
#include <sys/vsock.h>
#else
/* Fallback for older SDKs — shouldn't trigger on macOS 13+. */
#define AF_VSOCK 40
#define VMADDR_CID_ANY ((unsigned int)-1U)
#define VMADDR_CID_HOST 2
struct sockaddr_vm {
    unsigned char   svm_len;
    sa_family_t     svm_family;
    unsigned short  svm_reserved1;
    unsigned int    svm_port;
    unsigned int    svm_cid;
    unsigned char   svm_zero[sizeof(struct sockaddr) -
                             sizeof(unsigned char) -
                             sizeof(sa_family_t) -
                             sizeof(unsigned short) -
                             sizeof(unsigned int) -
                             sizeof(unsigned int)];
};
#endif

#define AGENT_VSOCK_PORT        1
#define SSH_PROXY_VSOCK_PORT    7
#define CLIP_VSOCK_PORT         5
#define HEARTBEAT_INTERVAL_MS   5000
#define RECV_SEND_TIMEOUT_SEC   10
#define SSH_RELAY_BUF           8192
#define LOG_PATH                "/var/log/appsandbox-agent.log"

static int g_listen_sock = -1;
static int g_client_sock = -1;
static volatile sig_atomic_t g_stop = 0;

static int g_ssh_listen_sock = -1;
static pthread_t g_ssh_proxy_thread;
static volatile int g_ssh_proxy_running = 0;

static int g_clip_listen_sock = -1;
static pthread_t g_clip_accept_thread;
static volatile int g_clip_accept_running = 0;

static void agent_log(const char *fmt, ...) {
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] ",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

static int send_line(int s, const char *msg) {
    size_t len = strlen(msg);
    ssize_t n = send(s, msg, len, 0);
    if (n <= 0) return (int)n;
    return (int)send(s, "\n", 1, 0);
}

static int recv_line(int s, char *buf, int buf_size) {
    int pos = 0;
    while (pos < buf_size - 1) {
        char c;
        ssize_t n = recv(s, &c, 1, 0);
        if (n <= 0) return (int)n;
        if (c == '\n') break;
        if (c != '\r') buf[pos++] = c;
    }
    buf[pos] = '\0';
    return pos;
}

static void send_reply(int s, const char *tag, const char *msg) {
    if (tag && tag[0]) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s%s", tag, msg);
        send_line(s, buf);
    } else {
        send_line(s, msg);
    }
}

/* ---- SSH proxy: vsock port 7 → 127.0.0.1:22 ----
 *
 * Mirrors the Windows agent's ssh_proxy_thread (agent.c). A separate
 * vsock listener on port 7 accepts connections from the host-side
 * VmSshProxyMac, then relays bidirectional bytes to the local sshd
 * on 127.0.0.1:22. One relay pthread per connection. */

typedef struct {
    int vsock_fd;
    int tcp_fd;
} SshRelayCtx;

static void *ssh_relay_thread(void *arg) {
    SshRelayCtx *ctx = (SshRelayCtx *)arg;
    char buf[SSH_RELAY_BUF];

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ctx->vsock_fd, &rfds);
        FD_SET(ctx->tcp_fd, &rfds);
        int maxfd = ctx->vsock_fd > ctx->tcp_fd ? ctx->vsock_fd : ctx->tcp_fd;
        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        int n = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) continue;

        if (FD_ISSET(ctx->vsock_fd, &rfds)) {
            ssize_t r = recv(ctx->vsock_fd, buf, SSH_RELAY_BUF, 0);
            if (r <= 0) break;
            if (send(ctx->tcp_fd, buf, r, 0) != r) break;
        }
        if (FD_ISSET(ctx->tcp_fd, &rfds)) {
            ssize_t r = recv(ctx->tcp_fd, buf, SSH_RELAY_BUF, 0);
            if (r <= 0) break;
            if (send(ctx->vsock_fd, buf, r, 0) != r) break;
        }
    }

    close(ctx->vsock_fd);
    close(ctx->tcp_fd);
    free(ctx);
    return NULL;
}

static int connect_to_local_sshd(void) {
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port   = htons(22);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(s); return -1;
    }
    return s;
}

static void *ssh_proxy_thread_main(void *arg) {
    (void)arg;

    int ls = socket(AF_VSOCK, SOCK_STREAM, 0);
    if (ls < 0) {
        agent_log("SSH proxy: socket(AF_VSOCK) failed: %s", strerror(errno));
        return NULL;
    }
    struct sockaddr_vm addr;
    memset(&addr, 0, sizeof(addr));
    addr.svm_len    = sizeof(addr);
    addr.svm_family = AF_VSOCK;
    addr.svm_cid    = VMADDR_CID_ANY;
    addr.svm_port   = SSH_PROXY_VSOCK_PORT;
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        agent_log("SSH proxy: bind failed: %s", strerror(errno));
        close(ls); return NULL;
    }
    if (listen(ls, 4) != 0) {
        agent_log("SSH proxy: listen failed: %s", strerror(errno));
        close(ls); return NULL;
    }
    g_ssh_listen_sock = ls;
    agent_log("SSH proxy: listening on AF_VSOCK port %d", SSH_PROXY_VSOCK_PORT);

    while (g_ssh_proxy_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ls, &rfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int n = select(ls + 1, &rfds, NULL, NULL, &tv);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) continue;

        int vsock_client = accept(ls, NULL, NULL);
        if (vsock_client < 0) continue;

        int tcp_client = connect_to_local_sshd();
        if (tcp_client < 0) {
            agent_log("SSH proxy: connect localhost:22 failed: %s", strerror(errno));
            close(vsock_client);
            continue;
        }

        SshRelayCtx *ctx = malloc(sizeof(SshRelayCtx));
        if (!ctx) {
            close(vsock_client); close(tcp_client); continue;
        }
        ctx->vsock_fd = vsock_client;
        ctx->tcp_fd   = tcp_client;
        pthread_t t;
        if (pthread_create(&t, NULL, ssh_relay_thread, ctx) != 0) {
            agent_log("SSH proxy: pthread_create failed");
            close(vsock_client); close(tcp_client); free(ctx);
            continue;
        }
        pthread_detach(t);  /* relay cleans itself up */
    }

    close(ls);
    g_ssh_listen_sock = -1;
    agent_log("SSH proxy: stopped.");
    return NULL;
}

static void start_ssh_proxy(void) {
    if (g_ssh_proxy_running) return;
    g_ssh_proxy_running = 1;
    if (pthread_create(&g_ssh_proxy_thread, NULL, ssh_proxy_thread_main, NULL) != 0) {
        g_ssh_proxy_running = 0;
        agent_log("SSH proxy: failed to spawn thread");
    }
}

static void stop_ssh_proxy(void) {
    if (!g_ssh_proxy_running) return;
    g_ssh_proxy_running = 0;
    if (g_ssh_listen_sock >= 0) {
        shutdown(g_ssh_listen_sock, SHUT_RDWR);
        close(g_ssh_listen_sock);
        g_ssh_listen_sock = -1;
    }
    pthread_join(g_ssh_proxy_thread, NULL);
}

/* ---- Clipboard: vsock :5 → XPC hand-off to the user helper ----
 *
 * We (root) bind vsock :5 because AF_VSOCK bind requires root on Darwin.
 * The helper (appsandbox-clipboard, LaunchAgent in the Aqua session) owns
 * NSPasteboard access. When the helper connects to our Mach service we
 * stash its remote object proxy; each accepted vsock connection is then
 * passed to the helper as an NSFileHandle via -takeHostConnection:. This
 * is the Mac-native equivalent of the Windows SYSTEM-service + user-token
 * split for the :0005 clipboard channel. */

@interface AsbClipboardXPC : NSObject <NSXPCListenerDelegate, AppSandboxClipboardService>
@property (atomic, strong, nullable) id<AppSandboxClipboardCallback> helper;
@end

static NSXPCListener *g_clip_xpc_listener;
static AsbClipboardXPC *g_clip_xpc;

@implementation AsbClipboardXPC

- (BOOL)listener:(NSXPCListener *)listener
        shouldAcceptNewConnection:(NSXPCConnection *)conn {
    conn.exportedInterface = [NSXPCInterface interfaceWithProtocol:
        @protocol(AppSandboxClipboardService)];
    conn.exportedObject = self;
    conn.remoteObjectInterface = [NSXPCInterface interfaceWithProtocol:
        @protocol(AppSandboxClipboardCallback)];

    __weak AsbClipboardXPC *ws = self;
    conn.invalidationHandler = ^{
        agent_log("Clipboard XPC: helper disconnected.");
        ws.helper = nil;
    };
    conn.interruptionHandler = ^{
        agent_log("Clipboard XPC: helper interrupted.");
        ws.helper = nil;
    };

    self.helper = [conn remoteObjectProxyWithErrorHandler:^(NSError *err) {
        agent_log("Clipboard XPC: proxy error: %s",
                  err.localizedDescription.UTF8String ?: "(nil)");
    }];
    [conn resume];
    agent_log("Clipboard XPC: helper connected.");
    return YES;
}

- (void)helperReady {
    agent_log("Clipboard XPC: helper reports ready.");
}

@end

static void start_clip_xpc_listener(void) {
    if (g_clip_xpc_listener) return;
    g_clip_xpc = [AsbClipboardXPC new];
    g_clip_xpc_listener = [[NSXPCListener alloc]
        initWithMachServiceName:CLIPBOARD_XPC_MACH_SERVICE];
    g_clip_xpc_listener.delegate = g_clip_xpc;
    [g_clip_xpc_listener resume];
    agent_log("Clipboard XPC: listening on Mach service %s",
              CLIPBOARD_XPC_MACH_SERVICE.UTF8String);
}

static void *clip_accept_thread_main(void *arg) {
    (void)arg;
  @autoreleasepool {
    int ls = socket(AF_VSOCK, SOCK_STREAM, 0);
    if (ls < 0) {
        agent_log("Clipboard: socket(AF_VSOCK) failed: %s", strerror(errno));
        return NULL;
    }
    struct sockaddr_vm addr;
    memset(&addr, 0, sizeof(addr));
    addr.svm_len    = sizeof(addr);
    addr.svm_family = AF_VSOCK;
    addr.svm_cid    = VMADDR_CID_ANY;
    addr.svm_port   = CLIP_VSOCK_PORT;
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        agent_log("Clipboard: bind :%u failed: %s",
                  CLIP_VSOCK_PORT, strerror(errno));
        close(ls); return NULL;
    }
    if (listen(ls, 4) != 0) {
        agent_log("Clipboard: listen failed: %s", strerror(errno));
        close(ls); return NULL;
    }
    g_clip_listen_sock = ls;
    agent_log("Clipboard: vsock :%u listening", CLIP_VSOCK_PORT);

    while (g_clip_accept_running) {
      @autoreleasepool {
        fd_set r;
        FD_ZERO(&r); FD_SET(ls, &r);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int n = select(ls + 1, &r, NULL, NULL, &tv);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) continue;

        int host = accept(ls, NULL, NULL);
        if (host < 0) continue;

        /* Wait briefly for the helper to register over XPC if it hasn't yet. */
        id<AppSandboxClipboardCallback> helper = g_clip_xpc.helper;
        for (int i = 0; i < 50 && !helper && g_clip_accept_running; i++) {
            usleep(100 * 1000);
            helper = g_clip_xpc.helper;
        }
        if (!helper) {
            agent_log("Clipboard: no helper registered, dropping connection");
            close(host);
            continue;
        }

        /* NSFileHandle passes the fd across XPC via xpc_fd_create; the
         * helper ends up with its own kernel-dup. closeOnDealloc closes
         * our copy once the XPC encoding completes. */
        NSFileHandle *fh = [[NSFileHandle alloc]
            initWithFileDescriptor:host closeOnDealloc:YES];
        agent_log("Clipboard: handing fd %d to helper", host);
        [helper takeHostConnection:fh];
      }
    }
    close(ls);
    g_clip_listen_sock = -1;
    agent_log("Clipboard: accept loop stopped.");
  }
    return NULL;
}

static void start_clip_accept(void) {
    if (g_clip_accept_running) return;
    g_clip_accept_running = 1;
    if (pthread_create(&g_clip_accept_thread, NULL,
                       clip_accept_thread_main, NULL) != 0) {
        g_clip_accept_running = 0;
        agent_log("Clipboard: pthread_create failed");
    }
}

static void stop_clip_accept(void) {
    if (!g_clip_accept_running) return;
    g_clip_accept_running = 0;
    if (g_clip_listen_sock >= 0) {
        shutdown(g_clip_listen_sock, SHUT_RDWR);
        close(g_clip_listen_sock);
        g_clip_listen_sock = -1;
    }
    pthread_join(g_clip_accept_thread, NULL);
}

/* sshd is flipped on offline by the host-side stage (disabled.plist
 * override), so by the time we handle ssh_enable the daemon is already
 * listening — just start the proxy and answer ready. */
static void handle_ssh_enable(int client, const char *tag) {
    start_ssh_proxy();
    send_reply(client, tag, "ssh_ready");
}

static void handle_client(int client) {
    char buf[512];

    agent_log("Client connected.");
    g_client_sock = client;

    struct timeval tv = { .tv_sec = RECV_SEND_TIMEOUT_SEC, .tv_usec = 0 };
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (send_line(client, "hello") <= 0) {
        agent_log("Failed to send hello.");
        goto done;
    }

    /* Report our non-loopback IPv4 interface addresses so the host knows
     * where to SSH to. The guest gets its IP via DHCP from the host's
     * vmnet daemon; we don't know it until the NIC is up. */
    {
        struct ifaddrs *head = NULL;
        if (getifaddrs(&head) == 0) {
            for (struct ifaddrs *ifa = head; ifa; ifa = ifa->ifa_next) {
                if (!ifa->ifa_addr) continue;
                if (ifa->ifa_addr->sa_family != AF_INET) continue;
                if (ifa->ifa_flags & IFF_LOOPBACK) continue;
                if (!(ifa->ifa_flags & IFF_UP)) continue;
                if (strncmp(ifa->ifa_name, "en", 2) != 0) continue;

                char host[NI_MAXHOST];
                if (getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in),
                                host, sizeof(host), NULL, 0,
                                NI_NUMERICHOST) == 0) {
                    char line[128];
                    snprintf(line, sizeof(line), "ip:%s:%s",
                             ifa->ifa_name, host);
                    send_line(client, line);
                    agent_log("Reported %s=%s", ifa->ifa_name, host);
                }
            }
            freeifaddrs(head);
        }
    }

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t last_heartbeat_ms = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    while (!g_stop) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(client, &rfds);
        struct timeval to = { .tv_sec = 1, .tv_usec = 0 };
        int ret = select(client + 1, &rfds, NULL, NULL, &to);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now_ms = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        if (now_ms - last_heartbeat_ms >= HEARTBEAT_INTERVAL_MS) {
            if (send_line(client, "heartbeat") <= 0) {
                agent_log("Heartbeat send failed, client disconnected.");
                break;
            }
            last_heartbeat_ms = now_ms;
        }

        if (ret == 0) continue;

        int n = recv_line(client, buf, sizeof(buf));
        if (n <= 0) {
            agent_log("Client disconnected.");
            break;
        }

        char tag[32] = {0};
        char *cmd = buf;
        char *colon = strchr(buf, ':');
        if (colon && colon > buf && colon - buf < 16) {
            int is_seq = 1;
            for (char *p = buf; p < colon; p++) {
                if (*p < '0' || *p > '9') { is_seq = 0; break; }
            }
            if (is_seq) {
                int tlen = (int)(colon - buf + 1);
                memcpy(tag, buf, tlen);
                tag[tlen] = '\0';
                cmd = colon + 1;
            }
        }

        agent_log("Command: %s", buf);

        if (strcmp(cmd, "ping") == 0) {
            send_reply(client, tag, "ok");
        } else if (strcmp(cmd, "ssh_enable") == 0) {
            handle_ssh_enable(client, tag);
        } else {
            send_reply(client, tag, "error:unknown_command");
        }
    }

done:
    close(client);
    g_client_sock = -1;
}

static int open_listener(void) {
    int s = socket(AF_VSOCK, SOCK_STREAM, 0);
    if (s < 0) {
        agent_log("socket(AF_VSOCK) failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_vm addr;
    memset(&addr, 0, sizeof(addr));
    addr.svm_len = sizeof(addr);
    addr.svm_family = AF_VSOCK;
    addr.svm_cid = VMADDR_CID_ANY;
    addr.svm_port = AGENT_VSOCK_PORT;

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        agent_log("bind failed: %s", strerror(errno));
        close(s);
        return -1;
    }
    if (listen(s, 4) != 0) {
        agent_log("listen failed: %s", strerror(errno));
        close(s);
        return -1;
    }

    agent_log("Listening on AF_VSOCK port %d.", AGENT_VSOCK_PORT);
    return s;
}

static void handle_signal(int sig) {
    (void)sig;
    g_stop = 1;
    g_ssh_proxy_running = 0;
    g_clip_accept_running = 0;
    if (g_listen_sock >= 0) {
        shutdown(g_listen_sock, SHUT_RDWR);
        close(g_listen_sock);
        g_listen_sock = -1;
    }
    if (g_ssh_listen_sock >= 0) {
        shutdown(g_ssh_listen_sock, SHUT_RDWR);
        close(g_ssh_listen_sock);
        g_ssh_listen_sock = -1;
    }
    if (g_clip_listen_sock >= 0) {
        shutdown(g_clip_listen_sock, SHUT_RDWR);
        close(g_clip_listen_sock);
        g_clip_listen_sock = -1;
    }
    if (g_client_sock >= 0) {
        shutdown(g_client_sock, SHUT_RDWR);
    }
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    struct sigaction sa = {0};
    sa.sa_handler = handle_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    agent_log("Service starting (pid %d).", getpid());

    /* Clipboard: we (root) own the vsock bind; the user LaunchAgent
     * receives accepted fds over our XPC Mach service and speaks the
     * pasteboard protocol directly on them. */
    start_clip_xpc_listener();
    start_clip_accept();

    while (!g_stop) {
        g_listen_sock = open_listener();
        if (g_listen_sock < 0) {
            sleep(3);
            continue;
        }

        while (!g_stop) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(g_listen_sock, &rfds);
            struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
            int ret = select(g_listen_sock + 1, &rfds, NULL, NULL, &tv);
            if (ret < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (ret == 0) continue;
            int client = accept(g_listen_sock, NULL, NULL);
            if (client < 0) continue;
            handle_client(client);
        }

        if (g_listen_sock >= 0) {
            close(g_listen_sock);
            g_listen_sock = -1;
        }
    }

    agent_log("Service stopped.");
    return 0;
}
