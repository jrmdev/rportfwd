# rportfwd
A minimal **reverse port forwarding** client and server for Windows (MSVC / Visual Studio 2022).  
Inspired by `ssh -R`, this project lets you expose a TCP service running on a *client* host through a *server* host.
This was written mostly by AI. There's no way I'd write this myself.

## Security notice:
This code is for **testing / lab** use only. It has **no authentication** and **no encryption**. Do **not** expose it to untrusted networks.

## What it does

- **Server** accepts a single client. Client tells server which `server_port` to listen on.
- **Client** connects to server and can dynamically `add` and `remove` reverse tunnels:
  - `add <server_port> <client_addr> <client_port>` — ask server to listen on `server_port`, forward incoming connections back to `client_addr:client_port` on the client side.
  - `remove <server_port>` — stop that tunnel.
- When an external peer connects to `server:server_port`:
  1. Server announces `OPEN <sessionid> <server_port>` to the client (over the control connection).
  2. Client opens a new connection to the server, sends `DATA <sessionid>\n`, connects to the local `client_addr:client_port`, and the two sockets are proxied.

---

## Files

- `server.c` — single-client reverse-forward server (MSVC-compatible).
- `client.c` — interactive client (MSVC-compatible).

---

## Compile (Tested under Visual Studio 2022 Developer Prompt)

```bat
cl /MD /O2 /W3 /Fe:server.exe server.c Ws2_32.lib
cl /MD /O2 /W3 /Fe:client.exe client.c Ws2_32.lib
```
---

## Usage

### Start the server
The server expects a listen address and port:

```bat
server.exe <listen_addr> <listen_port>
```

Example (listen on all interfaces, control port 2222):

```bat
server.exe 0.0.0.0 2222
```

### Start the client
Run the client on the host that actually runs the service you want to expose:

```bat
client.exe <server_host> <server_port>
```

Example:

```bat
client.exe your-server.example.com 2222
```

After connecting, the client enters an interactive prompt. Available commands:

```
add <server_port> <client_addr> <client_port>
remove <server_port>
list
exit
```

- `add 8080 127.0.0.1 80` — tell the server to listen on port `8080` and forward to `127.0.0.1:80` on the client machine.
- `remove 8080` — stop that mapping.
- `list` — show current mappings in the client.
- `exit` — close control connection and quit.

> The client sends `LISTEN <port>` and `CLOSE <port>` control lines to the server. The server responds by creating/destroying listeners and will send `OPEN <sessionid> <port>` when a connection arrives.

---

## Example

1. **On server (public machine):**

   ```bat
   server.exe 0.0.0.0 2222
   ```

2. **On client (local machine that runs the service):**

   ```bat
   client.exe server.example.com 2222
   ```

   At the `>` prompt:

   ```
   > add 9000 127.0.0.1 8080
   ```

   - This requests the server listen on `server:9000`.
   - When someone connects to `server:9000`, that connection will be reverse forwarded to `client:127.0.0.1:8080`.

---

## Protocol summary

- **Control channel (client ↔ server)** — text lines terminated with `\n`:
  - `LISTEN <port> [client_addr client_port]` — client asks server to open a tunnel (server ignores the extra fields; client keeps the mapping locally).
  - `CLOSE <port>` — client asks server to close the tunnel.
  - `OPEN <sessionid> <port>` — server notifies client that an external connection arrived and a `DATA` channel is expected.

- **Data channel (client → server)**:
  - New TCP connection where client immediately sends: `DATA <sessionid>\n` — this connection will be paired with the external connection identified by `<sessionid>`, and bytes are proxied both ways.

---

## ASCII diagram

```
            +---------------------+
            |   Server-side user  |
            +---------------------+
                     |
     connects to     | TCP
     server:9000     |
                     v
            +-----------------+                     Control TCP
            |     Server      |<-------------------------------------+
            |                 |                                      |
            | - listener 9000 |                                      |
            | - control port  |<-- control connection -------------->|
            +-----------------+                                      |
                     | OPEN <sid> <port>                             |
                     |                                               |
                     | paired with DATA connection                   |
                     v                                               |
            [pending ext socket]                                     |
                     |                                               |
                     |  <--- proxied data --->                       |
                     |                                               |
            +-----------------+     DATA <sid>   +---------------+   |
            |     Client      |----------------->|   client.exe  |-->|
            |   (behind NAT)  |                  | (connects to) | 
            +-----------------+                  +---------------+ 
                                                - connects to server control port 
                                                - opens DATA connections when server
                                                - sends OPEN
```

---

## Limitations & notes

- Single-client server only (new control connection replaces the old one).
- No encryption, no authentication — *use only in trusted test environments*.
- Blocking sockets + threads are used for simplicity.
- TCP only.
