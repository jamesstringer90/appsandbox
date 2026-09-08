/*
 * appsandbox-agent.c — Linux guest agent for App Sandbox.
 *
 * Listens on AF_VSOCK port 1 for the host control channel. Speaks the
 * line-based text protocol from docs/linux-idd-implementation-plan.md
 * section 3 (Port 1 — agent). Compatible with the existing host code in
 * src/backend_win/vm_agent.c.
 *
 * Single-client at a time. On accept: send "hello", start a heartbeat
 * thread emitting "heartbeat" every 5s, run a command dispatch loop on
 * the main thread. On client disconnect, join heartbeat thread and accept
 * the next connection.
 *
 * Build:    gcc -O2 -Wall -Wextra -pthread -D_GNU_SOURCE \
 *               -o appsandbox-agent appsandbox-agent.c
 * Install:  /usr/local/bin/appsandbox-agent
 * Run as:   systemd system service (root).
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <netinet/in.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <linux/vm_sockets.h>

/* ---- Constants ---- */

#define AGENT_PORT              1
#define HEARTBEAT_INTERVAL_SEC  5
#define LINE_BUF_MAX            4096
#define REPLY_MAX               256

/* ---- Global state for the currently-active client connection ---- */

static volatile sig_atomic_t g_stop      = 0;
static volatile int          g_client_fd = -1;
static pthread_mutex_t       g_send_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- Logging (timestamped, goes to stderr → systemd journal) ---- */

static void agent_log(const char *fmt, ...)
{
    char    ts[32];
    time_t  t = time(NULL);
    struct tm tm;
    va_list ap;

    localtime_r(&t, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(stderr, "[%s] ", ts);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* ---- Line I/O ---- */

/* Send a line + '\n'. Thread-safe (heartbeat thread and command-reply
 * code share the write end of the same socket). */
static int send_line(int fd, const char *line)
{
    size_t  len = strlen(line);
    int     ok;
    ssize_t n1, n2;

    if (fd < 0) return -1;

    pthread_mutex_lock(&g_send_lock);
    n1 = write(fd, line, len);
    n2 = (n1 == (ssize_t)len) ? write(fd, "\n", 1) : -1;
    ok = (n1 == (ssize_t)len && n2 == 1);
    pthread_mutex_unlock(&g_send_lock);

    return ok ? 0 : -1;
}

/* Read one line (up to '\n') into buf. Strips '\r'. Null-terminates.
 * Returns: number of chars in buf, 0 on EOF, -1 on error. */
static int recv_line(int fd, char *buf, int max)
{
    int pos = 0;
    while (pos < max - 1) {
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n == 0) { buf[pos] = '\0'; return 0; }       /* EOF */
        if (n < 0) {
            if (errno == EINTR) continue;
            buf[pos] = '\0';
            return -1;
        }
        if (c == '\n') break;
        if (c != '\r') buf[pos++] = c;
    }
    buf[pos] = '\0';
    return pos;
}

/* ---- Subprocess helpers ---- */

/* Run a shell command synchronously, return exit code (-1 on spawn fail). */
static int run_sync(const char *cmd)
{
    pid_t pid = fork();
    int status = 0;

    if (pid < 0) return -1;
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Spawn a shell command, double-fork to detach it from us. Returns 0
 * if the intermediate fork succeeded (doesn't wait for the real child). */
static int spawn_detached(const char *cmd)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (setsid() < 0) _exit(127);
        if (fork() == 0) {
            execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    waitpid(pid, NULL, 0);  /* reap intermediate */
    return 0;
}

/* ---- Clipboard helper spawn ----
 *
 * Mirrors the Windows-agent pattern (tools/agent/agent.c
 * clipboard_monitor_thread + spawn_clipboard_in_session): the agent
 * binds the privileged vsock ports as root, then forks+execs a
 * user-context helper with the fds inherited at 3/4 (LISTEN_FDS
 * convention). A background monitor thread polls every 3s for user
 * session presence and helper liveness and respawns as needed —
 * idd_connect alone is a one-shot trigger and doesn't survive helper
 * crashes / window-server restarts. */

static pid_t           g_clipboard_helper_pid = 0;
static pthread_mutex_t g_clipboard_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       g_clipboard_monitor_thread;
static volatile int    g_clipboard_monitor_running = 0;

static int vsock_bind_listen_privileged(unsigned port)
{
    int s = socket(AF_VSOCK, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_vm sa = {0};
    sa.svm_family = AF_VSOCK;
    sa.svm_cid    = VMADDR_CID_ANY;
    sa.svm_port   = port;
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        agent_log("clipboard: bind vsock :%u failed: %s",
                  port, strerror(errno));
        close(s); return -1;
    }
    if (listen(s, 1) < 0) {
        agent_log("clipboard: listen vsock :%u failed: %s",
                  port, strerror(errno));
        close(s); return -1;
    }
    return s;
}

/* Find the active user's Mutter-XWayland auth cookie. Sets *out to a
 * NUL-terminated path on success, leaves it untouched otherwise.
 * Caller must already be in the user's uid context if checking
 * readability matters. */
static void find_mutter_xauth(uid_t uid, char *out, size_t cap)
{
    char xdg_rt[64];
    snprintf(xdg_rt, sizeof(xdg_rt), "/run/user/%u", (unsigned)uid);
    DIR *d = opendir(xdg_rt);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, ".mutter-Xwaylandauth.", 21) == 0) {
            snprintf(out, cap, "%s/%s", xdg_rt, de->d_name);
            break;
        }
    }
    closedir(d);
}

/* True if the UID looks like a regular human user we can spawn the
 * clipboard helper as — has a /home/... directory and a real login shell.
 *
 * Without this filter we'd accept system users like gdm (pw_dir=/var/lib/gdm3,
 * pw_shell=/usr/sbin/nologin). The greeter's own mutter creates a
 * /run/user/<gdm-uid>/.mutter-Xwaylandauth.* cookie for the login screen,
 * which would match the xauth probe. Spawning the clipboard helper as gdm
 * crashloops (exit 1) until the user actually logs in. The pw_dir + shell
 * checks below reject gdm regardless of its UID. */
static int uid_is_real_user(uid_t uid)
{
    struct passwd *pw = getpwuid(uid);
    if (!pw || !pw->pw_dir || !pw->pw_shell) return 0;
    if (strncmp(pw->pw_dir, "/home/", 6) != 0) return 0;
    /* Reject nologin/false. Accept anything else as a real shell. */
    const char *shell = pw->pw_shell;
    const char *base = strrchr(shell, '/');
    base = base ? base + 1 : shell;
    if (strcmp(base, "nologin") == 0 || strcmp(base, "false") == 0)
        return 0;
    return 1;
}

/* Scan /run/user for the UID whose runtime dir has a Mutter XWayland auth
 * cookie AND who looks like a real user (not gdm). That's the user whose
 * graphical session we want to spawn the clipboard helper into. Returns
 * 0 (not found) or a valid uid_t.
 *
 * We can't hardcode a username — the create-VM modal lets the user pick
 * any account name. Looking for the live mutter xauth file is more robust
 * than parsing /etc/passwd because it implicitly filters for "user who
 * actually has a graphical session right now". */
static uid_t find_graphical_session_uid(void)
{
    DIR *d = opendir("/run/user");
    if (!d) return 0;
    struct dirent *de;
    uid_t found = 0;
    while ((de = readdir(d))) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        char *end = NULL;
        unsigned long v = strtoul(de->d_name, &end, 10);
        if (!end || *end != '\0' || v < 1000 || v >= 65534) continue;
        if (!uid_is_real_user((uid_t)v)) continue;
        char xauth[512] = {0};
        find_mutter_xauth((uid_t)v, xauth, sizeof(xauth));
        if (xauth[0]) { found = (uid_t)v; break; }
    }
    closedir(d);
    return found;
}

/* User session is "ready" once Mutter has dropped its XWayland auth
 * cookie. Before that, xcb_connect would fail with the no-auth-protocol
 * error we saw on early spawns. */
static int is_user_session_ready(uid_t uid)
{
    char xauth[512] = {0};
    find_mutter_xauth(uid, xauth, sizeof(xauth));
    if (xauth[0] == '\0') return 0;
    struct stat st;
    return stat(xauth, &st) == 0 && S_ISREG(st.st_mode);
}

static void kill_clipboard_helper_locked(void)
{
    if (g_clipboard_helper_pid <= 0) return;
    kill(g_clipboard_helper_pid, SIGTERM);
    int status;
    for (int i = 0; i < 10; i++) {
        if (waitpid(g_clipboard_helper_pid, &status, WNOHANG) != 0) break;
        usleep(100 * 1000);
    }
    /* If still alive after 1s, SIGKILL. */
    if (waitpid(g_clipboard_helper_pid, &status, WNOHANG) == 0) {
        kill(g_clipboard_helper_pid, SIGKILL);
        waitpid(g_clipboard_helper_pid, &status, 0);
    }
    g_clipboard_helper_pid = 0;
}

/* Spawn helper if no live one. Caller must hold g_clipboard_lock. */
static void spawn_clipboard_helper_locked(void)
{
    if (g_clipboard_helper_pid > 0) {
        int status;
        pid_t r = waitpid(g_clipboard_helper_pid, &status, WNOHANG);
        if (r == 0) return;                  /* still running */
        g_clipboard_helper_pid = 0;
    }

    uid_t uid = find_graphical_session_uid();
    if (uid == 0) {
        /* No graphical session yet — retry on next monitor tick. */
        return;
    }
    struct passwd *pw = getpwuid(uid);
    if (!pw) {
        agent_log("clipboard: getpwuid(%u) failed: %s",
                  (unsigned)uid, strerror(errno));
        return;
    }

    if (!is_user_session_ready(pw->pw_uid)) {
        /* Will retry on the next monitor tick. */
        return;
    }

    int writer_fd = vsock_bind_listen_privileged(5);
    int reader_fd = vsock_bind_listen_privileged(6);
    if (writer_fd < 0 || reader_fd < 0) {
        if (writer_fd >= 0) close(writer_fd);
        if (reader_fd >= 0) close(reader_fd);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        agent_log("clipboard: fork failed: %s", strerror(errno));
        close(writer_fd); close(reader_fd);
        return;
    }

    if (pid == 0) {
        /* Child: prepare fds at 3 and 4 (systemd LISTEN_FDS convention). */
        if (dup2(writer_fd, 3) < 0 || dup2(reader_fd, 4) < 0) {
            fprintf(stderr, "clipboard child: dup2 failed: %s\n", strerror(errno));
            _exit(127);
        }
        if (writer_fd != 3) close(writer_fd);
        if (reader_fd != 4) close(reader_fd);

        if (setgid(pw->pw_gid) < 0 ||
            initgroups(pw->pw_name, pw->pw_gid) < 0 ||
            setuid(pw->pw_uid) < 0) {
            fprintf(stderr, "clipboard child: drop privs failed: %s\n",
                    strerror(errno));
            _exit(127);
        }

        setenv("HOME", pw->pw_dir, 1);
        setenv("USER", pw->pw_name, 1);
        setenv("LOGNAME", pw->pw_name, 1);
        setenv("DISPLAY", ":0", 1);

        char xdg_rt[64];
        snprintf(xdg_rt, sizeof(xdg_rt), "/run/user/%u", (unsigned)pw->pw_uid);
        setenv("XDG_RUNTIME_DIR", xdg_rt, 1);
        setenv("WAYLAND_DISPLAY", "wayland-0", 1);

        char xauth[512] = {0};
        find_mutter_xauth(pw->pw_uid, xauth, sizeof(xauth));
        if (xauth[0] == '\0')
            snprintf(xauth, sizeof(xauth), "%s/.Xauthority", pw->pw_dir);
        setenv("XAUTHORITY", xauth, 1);

        char fds_str[16], pid_str[16];
        snprintf(fds_str, sizeof(fds_str), "%d", 2);
        snprintf(pid_str, sizeof(pid_str), "%d", (int)getpid());
        setenv("LISTEN_FDS", fds_str, 1);
        setenv("LISTEN_PID", pid_str, 1);

        execl("/usr/local/bin/appsandbox-clipboard",
              "appsandbox-clipboard", (char *)NULL);
        fprintf(stderr, "clipboard child: execl failed: %s\n", strerror(errno));
        _exit(127);
    }

    close(writer_fd);
    close(reader_fd);
    g_clipboard_helper_pid = pid;
    agent_log("clipboard helper spawned (pid=%d, uid=%u, xauth_ok=1)",
              (int)pid, (unsigned)pw->pw_uid);
}

/* Kill+respawn variant used from the idd_connect handler — guarantees a
 * fresh helper bound to a fresh socket so we don't accidentally serve
 * stale state when the host display reconnects. */
static void respawn_clipboard_helper(void)
{
    pthread_mutex_lock(&g_clipboard_lock);
    kill_clipboard_helper_locked();
    spawn_clipboard_helper_locked();
    pthread_mutex_unlock(&g_clipboard_lock);
}

/* Monitor thread: 3s polling, like tools/agent/agent.c
 * clipboard_monitor_thread. Respawns the helper if it died and spawns
 * one the first time the user session becomes ready. */
static void *clipboard_monitor_proc(void *arg)
{
    (void)arg;
    while (g_clipboard_monitor_running) {
        pthread_mutex_lock(&g_clipboard_lock);
        if (g_clipboard_helper_pid > 0) {
            int status;
            pid_t r = waitpid(g_clipboard_helper_pid, &status, WNOHANG);
            if (r > 0) {
                agent_log("clipboard helper exited (pid=%d, status=%d), "
                          "will respawn", (int)g_clipboard_helper_pid, status);
                g_clipboard_helper_pid = 0;
            }
        }
        if (g_clipboard_helper_pid == 0) {
            spawn_clipboard_helper_locked();
        }
        pthread_mutex_unlock(&g_clipboard_lock);

        for (int i = 0; i < 6 && g_clipboard_monitor_running; i++)
            usleep(500 * 1000);
    }
    return NULL;
}

static void start_clipboard_monitor(void)
{
    g_clipboard_monitor_running = 1;
    if (pthread_create(&g_clipboard_monitor_thread, NULL,
                       clipboard_monitor_proc, NULL) != 0) {
        agent_log("clipboard: failed to start monitor thread: %s",
                  strerror(errno));
        g_clipboard_monitor_running = 0;
    }
}

/* ---- Reply helper: prepends the sequence tag if present ---- */

static void send_reply(int fd, const char *tag, const char *msg)
{
    if (tag && tag[0]) {
        char buf[REPLY_MAX];
        snprintf(buf, sizeof(buf), "%s%s", tag, msg);
        send_line(fd, buf);
    } else {
        send_line(fd, msg);
    }
}

/* ---- Heartbeat thread ---- */

static void *heartbeat_thread(void *arg)
{
    (void)arg;
    int idd_last = 0;   /* last reported display-driver readiness (so we only send on change) */
    while (!g_stop) {
        /* Sleep first so the very first heartbeat is at +5s, after hello. */
        for (int s = 0; s < HEARTBEAT_INTERVAL_SEC && !g_stop; s++)
            sleep(1);
        if (g_stop) break;
        int fd = g_client_fd;
        if (fd < 0) break;          /* client disconnected */
        if (send_line(fd, "heartbeat") < 0) {
            agent_log("heartbeat: send failed, exiting thread");
            break;
        }
        /* Report display readiness to the host (the same idd_status the Windows
         * agent sends). The display is serveable once the user session is up --
         * Mutter is compositing, so asb_drm has an active framebuffer for a host
         * consumer to capture. The host latches this as the display-open gate, so
         * it never has to probe the frame channel to find out. send_line is
         * mutex-locked, so this is safe alongside the command thread's replies. */
        {
            uid_t uid = find_graphical_session_uid();
            int ready = (uid != 0 && is_user_session_ready(uid));
            if (ready != idd_last) {
                send_line(fd, ready ? "idd_status:ok" : "idd_status:not_found");
                idd_last = ready;
            }
        }
    }
    return NULL;
}

/* ---- Command handlers ---- */

/* set_ip:<ip>/<prefix>:<gw>
 *   e.g. set_ip:192.168.42.2/24:192.168.42.1
 *
 * Mirrors the Windows agent's `netsh ip set static` clobber semantics:
 *   1. Disable cloud-init's network module so it stops re-writing
 *      /etc/netplan/50-cloud-init.yaml at next boot.
 *   2. Remove every other *.yaml in /etc/netplan so the merge has only
 *      our file to consider — no coexistence games.
 *   3. Write /etc/netplan/99-appsandbox.yaml with the host-assigned
 *      address + gateway + DNS (gateway primary, 8.8.8.8 fallback —
 *      same DNS layout the Windows agent uses).
 *   4. `netplan apply` then `systemctl restart systemd-networkd` so the
 *      kernel actually drops any stale addresses from a prior config.
 *
 * Reply: "ok" on success, "error:..." on failure. */
static void handle_set_ip(int fd, const char *tag, const char *args)
{
    char ip[64], prefix[8], gw[64];
    const char *slash  = strchr(args, '/');
    const char *colon2 = slash ? strchr(slash, ':') : NULL;
    size_t ip_len, pfx_len;
    char cmd[2400];
    int n, rc;

    if (!slash || !colon2) {
        send_reply(fd, tag, "error:bad_format");
        return;
    }
    ip_len  = (size_t)(slash - args);
    pfx_len = (size_t)(colon2 - slash - 1);
    if (ip_len >= sizeof(ip) || pfx_len >= sizeof(prefix)) {
        send_reply(fd, tag, "error:bad_format");
        return;
    }
    memcpy(ip,     args,        ip_len);  ip[ip_len]   = '\0';
    memcpy(prefix, slash + 1,   pfx_len); prefix[pfx_len] = '\0';
    snprintf(gw, sizeof(gw), "%s", colon2 + 1);

    n = snprintf(cmd, sizeof(cmd),
        /* Pick the renderer by probing whether the NetworkManager service
         * is active (Ubuntu Desktop); otherwise default to systemd-networkd
         * (Server). Writing a netplan with the wrong renderer makes the
         * active one stop managing the NIC entirely (which is why "no
         * network in Ubuntu Settings" happened). */
        "RENDERER=networkd; "
        "if systemctl is-active --quiet NetworkManager; then "
        "  RENDERER=NetworkManager; "
        "fi; "
        /* Disable cloud-init's network module so it stops re-writing
         * /etc/netplan/50-cloud-init.yaml on every boot. Idempotent. */
        "mkdir -p /etc/cloud/cloud.cfg.d && "
        "printf 'network: {config: disabled}\\n' "
        "  > /etc/cloud/cloud.cfg.d/99-disable-network-config.cfg && "
        /* Remove every other netplan dropfile so merge can't reintroduce
         * a conflicting interface key (e.g. cloud-init's `enp0s5: dhcp4`
         * + our `appsbnic` both binding the same NIC). */
        "find /etc/netplan -maxdepth 1 -type f -name '*.yaml' "
        "  ! -name '99-appsandbox.yaml' -delete; "
        /* Our authoritative config. Heredoc is unquoted so $RENDERER
         * expands; nothing else in the YAML uses '$'. */
        "umask 077 && cat > /etc/netplan/99-appsandbox.yaml <<EOF\n"
        "network:\n"
        "  version: 2\n"
        "  renderer: $RENDERER\n"
        "  ethernets:\n"
        "    appsbnic:\n"
        "      match: { name: \"e*\" }\n"
        "      dhcp4: false\n"
        "      dhcp6: false\n"
        "      addresses: [\"%s/%s\"]\n"
        "      routes:\n"
        "        - to: default\n"
        "          via: %s\n"
        "      nameservers:\n"
        "        addresses: [%s, 8.8.8.8]\n"
        "EOF\n"
        /* Apply, then restart the renderer to drop stale leases / state
         * left behind by cloud-init or a previous run. netplan apply
         * alone is sometimes a no-op against a NIC that already has an
         * address the renderer hasn't released.
         *
         * IMPORTANT: reset umask BEFORE `netplan apply`. netplan's
         * systemd-networkd backend writes the generated
         * /run/systemd/network/10-netplan-*.network file via Python
         * open() with no explicit mode, so the file ends up at
         * (0666 & ~umask). With our 077 umask the generated file is 600
         * (root:systemd-network), and systemd-networkd — running as
         * `systemd-network` user, only in the systemd-network group —
         * gets "Permission denied" trying to read it, falls back to the
         * dracut catch-all `.network`, and eth0 ends up with no IP.
         * 022 → generated .network is 644 → systemd-network can read it. */
        "chmod 600 /etc/netplan/99-appsandbox.yaml && "
        "umask 022 && "
        "netplan apply 2>&1; "
        "if [ \"$RENDERER\" = NetworkManager ]; then "
        "  systemctl restart NetworkManager 2>&1; "
        "else "
        "  systemctl restart systemd-networkd 2>&1; "
        "fi; "
        /* Verify the address actually materialised. netplan apply / NM
         * reload are async — the host might try to SSH before the new
         * IP claims the wire. Poll for up to 5 seconds. */
        "for i in 1 2 3 4 5 6 7 8 9 10; do "
        "  ip -4 addr show | grep -q '%s/' && exit 0; "
        "  sleep 0.5; "
        "done; "
        "echo 'set_ip: address never appeared'; ip -4 addr show; exit 1",
        ip, prefix, gw, gw, ip);
    if (n < 0 || n >= (int)sizeof(cmd)) {
        send_reply(fd, tag, "error:cmd_too_long");
        return;
    }
    rc = run_sync(cmd);
    agent_log("set_ip: %s/%s via %s → rc=%d", ip, prefix, gw, rc);
    send_reply(fd, tag, rc == 0 ? "ok" : "error:netplan_failed");
}

/* ---- SSH proxy: AF_VSOCK :7 ↔ localhost:22 ----
 *
 * Mirror of tools/agent/agent.c's ssh_proxy_thread + ssh_relay_thread.
 * The host's vm_ssh_proxy.c connects to AF_HYPERV service GUID :0007 —
 * hcs_service_guid(os_type, 7, ...) translates that to AF_VSOCK port 7
 * for Linux VMs — so we listen there and bridge each accepted vsock
 * connection to a fresh TCP socket on 127.0.0.1:22 (sshd).
 *
 * Port 7 is privileged (<1024); the agent runs as root so the bind
 * goes through without extra capability work. Per-connection relays
 * run as detached pthreads so we don't have to track them. */

#define SSH_RELAY_BUF   8192u
#define SSH_VSOCK_PORT  7u
#define SSH_TCP_PORT    22u

static pthread_t        g_ssh_proxy_pthread;
static volatile int     g_ssh_proxy_running = 0;

typedef struct {
    int vsock_fd;
    int tcp_fd;
} ssh_relay_ctx_t;

static int ssh_send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p   += n;
        len -= (size_t)n;
    }
    return 0;
}

static void *ssh_relay_proc(void *arg)
{
    ssh_relay_ctx_t *r = (ssh_relay_ctx_t *)arg;
    uint8_t buf[SSH_RELAY_BUF];
    struct pollfd pfd[2];

    pfd[0].fd = r->vsock_fd; pfd[0].events = POLLIN;
    pfd[1].fd = r->tcp_fd;   pfd[1].events = POLLIN;

    for (;;) {
        int n = poll(pfd, 2, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (pfd[0].revents & POLLIN) {
            ssize_t m = recv(r->vsock_fd, buf, sizeof(buf), 0);
            if (m <= 0) break;
            if (ssh_send_all(r->tcp_fd, buf, (size_t)m) < 0) break;
        }
        if (pfd[1].revents & POLLIN) {
            ssize_t m = recv(r->tcp_fd, buf, sizeof(buf), 0);
            if (m <= 0) break;
            if (ssh_send_all(r->vsock_fd, buf, (size_t)m) < 0) break;
        }
        if ((pfd[0].revents | pfd[1].revents) & (POLLHUP | POLLERR | POLLNVAL))
            break;
    }
    close(r->vsock_fd);
    close(r->tcp_fd);
    free(r);
    return NULL;
}

static void *ssh_proxy_proc(void *arg)
{
    (void)arg;

    int s = socket(AF_VSOCK, SOCK_STREAM, 0);
    if (s < 0) {
        agent_log("ssh proxy: socket(AF_VSOCK) failed: %s", strerror(errno));
        return NULL;
    }
    struct sockaddr_vm sa = {0};
    sa.svm_family = AF_VSOCK;
    sa.svm_cid    = VMADDR_CID_ANY;
    sa.svm_port   = SSH_VSOCK_PORT;
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        agent_log("ssh proxy: bind vsock :%u failed: %s",
                  SSH_VSOCK_PORT, strerror(errno));
        close(s);
        return NULL;
    }
    if (listen(s, 4) < 0) {
        agent_log("ssh proxy: listen failed: %s", strerror(errno));
        close(s);
        return NULL;
    }
    agent_log("ssh proxy: listening on AF_VSOCK :%u", SSH_VSOCK_PORT);

    while (g_ssh_proxy_running) {
        struct pollfd pfd = { .fd = s, .events = POLLIN };
        int r = poll(&pfd, 1, 1000);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) continue;

        int c = accept(s, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR) continue;
            agent_log("ssh proxy: accept failed: %s", strerror(errno));
            continue;
        }

        /* Per-connection TCP socket → localhost:22. Done eagerly so we
         * surface refused/timeout immediately rather than after one
         * round of relay polling. */
        int t = socket(AF_INET, SOCK_STREAM, 0);
        if (t < 0) { close(c); continue; }
        struct sockaddr_in ta = {0};
        ta.sin_family      = AF_INET;
        ta.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ta.sin_port        = htons(SSH_TCP_PORT);
        if (connect(t, (struct sockaddr *)&ta, sizeof(ta)) < 0) {
            agent_log("ssh proxy: connect 127.0.0.1:%u failed: %s",
                      SSH_TCP_PORT, strerror(errno));
            close(t); close(c);
            continue;
        }

        ssh_relay_ctx_t *ctx = malloc(sizeof(*ctx));
        if (!ctx) { close(t); close(c); continue; }
        ctx->vsock_fd = c;
        ctx->tcp_fd   = t;

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_t rt;
        if (pthread_create(&rt, &attr, ssh_relay_proc, ctx) != 0) {
            agent_log("ssh proxy: relay pthread_create failed: %s",
                      strerror(errno));
            close(t); close(c); free(ctx);
        }
        pthread_attr_destroy(&attr);
    }

    close(s);
    agent_log("ssh proxy: stopped");
    return NULL;
}

static void start_ssh_proxy(void)
{
    if (g_ssh_proxy_running) return;
    g_ssh_proxy_running = 1;
    if (pthread_create(&g_ssh_proxy_pthread, NULL, ssh_proxy_proc, NULL) != 0) {
        agent_log("ssh proxy: pthread_create failed: %s", strerror(errno));
        g_ssh_proxy_running = 0;
    }
}

/* Find the primary interactive account (the one firstboot's useradd created):
 * a real /home user with a login shell. The username is user-chosen, so we scan
 * the passwd db rather than hardcode it. Returns 1 on success. */
static int find_primary_user(char *home, size_t hsz, uid_t *uid, gid_t *gid)
{
    struct passwd *pw;
    int found = 0;
    setpwent();
    while ((pw = getpwent())) {
        const char *sh, *b;
        if (pw->pw_uid < 1000 || pw->pw_uid >= 65534) continue;
        if (!pw->pw_dir || strncmp(pw->pw_dir, "/home/", 6) != 0) continue;
        sh = pw->pw_shell ? pw->pw_shell : "";
        b = strrchr(sh, '/'); b = b ? b + 1 : sh;
        if (strcmp(b, "nologin") == 0 || strcmp(b, "false") == 0) continue;
        snprintf(home, hsz, "%s", pw->pw_dir);
        *uid = pw->pw_uid; *gid = pw->pw_gid; found = 1;
        break;
    }
    endpwent();
    return found;
}

/* ssh_deploy_key: write the AppSandbox public key into the primary user's
 * ~/.ssh/authorized_keys (0700 dir, 0600 file, owned by the user). */
static int deploy_ssh_key(const char *pubkey)
{
    char home[256], sshdir[320], authk[400];
    uid_t uid; gid_t gid;
    FILE *f;
    if (!pubkey || !*pubkey) return 0;
    if (!find_primary_user(home, sizeof(home), &uid, &gid)) {
        agent_log("ssh key: no primary /home user found");
        return 0;
    }
    snprintf(sshdir, sizeof(sshdir), "%s/.ssh", home);
    snprintf(authk, sizeof(authk), "%s/authorized_keys", sshdir);
    mkdir(sshdir, 0700);
    f = fopen(authk, "w");
    if (!f) { agent_log("ssh key: cannot write %s: %s", authk, strerror(errno)); return 0; }
    fprintf(f, "%s\n", pubkey);
    fclose(f);
    chmod(sshdir, 0700);
    chmod(authk, 0600);
    if (chown(sshdir, uid, gid) != 0 || chown(authk, uid, gid) != 0)
        agent_log("ssh key: chown warning: %s", strerror(errno));
    agent_log("ssh key: deployed to %s", authk);
    return 1;
}

/* ssh_enable: enable+start openssh-server, then start the vsock:7 →
 * localhost:22 proxy. Reply: ssh_ready/ssh_failed. Idempotent — repeat
 * calls just confirm state. */
static void handle_ssh_enable(int fd, const char *tag)
{
    if (run_sync("systemctl is-active --quiet ssh") == 0) {
        agent_log("ssh: already running");
        start_ssh_proxy();
        send_reply(fd, tag, "ssh_ready");
        return;
    }
    int rc = run_sync("systemctl enable --now ssh 2>&1");
    if (rc == 0) {
        agent_log("ssh: enabled and started");
        start_ssh_proxy();
        send_reply(fd, tag, "ssh_ready");
    } else {
        agent_log("ssh: enable failed (rc=%d)", rc);
        send_reply(fd, tag, "ssh_failed");
    }
}

/* gpu_query_response:N — host sent N share descriptors after the header.
 *
 * Wire format per share line: "share_name|guest_path|filter\n" where
 *   share_name = "AppSandbox.HostLxssLib" or "AppSandbox.Drv.N"
 *   guest_path = Windows-format target path (we ignore it except to
 *                lift the leaf folder name for AppSandbox.Drv.N)
 *   filter     = ";"-separated filename whitelist (unused on Linux —
 *                we mount the whole share read-only instead of copying)
 *
 * For each share we open AF_VSOCK to (CID_HOST=2, port=50001), the HCS
 * Plan9 server, and hand the socket fd to the kernel's 9p driver via
 * mount(2) with "trans=fd,rfdno=N,wfdno=N,aname=<share_name>". The
 * kernel 9p client does Tversion + Tattach over the fd; userspace can
 * close its copy after mount() returns (kernel fget()'d the file).
 *
 * After all mounts succeed, write /etc/ld.so.conf.d/wsl.conf with one
 * line per mount path that contains .so files, run ldconfig, and send
 * gpu_copy_done back to the host (best-effort status reply — host
 * doesn't block on it).
 *
 * Mount paths follow the WSL convention:
 *   AppSandbox.HostLxssLib  → /usr/lib/wsl/lib
 *   AppSandbox.Drv.N        → /usr/lib/wsl/drivers/<windows-leaf>
 */

#define VSOCK_PLAN9_PORT  50001u
#define WSL_LIB_DIR       "/usr/lib/wsl/lib"
#define WSL_DRV_BASE      "/usr/lib/wsl/drivers"

/* Recursive mkdir like `mkdir -p`. Best-effort: ignores existing dirs
 * but reports if the final mkdir fails. */
static int mkpath_p(const char *path)
{
    char tmp[1024];
    size_t len = strnlen(path, sizeof(tmp));
    if (len == 0 || len >= sizeof(tmp)) return -1;
    memcpy(tmp, path, len + 1);
    /* Strip trailing slash (except root). */
    if (len > 1 && tmp[len-1] == '/') tmp[len-1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) < 0 && errno != EEXIST) {
                agent_log("mkdir %s: %s", tmp, strerror(errno));
                /* keep going — parent might already exist */
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST) return -1;
    return 0;
}

/* Translate a share name (and the Windows guest_path we're handed) into
 * the Linux mount target. Returns 0 if the share is one we recognise,
 * -1 otherwise. */
static int derive_mount_target(const char *share_name,
                               const char *wpath,
                               char *out, size_t outsz)
{
    if (strcmp(share_name, "AppSandbox.HostLxssLib") == 0) {
        snprintf(out, outsz, "%s", WSL_LIB_DIR);
        return 0;
    }
    if (strncmp(share_name, "AppSandbox.Drv.", 15) == 0) {
        /* Extract the leaf folder from the Windows guest_path.
         * Tolerate either backslash or forward slash separators. */
        const char *bs = strrchr(wpath, '\\');
        const char *fs = strrchr(wpath, '/');
        const char *leaf = bs > fs ? bs : fs;
        leaf = leaf ? leaf + 1 : wpath;
        if (!*leaf) return -1;
        snprintf(out, outsz, "%s/%s", WSL_DRV_BASE, leaf);
        return 0;
    }
    return -1;
}

/* Mount one Plan9 share. Returns 0 on success, -1 otherwise. Logs the
 * outcome either way. */
static int mount_plan9_share(const char *share_name, const char *target)
{
    int v;
    struct sockaddr_vm sa = {0};
    char opts[512];

    /* mkdir -p target first — mount(2) requires it to exist. */
    if (mkpath_p(target) < 0) {
        agent_log("gpu_mount: mkdir(%s): %s", target, strerror(errno));
        return -1;
    }

    /* If already mounted (re-run on second agent connect), umount first
     * so the new mount replaces stale state. MNT_DETACH lets the kernel
     * drop refs lazily without blocking on outstanding accesses. */
    if (umount2(target, MNT_DETACH) == 0)
        agent_log("gpu_mount: lazy-unmounted stale %s", target);

    v = socket(AF_VSOCK, SOCK_STREAM, 0);
    if (v < 0) {
        agent_log("gpu_mount: socket(AF_VSOCK): %s", strerror(errno));
        return -1;
    }
    sa.svm_family = AF_VSOCK;
    sa.svm_cid    = VMADDR_CID_HOST;   /* 2 = host parent partition */
    sa.svm_port   = VSOCK_PLAN9_PORT;
    if (connect(v, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        agent_log("gpu_mount: vsock connect (2,%u): %s",
                  VSOCK_PLAN9_PORT, strerror(errno));
        close(v);
        return -1;
    }

    snprintf(opts, sizeof(opts),
        "trans=fd,rfdno=%d,wfdno=%d,version=9p2000.L,"
        "aname=%s,access=any,msize=65536,cache=loose",
        v, v, share_name);

    if (mount("none", target, "9p", MS_RDONLY | MS_NODEV | MS_NOSUID,
              opts) < 0) {
        agent_log("gpu_mount: mount %s -> %s: %s",
                  share_name, target, strerror(errno));
        close(v);
        return -1;
    }

    /* Kernel held its own reference via fget; we can drop ours. */
    close(v);
    agent_log("gpu_mount: %s -> %s OK", share_name, target);
    return 0;
}

/* Update /etc/ld.so.conf.d/wsl.conf to list every mount path that holds
 * .so files, then run ldconfig. Idempotent — overwrites each time. */
static void refresh_ldconfig_for_mounts(char (*paths)[1024], int n_paths)
{
    FILE *f = fopen("/etc/ld.so.conf.d/wsl.conf", "w");
    if (!f) {
        agent_log("ldconfig: open wsl.conf: %s", strerror(errno));
        return;
    }
    fprintf(f, "# Auto-generated by appsandbox-agent. Do not edit.\n");
    for (int i = 0; i < n_paths; i++)
        fprintf(f, "%s\n", paths[i]);
    fclose(f);

    int rc = run_sync("ldconfig 2>&1");
    agent_log("ldconfig refreshed (%d paths, rc=%d)", n_paths, rc);
}

static void handle_gpu_query_response(int fd, int n_shares)
{
    if (n_shares <= 0 || n_shares > 64) {
        agent_log("gpu_query: bad share count %d", n_shares);
        return;
    }

    char ldpaths[64][1024];
    int  n_ldpaths = 0;
    int  n_mounted = 0;

    for (int i = 0; i < n_shares; i++) {
        char line[LINE_BUF_MAX];
        int n = recv_line(fd, line, sizeof(line));
        if (n <= 0) {
            agent_log("gpu_query: short read on share %d", i);
            return;
        }
        agent_log("gpu_share[%d]: %s", i, line);

        /* Parse "share_name|guest_path|filter". filter may be empty. */
        char *p1 = strchr(line, '|');
        if (!p1) { agent_log("gpu_query: malformed: %s", line); continue; }
        *p1++ = '\0';
        char *p2 = strchr(p1, '|');
        if (p2) *p2++ = '\0';

        const char *share = line;        /* before first '|' */
        const char *wpath = p1;          /* between '|'s */
        (void)p2;                        /* filter — unused on Linux */

        char target[1024];
        if (derive_mount_target(share, wpath, target, sizeof(target)) < 0) {
            agent_log("gpu_query: unknown share name '%s', skipping", share);
            continue;
        }

        if (mount_plan9_share(share, target) < 0)
            continue;

        if (n_ldpaths < (int)(sizeof(ldpaths) / sizeof(ldpaths[0]))) {
            strncpy(ldpaths[n_ldpaths], target, sizeof(ldpaths[0]) - 1);
            ldpaths[n_ldpaths][sizeof(ldpaths[0]) - 1] = '\0';
            n_ldpaths++;
        }
        n_mounted++;
    }

    if (n_ldpaths > 0)
        refresh_ldconfig_for_mounts(ldpaths, n_ldpaths);

    /* Status reply to host. vm_agent.c just logs it; we send anyway so
     * the host log line "GPU copy complete for ..." fires and the user
     * sees confirmation that the share pipeline finished. */
    char ack[64];
    snprintf(ack, sizeof(ack), "gpu_copy_done:%d", n_mounted);
    send_line(g_client_fd, ack);
    agent_log("gpu_query: mounted %d/%d shares", n_mounted, n_shares);
}

/* ---- Per-client command dispatch ---- */

static void handle_client(int fd)
{
    pthread_t hb;
    int       hb_started = 0;
    char      line[LINE_BUF_MAX];

    g_client_fd = fd;
    agent_log("client connected (fd=%d)", fd);

    if (send_line(fd, "hello") < 0) {
        agent_log("hello: send failed");
        goto out;
    }

    if (pthread_create(&hb, NULL, heartbeat_thread, NULL) != 0) {
        agent_log("heartbeat: pthread_create failed: %s", strerror(errno));
    } else {
        hb_started = 1;
    }

    while (!g_stop) {
        int   n;
        char  tag[32] = "";
        const char *cmd;
        char *colon;

        n = recv_line(fd, line, sizeof(line));
        if (n <= 0) break;

        /* Strip optional "<digits>:" sequence tag */
        cmd   = line;
        colon = strchr(line, ':');
        if (colon && colon > line && (size_t)(colon - line) < sizeof(tag) - 1) {
            int is_seq = 1;
            for (char *p = line; p < colon; p++) {
                if (*p < '0' || *p > '9') { is_seq = 0; break; }
            }
            if (is_seq) {
                size_t t = (size_t)(colon - line + 1);
                memcpy(tag, line, t);
                tag[t] = '\0';
                cmd = colon + 1;
            }
        }

        agent_log("cmd: %s", line);

        if (strcmp(cmd, "ping") == 0) {
            send_reply(fd, tag, "ok");
        }
        else if (strcmp(cmd, "shutdown") == 0) {
            send_reply(fd, tag, "ok");
            agent_log("shutdown requested");
            spawn_detached("sleep 1 && systemctl poweroff");
        }
        else if (strcmp(cmd, "restart") == 0) {
            send_reply(fd, tag, "ok");
            agent_log("restart requested");
            spawn_detached("sleep 1 && systemctl reboot");
        }
        else if (strncmp(cmd, "set_ip:", 7) == 0) {
            handle_set_ip(fd, tag, cmd + 7);
        }
        else if (strcmp(cmd, "ssh_enable") == 0) {
            handle_ssh_enable(fd, tag);
        }
        else if (strncmp(cmd, "ssh_deploy_key ", 15) == 0) {
            send_reply(fd, tag, deploy_ssh_key(cmd + 15) ? "ssh_key_deployed" : "ssh_key_failed");
        }
        else if (strcmp(cmd, "idd_connect") == 0) {
            /* Mirror Windows handle_idd_connect (tools/agent/agent.c:1485):
             * kill+respawn the helper so a fresh CLDY handshake happens
             * for the new host display session. The monitor thread keeps
             * it alive after that. */
            respawn_clipboard_helper();
            send_reply(fd, tag, "ok");
        }
        else if (strncmp(cmd, "gpu_query_response:", 19) == 0) {
            int n_shares = atoi(cmd + 19);
            handle_gpu_query_response(fd, n_shares);
            /* No reply — host sends this fire-and-forget */
        }
        else if (strcmp(cmd, "gpu_none") == 0) {
            /* Host reports no GPU-PV assignment — nothing to do */
        }
        else if (strcmp(cmd, "gpu_copy") == 0) {
            /* Host asks us to re-trigger the share enumeration.
             * Not implemented in v1. */
            send_reply(fd, tag, "error:not_implemented");
        }
        else {
            send_reply(fd, tag, "error:unknown");
        }
    }

out:
    agent_log("client disconnected");
    g_client_fd = -1;
    if (hb_started) {
        /* heartbeat thread checks g_client_fd and exits on its own */
        pthread_join(hb, NULL);
    }
}

/* ---- Listener setup ---- */

static int listen_vsock(void)
{
    int s = socket(AF_VSOCK, SOCK_STREAM, 0);
    struct sockaddr_vm addr;

    if (s < 0) {
        agent_log("socket(AF_VSOCK) failed: %s", strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.svm_family = AF_VSOCK;
    addr.svm_cid    = VMADDR_CID_ANY;
    addr.svm_port   = AGENT_PORT;

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        agent_log("bind(vsock:%d) failed: %s", AGENT_PORT, strerror(errno));
        close(s);
        return -1;
    }
    if (listen(s, 1) < 0) {
        agent_log("listen failed: %s", strerror(errno));
        close(s);
        return -1;
    }
    agent_log("listening on AF_VSOCK port %d", AGENT_PORT);
    return s;
}

/* ---- Signal handling ---- */

static void on_term(int sig)
{
    (void)sig;
    g_stop = 1;
    /* Best-effort: tell host we're stopping before systemd kills us.
     * Use raw write — async-signal-safe. */
    int fd = g_client_fd;
    if (fd >= 0) {
        static const char msg[] = "service_stopping\n";
        ssize_t w = write(fd, msg, sizeof(msg) - 1);
        (void)w;
    }
}

/* ---- Main ---- */

int main(int argc, char **argv)
{
    struct sigaction sa;
    int ls;

    (void)argc; (void)argv;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    /* Don't die from SIGPIPE if peer goes away mid-write */
    signal(SIGPIPE, SIG_IGN);

    ls = listen_vsock();
    if (ls < 0) return 1;

    /* Start the clipboard monitor thread BEFORE accepting any control
     * clients. It'll spawn the helper as soon as the user session is
     * ready, independent of host-driven idd_connect. */
    start_clipboard_monitor();

    while (!g_stop) {
        int c = accept(ls, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR) continue;
            agent_log("accept failed: %s", strerror(errno));
            break;
        }
        handle_client(c);
        close(c);
    }

    close(ls);
    agent_log("agent stopped");
    return 0;
}
