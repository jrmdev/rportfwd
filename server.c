// server.c
// Simple reverse port forward server for Windows (single client).
// Compile: cl /MD /O2 /W3 /Fe:server.exe server.c Ws2_32.lib

#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#pragma comment(lib, "Ws2_32.lib")

#define BACKLOG 10
#define BUF_SZ 4096
#define MAX_TUNNELS 64

typedef struct Pending {
    int sessionid;
    SOCKET ext_sock;
    int port;
    struct Pending *next;
} Pending;

typedef struct {
    int port;
    SOCKET listener;
    HANDLE thread;
} Tunnel;

typedef struct {
    SOCKET listener;      // main server listen socket
    SOCKET ctrl_sock;     // current control socket (client)
    CRITICAL_SECTION lock;
    Tunnel tunnels[MAX_TUNNELS];
    int tunnel_count;
    Pending *pending;
    int next_sessionid;
} ServerState;

/* Global state pointer used by tunnel accept threads */
static ServerState *g_state = NULL;

/* Simple debug print */
void debug_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

/* Pending queue helpers */
void add_pending(ServerState *st, int sid, SOCKET ext, int port) {
    Pending *p = (Pending*)malloc(sizeof(Pending));
    if (!p) { closesocket(ext); return; }
    p->sessionid = sid;
    p->ext_sock = ext;
    p->port = port;
    EnterCriticalSection(&st->lock);
    p->next = st->pending;
    st->pending = p;
    LeaveCriticalSection(&st->lock);
}

SOCKET pop_pending(ServerState *st, int sid) {
    EnterCriticalSection(&st->lock);
    Pending **pp = &st->pending;
    while (*pp) {
        if ((*pp)->sessionid == sid) {
            Pending *found = *pp;
            SOCKET s = found->ext_sock;
            *pp = found->next;
            free(found);
            LeaveCriticalSection(&st->lock);
            return s;
        }
        pp = &(*pp)->next;
    }
    LeaveCriticalSection(&st->lock);
    return INVALID_SOCKET;
}

/* Create a listening socket on addr:port */
SOCKET make_listener(const char *addr, int port) {
    struct addrinfo hints, *res = NULL;
    char portbuf[32];
    sprintf_s(portbuf, sizeof(portbuf), "%d", port);
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    if (getaddrinfo(addr, portbuf, &hints, &res) != 0) return INVALID_SOCKET;
    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) { freeaddrinfo(res); return INVALID_SOCKET; }
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));
    if (bind(s, res->ai_addr, (int)res->ai_addrlen) == SOCKET_ERROR) {
        closesocket(s); freeaddrinfo(res); return INVALID_SOCKET;
    }
    if (listen(s, BACKLOG) == SOCKET_ERROR) {
        closesocket(s); freeaddrinfo(res); return INVALID_SOCKET;
    }
    freeaddrinfo(res);
    return s;
}

/* Forward declarations */
unsigned __stdcall tunnel_accept_thread(void *arg);
void start_tunnel(ServerState *st, int port);
void stop_tunnel(ServerState *st, int port);

/* Start listening thread for a server-side tunnel port */
void start_tunnel(ServerState *st, int port) {
    EnterCriticalSection(&st->lock);
    if (st->tunnel_count >= MAX_TUNNELS) {
        LeaveCriticalSection(&st->lock);
        debug_printf("Tunnel limit reached");
        return;
    }
    LeaveCriticalSection(&st->lock);

    SOCKET l = make_listener("0.0.0.0", port);
    if (l == INVALID_SOCKET) {
        debug_printf("Failed to listen on port %d (maybe in use)", port);
        return;
    }

    EnterCriticalSection(&st->lock);
    Tunnel *t = &st->tunnels[st->tunnel_count++];
    t->port = port;
    t->listener = l;
    t->thread = (HANDLE)_beginthreadex(NULL, 0, tunnel_accept_thread, (void*)t, 0, NULL);
    LeaveCriticalSection(&st->lock);
    debug_printf("Started tunnel on server port %d", port);
}

/* Stop a started tunnel */
void stop_tunnel(ServerState *st, int port) {
    EnterCriticalSection(&st->lock);
    for (int i = 0; i < st->tunnel_count; ++i) {
        if (st->tunnels[i].port == port) {
            closesocket(st->tunnels[i].listener);
            WaitForSingleObject(st->tunnels[i].thread, 500);
            CloseHandle(st->tunnels[i].thread);
            st->tunnels[i] = st->tunnels[st->tunnel_count - 1];
            st->tunnel_count--;
            LeaveCriticalSection(&st->lock);
            debug_printf("Stopped tunnel on port %d", port);
            return;
        }
    }
    LeaveCriticalSection(&st->lock);
    debug_printf("Tunnel on port %d not found", port);
}

/* Tunnel accept thread: accepts external connections on a server port,
   creates a session id, adds to pending, and notifies control with OPEN <sid> <port> */
unsigned __stdcall tunnel_accept_thread(void *arg) {
    Tunnel *tun = (Tunnel*)arg;
    ServerState *st = g_state;
    if (!st) return 0;

    while (1) {
        SOCKET ext = accept(tun->listener, NULL, NULL);
        if (ext == INVALID_SOCKET) {
            // likely the listener was closed
            break;
        }
        int sid = InterlockedIncrement((volatile LONG*)&st->next_sessionid);
        add_pending(st, sid, ext, tun->port);

        EnterCriticalSection(&st->lock);
        SOCKET ctrl = st->ctrl_sock;
        LeaveCriticalSection(&st->lock);

        if (ctrl != 0 && ctrl != INVALID_SOCKET) {
            char msg[64];
            sprintf_s(msg, sizeof(msg), "OPEN %d %d\n", sid, tun->port);
            send(ctrl, msg, (int)strlen(msg), 0);
            debug_printf("Notified control: %s", msg);
        } else {
            closesocket(ext);
            debug_printf("No control - dropped incoming on port %d", tun->port);
        }
    }
    return 0;
}

/* Proxy worker copies a->b until EOF */
unsigned __stdcall proxy_worker(void *arg) {
    SOCKET *s = (SOCKET*)arg;
    SOCKET a = s[0];
    SOCKET b = s[1];
    free(s);
    char buf[BUF_SZ];
    while (1) {
        int r = recv(a, buf, (int)sizeof(buf), 0);
        if (r <= 0) break;
        int sent = 0;
        while (sent < r) {
            int w = send(b, buf + sent, r - sent, 0);
            if (w <= 0) goto done;
            sent += w;
        }
    }
done:
    shutdown(a, SD_BOTH);
    shutdown(b, SD_BOTH);
    closesocket(a);
    closesocket(b);
    return 0;
}

/* Start bidirectional proxy pair using two threads */
void start_proxy_pair(SOCKET a, SOCKET b) {
    SOCKET *p1 = (SOCKET*)malloc(sizeof(SOCKET) * 2);
    p1[0] = a; p1[1] = b;
    SOCKET *p2 = (SOCKET*)malloc(sizeof(SOCKET) * 2);
    p2[0] = b; p2[1] = a;
    _beginthreadex(NULL, 0, proxy_worker, p1, 0, NULL);
    _beginthreadex(NULL, 0, proxy_worker, p2, 0, NULL);
}

/* Read a single line (blocking) until '\n' */
int recv_line(SOCKET s, char *buf, int buflen) {
    int pos = 0;
    while (pos < buflen - 1) {
        char c;
        int r = recv(s, &c, 1, 0);
        if (r <= 0) return r;
        buf[pos++] = c;
        if (c == '\n') break;
    }
    buf[pos] = 0;
    return pos;
}

/* Handle control socket lines (LISTEN / CLOSE) */
void handle_control_socket(ServerState *st, SOCKET ctrl) {
    char line[512];
    while (1) {
        int r = recv_line(ctrl, line, (int)sizeof(line));
        if (r <= 0) break;
        // trim CRLF
        while (r > 0 && (line[r-1] == '\n' || line[r-1] == '\r')) { line[r-1] = 0; r--; }
        if (r == 0) continue;
        debug_printf("CTRL: %s", line);
        if (strncmp(line, "LISTEN ", 7) == 0) {
            int port = atoi(line + 7);
            if (port > 0) start_tunnel(st, port);
        } else if (strncmp(line, "CLOSE ", 6) == 0) {
            int port = atoi(line + 6);
            if (port > 0) stop_tunnel(st, port);
        } else {
            debug_printf("Unknown control command: %s", line);
        }
    }

    /* control socket closed: clear it */
    EnterCriticalSection(&st->lock);
    if (st->ctrl_sock == ctrl) st->ctrl_sock = 0;
    LeaveCriticalSection(&st->lock);
    debug_printf("Control connection closed");
}

/* Thread that invokes handle_control_socket (simple wrapper) */
unsigned __stdcall control_thread(void *arg) {
    ServerState *st = (ServerState*)arg;
    EnterCriticalSection(&st->lock);
    SOCKET ctrl = st->ctrl_sock;
    LeaveCriticalSection(&st->lock);
    if (ctrl != 0 && ctrl != INVALID_SOCKET) {
        handle_control_socket(st, ctrl);
    }
    return 0;
}

/* Main */
int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: %s <listen_addr> <listen_port>\n", argv[0]);
        printf("Example: %s 0.0.0.0 2222\n", argv[0]);
        return 1;
    }
    const char *addr = argv[1];
    int port = atoi(argv[2]);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        printf("WSAStartup failed\n"); return 1;
    }

    ServerState st;
    ZeroMemory(&st, sizeof(st));
    InitializeCriticalSection(&st.lock);
    st.listener = make_listener(addr, port);
    if (st.listener == INVALID_SOCKET) {
        printf("Failed to listen on %s:%d\n", addr, port);
        return 1;
    }
    st.ctrl_sock = 0;
    st.tunnel_count = 0;
    st.pending = NULL;
    st.next_sessionid = 0;
    g_state = &st;

    printf("Server listening on %s:%d\n", addr, port);

    while (1) {
        SOCKET s = accept(st.listener, NULL, NULL);
        if (s == INVALID_SOCKET) {
            Sleep(100);
            continue;
        }

        /* Read first line from the newly accepted connection to determine its type */
        char line[256];
        int r = recv_line(s, line, (int)sizeof(line));
        if (r <= 0) { closesocket(s); continue; }
        while (r > 0 && (line[r-1] == '\n' || line[r-1] == '\r')) { line[r-1] = 0; r--; }

        if (strncmp(line, "DATA ", 5) == 0) {
            int sid = atoi(line + 5);
            SOCKET ext = pop_pending(&st, sid);
            if (ext == INVALID_SOCKET) {
                debug_printf("No pending for DATA %d", sid);
                closesocket(s);
                continue;
            }
            debug_printf("Pairing DATA %d with external socket", sid);
            start_proxy_pair(ext, s);
        } else {
            /* treat as control socket */
            EnterCriticalSection(&st.lock);
            if (st.ctrl_sock != 0 && st.ctrl_sock != INVALID_SOCKET) {
                closesocket(st.ctrl_sock);
                st.ctrl_sock = 0;
            }
            st.ctrl_sock = s;
            LeaveCriticalSection(&st.lock);
            debug_printf("Control client connected");

            /* process the first already-read line (if it contained a command) */
            if (strncmp(line, "LISTEN ", 7) == 0) {
                int p = atoi(line + 7);
                if (p > 0) start_tunnel(&st, p);
            } else if (strncmp(line, "CLOSE ", 6) == 0) {
                int p = atoi(line + 6);
                if (p > 0) stop_tunnel(&st, p);
            }

            /* spawn a thread to handle further control messages */
            _beginthreadex(NULL, 0, control_thread, &st, 0, NULL);
        }
    }

    WSACleanup();
    return 0;
}
