[![ru](https://img.shields.io/badge/lang-ru-green.svg)](README.ru-RU.md)

Stream Server
-

**Module** for [Apostol](https://github.com/apostoldevel/apostol) + [db-platform](https://github.com/apostoldevel/db-platform) — **Apostol CRM**[^crm].

Description
-
**StreamServer** is a UDP datagram receiver module that operates alongside the HTTP server within the same Apostol process. It receives binary or text datagrams on a configurable UDP port, and dispatches each datagram to a PostgreSQL function for processing.

The module consists of two layers:

- **StreamServer** (base class) — creates a non-blocking UDP socket, registers it with the `epoll`-based event loop, and invokes a virtual `on_datagram()` callback for each incoming packet.
- **PgStreamServer** (subclass) — overrides `on_datagram()` to dispatch each datagram to the PostgreSQL function `stream.parse(protocol, data)` asynchronously via the connection pool.

Key characteristics:

- Written in C++20 using the asynchronous, non-blocking I/O model based on the **epoll** API.
- The UDP socket and the HTTP listener share the same event loop — no threads, no extra processes.
- The base `StreamServer` class can be subclassed for custom datagram handling without PostgreSQL.

How it works
-
1. On `on_start()`, the module creates a `UdpSocket` bound to the configured port and registers it with EventLoop for `EPOLLIN` events.
2. When a datagram arrives, the event loop invokes `on_datagram(data, size, sender_addr)`.
3. `PgStreamServer` calls `SELECT stream.parse('<protocol>', '<hex_data>')` asynchronously.
4. On `on_stop()`, the socket is unregistered and closed.

Configuration
-

```json
{
  "modules": {
    "StreamServer": {
      "enabled": true,
      "port": 12228
    }
  }
}
```

| Parameter | Description |
|-----------|-------------|
| `enabled` | Enable/disable the module. |
| `port` | UDP port to listen on (0 = disabled). |

Installation
-
Follow the build and installation instructions for [Apostol](https://github.com/apostoldevel/apostol#build-and-installation).

[^crm]: **Apostol CRM** is an abstract term, not a standalone product. It refers to any project that uses both the [Apostol](https://github.com/apostoldevel/apostol) C++ framework and [db-platform](https://github.com/apostoldevel/db-platform) together through purpose-built modules and processes. Each framework can be used independently; combined, they form a full-stack backend platform.
