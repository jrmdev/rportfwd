// client.c
// Reverse port forward client for Windows.
// Compile: cl /MD /O2 /W3 /Fe:client.exe client.c Ws2_32.lib

#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "Ws2_32.lib")

#define BUF_SZ 4096
#define MAX_TUNNELS 128

#ifndef _countof
#define _countof(a) (sizeof(a)/sizeof((a)[0]))
#endif

typedef struct {
    int server_port;           // port on server to listen on
    char client_addr[64];      // address on client machine to connect to
    int client_port;           // port on client machine to connect to
} TunnelMapping;

static TunnelMapping mappings[MAX_TUNNELS];
static int mapping_count = 0;
static CRITICAL_SECTION map_lock;

static SOCKET ctrl_sock = INVALID_SOCKET;
static char server_host[128];
static char server_port_str[16];

void debug_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

/* Connect to server, return SOCKET or INVALID_SOCKET */
SOCKET connect_to_server(const char *host, const char *port) {
    struct addrinfo hints, *res = NULL;
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0) return INVALID_SOCKET;
    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) { freeaddrinfo(res); return INVALID_SOCKET; }
    if (connect(s, res->ai_addr, (int)res->ai_addrlen) == SOCKET_ERROR) {
        closesocket(s); freeaddrinfo(res); return INVALID_SOCKET;
    }
    freeaddrinfo(res);
    return s;
}

/* Read a single line (until '\n') */
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

void add_mapping(int server_port, const char *client_addr, int client_port) {
    EnterCriticalSection(&map_lock);
    if (mapping_count >= MAX_TUNNELS) {
        LeaveCriticalSection(&map_lock);
        debug_printf("mapping full");
        return;
    }
    mappings[mapping_count].server_port = server_port;
    strncpy_s(mappings[mapping_count].client_addr, sizeof(mappings[mapping_count].client_addr), client_addr, _TRUNCATE);
    mappings[mapping_count].client_port = client_port;
    mapping_count++;
    LeaveCriticalSection(&map_lock);
}

void remove_mapping(int server_port) {
    EnterCriticalSection(&map_lock);
    int found = -1;
    for (int i = 0; i < mapping_count; ++i) {
        if (mappings[i].server_port == server_port) { found = i; break; }
    }
    if (found >= 0) {
        for (int j = found; j < mapping_count - 1; ++j) mappings[j] = mappings[j + 1];
        mapping_count--;
    }
    LeaveCriticalSection(&map_lock);
}

int find_mapping(int server_port, char *out_addr, int *out_port) {
    EnterCriticalSection(&map_lock);
    for (int i = 0; i < mapping_count; ++i) {
        if (mappings[i].server_port == server_port) {
            strncpy_s(out_addr, 64, mappings[i].client_addr, _TRUNCATE);
            *out_port = mappings[i].client_port;
            LeaveCriticalSection(&map_lock);
            return 1;
        }
    }
    LeaveCriticalSection(&map_lock);
    return 0;
}

/* proxy worker */
unsigned __stdcall proxy_worker(void *arg) {
    SOCKET *socks = (SOCKET*)arg;
    SOCKET a = socks[0], b = socks[1];
    free(socks);
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

void start_proxy_pair(SOCKET a, SOCKET b) {
    SOCKET *p1 = (SOCKET*)malloc(sizeof(SOCKET) * 2);
    p1[0] = a; p1[1] = b;
    SOCKET *p2 = (SOCKET*)malloc(sizeof(SOCKET) * 2);
    p2[0] = b; p2[1] = a;
    _beginthreadex(NULL, 0, proxy_worker, p1, 0, NULL);
    _beginthreadex(NULL, 0, proxy_worker, p2, 0, NULL);
}

/* Called when server sends "OPEN <sid> <server_port>" */
void handle_open(int sessionid, int server_port) {
    debug_printf("OPEN %d (server_port=%d) received", sessionid, server_port);
    char target_addr[64] = {0}; int target_port = 0;
    if (!find_mapping(server_port, target_addr, &target_port)) {
        debug_printf("No mapping for server_port %d, ignoring", server_port);
        return;
    }

    /* Connect back to server for DATA channel */
    SOCKET data_sock = connect_to_server(server_host, server_port_str);
    if (data_sock == INVALID_SOCKET) {
        debug_printf("Failed to connect to server for DATA %d", sessionid);
        return;
    }
    char line[64];
    sprintf_s(line, sizeof(line), "DATA %d\n", sessionid);
    send(data_sock, line, (int)strlen(line), 0);

    /* Connect to client-side target */
    SOCKET local_sock = INVALID_SOCKET;
    {
        struct addrinfo hints, *res = NULL;
        char portbuf[16];
        sprintf_s(portbuf, sizeof(portbuf), "%d", target_port);
        ZeroMemory(&hints, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(target_addr, portbuf, &hints, &res) == 0) {
            local_sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
            if (local_sock != INVALID_SOCKET) {
                if (connect(local_sock, res->ai_addr, (int)res->ai_addrlen) == SOCKET_ERROR) {
                    closesocket(local_sock);
                    local_sock = INVALID_SOCKET;
                }
            }
            freeaddrinfo(res);
        }
    }

    if (local_sock == INVALID_SOCKET) {
        debug_printf("Failed to connect to local target %s:%d", target_addr, target_port);
        closesocket(data_sock);
        return;
    }

    debug_printf("Paired DATA %d <-> %s:%d", sessionid, target_addr, target_port);
    start_proxy_pair(data_sock, local_sock);
}

/* Control reader thread: receives server messages like OPEN ... */
unsigned __stdcall control_reader(void *arg) {
    SOCKET s = (SOCKET)arg;
    char line[512];
    while (1) {
        int r = recv_line(s, line, (int)sizeof(line));
        if (r <= 0) break;
        int len = r;
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) { line[len-1] = 0; len--; }
        if (len == 0) continue;
        debug_printf("SERVER: %s", line);
        if (strncmp(line, "OPEN ", 5) == 0) {
            int sid = 0, srvport = 0;
            if (sscanf_s(line + 5, "%d %d", &sid, &srvport) >= 1) {
                handle_open(sid, srvport);
            }
        } else {
            debug_printf("Unknown from server: %s", line);
        }
    }
    debug_printf("Control connection closed by server");
    return 0;
}

/* Main client */
int main(int argc, char **argv) {
    if (argc != 3) {
        printf("Usage: %s <server_host> <server_port>\n", argv[0]);
        return 1;
    }
    strncpy_s(server_host, sizeof(server_host), argv[1], _TRUNCATE);
    strncpy_s(server_port_str, sizeof(server_port_str), argv[2], _TRUNCATE);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) { printf("WSAStartup failed\n"); return 1; }

    ctrl_sock = connect_to_server(server_host, server_port_str);
    if (ctrl_sock == INVALID_SOCKET) {
        printf("Failed to connect to server %s:%s\n", server_host, server_port_str);
        return 1;
    }
    printf("Connected to server %s:%s\n", server_host, server_port_str);

    InitializeCriticalSection(&map_lock);

    /* start reader thread */
    _beginthreadex(NULL, 0, control_reader, (void*)ctrl_sock, 0, NULL);

    /* interactive input */
    char cmdline[256];
    printf("Commands:\n  add <server_port> <client_addr> <client_port>\n  remove <server_port>\n  list\n  exit\n");
    while (1) {
        printf("> ");
        if (!fgets(cmdline, (int)sizeof(cmdline), stdin)) break;
        size_t L = strlen(cmdline);
        while (L > 0 && (cmdline[L-1] == '\n' || cmdline[L-1] == '\r')) { cmdline[L-1] = 0; L--; }
        if (L == 0) continue;

        if (strncmp(cmdline, "add ", 4) == 0) {
            int srvp = 0, clp = 0;
            char claddr[64] = {0};
            if (sscanf_s(cmdline + 4, "%d %63s %d", &srvp, claddr, (unsigned)_countof(claddr), &clp) >= 3) {
                char out[256];
                /* server only needs LISTEN <port>; we include client addr/port in the line for human readability */
                sprintf_s(out, sizeof(out), "LISTEN %d %s %d\n", srvp, claddr, clp);
                send(ctrl_sock, out, (int)strlen(out), 0);
                add_mapping(srvp, claddr, clp);
                debug_printf("Requested LISTEN %d -> %s:%d", srvp, claddr, clp);
            } else {
                printf("Usage: add <server_port> <client_addr> <client_port>\n");
            }
        } else if (strncmp(cmdline, "remove ", 7) == 0) {
            int srvp = 0;
            if (sscanf_s(cmdline + 7, "%d", &srvp) == 1) {
                char out[64];
                sprintf_s(out, sizeof(out), "CLOSE %d\n", srvp);
                send(ctrl_sock, out, (int)strlen(out), 0);
                remove_mapping(srvp);
                debug_printf("Requested CLOSE %d", srvp);
            } else {
                printf("Usage: remove <server_port>\n");
            }
        } else if (strcmp(cmdline, "list") == 0) {
            EnterCriticalSection(&map_lock);
            if (mapping_count == 0) printf("No mappings\n");
            for (int i = 0; i < mapping_count; ++i) {
                printf("server:%d -> %s:%d\n", mappings[i].server_port, mappings[i].client_addr, mappings[i].client_port);
            }
            LeaveCriticalSection(&map_lock);
        } else if (strcmp(cmdline, "exit") == 0) {
            break;
        } else {
            printf("Unknown. Commands:\n  add <server_port> <client_addr> <client_port>\n  remove <server_port>\n  list\n  exit\n");
        }
    }

    closesocket(ctrl_sock);
    WSACleanup();
    return 0;
}
