[![ru](https://img.shields.io/badge/lang-ru-green.svg)](README.ru-RU.md)

Stream Server
-

**Process** for [Apostol](https://github.com/apostoldevel/apostol) + [db-platform](https://github.com/apostoldevel/db-platform) — **Apostol CRM**[^crm].

Description
-

**Stream Server** is a C++ UDP streaming data server process for the [Apostol](https://github.com/apostoldevel/apostol) framework. It runs as an independent background process and ingests binary telemetry packets from IoT devices and mobile clients over UDP.

Key characteristics:

* Written in C++14 using an asynchronous, non-blocking I/O model based on the **epoll** API — suitable for high-throughput, low-latency telemetry ingestion.
* Uses a **UDP server** (`CUDPAsyncServer`) to receive datagrams from IoT and mobile devices without maintaining persistent connections.
* Implements the **LPWAN** binary protocol — a lightweight, device-agnostic binary format with variable-length framing, device type and serial number identification, multi-packet command reassembly, and **CRC16 (Modbus)** integrity checking. All byte ordering is little-endian.
* Forwards every validated packet to **PostgreSQL** via the `stream.parse()` PL/pgSQL function. All protocol decoding, device auto-registration, and data storage are handled inside the database.
* Connects to PostgreSQL using the `[postgres/helper]` connection pool as the `apibot` user, authenticated via OAuth2 `client_credentials`.
* Supports three device classes: **IoT** (`0xA0`), **Android** (`0xA1`), and **iOS** (`0xA2`).
* On first contact from an unknown device, the database automatically creates a device record. Subsequent packets update GPS coordinates, battery level, and other sensor values.

### How it fits into Apostol

Apostol runs a master process that spawns N worker processes and one or more independent background processes. `CStreamServer` is one such background process — it runs in its own OS process alongside (and independently of) the Apostol workers. When a UDP datagram arrives:

1. The epoll event loop fires `DoRead`, which reads the raw datagram into a buffer.
2. The variable-length `length` field (1 or 2 bytes) is parsed and the **CRC16** checksum is verified using the Modbus polynomial. Malformed or corrupt packets are silently discarded.
3. Each validated packet binary is base64-encoded and passed to `Parse()`, which constructs the SQL call `SELECT * FROM stream.parse(protocol, peer_address, base64_data)` for every active authenticated session.
4. PostgreSQL's `stream.Parse()` decodes the base64 payload, dispatches to the appropriate protocol handler (`stream.ParseLPWAN` for the **LPWAN** protocol), persists the raw packet to `stream.log`, and executes device-side business logic (device lookup or creation, GPS update, sensor value storage).
5. If the database returns a non-null base64-encoded response, the C++ layer decodes it and sends a UDP reply back to the originating device.

The process authenticates against PostgreSQL at startup and re-authenticates every 24 hours (retrying after 5 seconds on error) using the OAuth2 `client_credentials` grant to maintain valid sessions for all database calls.

Installation
-

Follow the build and installation instructions for [Apostol](https://github.com/apostoldevel/apostol#build) and [db-platform](https://github.com/apostoldevel/db-platform#quick-start).

Protocol
-

The protocol is designed without binding to a specific device type.

### Terminology

* **Packet** — the unit of data transfer at the data link layer.

* **Command** — the unit of data transfer at the application layer. If a command is long, it is split into packets.

A command consists of data from combined packets. The combination is performed from the initial to the final packet in ascending packet number order.

Packets of a command may arrive in arbitrary order. An incompletely assembled command is discarded after a timeout.

If the device type is undefined and the identifier is empty, this is a broadcast packet.

#### General packet structure

| Field | Field size | Data type | Description |
|-------|-----------|-----------|-------------|
| Packet length | 1 or 2 bytes | length | Length of the entire packet (all fields except the length field) in bytes. |
| Protocol version | 1 byte | uint8 | Current version = 1. |
| Parameters | 1 byte | bin | bit 0 — initial command packet; bit 1 — final command packet; bit 2 — packet to device; bit 3 — reply to request. |
| Device type | 1 byte | device_type | 0xA0 - IoT, 0xA1, 0xA2 - Mobile devices. |
| Serial number size | 1 byte | uint8 | In bytes. |
| Serial number | ... | utf8 | Example: ABC-012345678 |
| Command number | 1 byte | uint8 | Cyclic from 0 to 255. On reply to a request, copied from the request. |
| Packet number | 1 byte | uint8 | Packet number within the command, 0–255 (starts at 0 for each command). |
| Packet data | ... | ... | |
| Checksum | 2 bytes | crc16 | All fields except the checksum. |

#### Command structure (from device)

| Field | Field size | Data type | Description |
|-------|-----------|-----------|-------------|
| Timestamp | 4 bytes | time | Current device time: 0 — undefined time, 1 — time error. |
| Command type | 1 byte | uint8 | 0x01 — Current device state, etc. |
| Error code | 1 byte | uint8 | See below. If there is an error, the "Command data" field is absent. |
| Command data | ... | ... | Format and size depend on the command type. |

The reply command type equals the request command type with the high bit cleared.
Example: request 0x81 — reply 0x01, request 0x82 — reply 0x02, etc.

Error codes:
* 0 — No error;
* 1 — Undefined error;
* 2 — Unknown command;
* 3 — Invalid command format;
* 4 — Invalid command parameters.

#### Command structure (to device)

| Field | Field size | Data type | Description |
|-------|-----------|-----------|-------------|
| Command type | 1 byte | uint8 | 0x82 — transparent data request, etc. |
| Command data | ... | ... | Format and size depend on the command type. |

### Command types

#### Current device state

**Command**: `0x01`

* Sent when there has been no exchange during the period set in the configuration, and in response to a "Current state request".

| Field | Field size | Data type | Description |
|-------|-----------|-----------|-------------|
| State | 2 bytes | device_state | |
| Current tariff | 1 byte | uint8 | 1, 2, etc. 0 — current tariff not set. |
| Time of last configuration | 4 bytes | time | 0 — no configuration performed, 1 — time error at last configuration. |
| Type of last time sync | 1 byte | time_sync | |
| Time of last time sync | 4 bytes | time | 0 — no synchronisation performed, 1 — time error at last sync. |

#### Current device values

**Command**: `0x04`

| Field | Field size | Data type | Description |
|-------|-----------|-----------|-------------|
| Value count | 1 byte | uint8 | Number of current values. |
| For each value: | | | |
| Value type | 1 byte | value_type | |
| Value size | 1 byte | uint8 | In bytes. |
| Current value | ... | ... | Format depends on value type. |

#### Packet example

Command `0x01` [Current device state](#current-device-state).
 - Command number: `0xF0` (int8);
 - Serial number: `1234` (utf8);
 - Time: `2020-01-01 08:09:10` (time);
 - No errors: `0x00` (int8);
 - Time of last time sync: `2020-01-01 08:09:00` (time).

Command in hexadecimal:
````
 1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31
1E 01 03 06 04 31 32 33 34 F0 00 A6 53 0C 5E 01 00 00 00 04 00 00 00 00 02 9C 53 0C 5E 3E C7
│  │  │  │  │  │           │  │  ├──┐        │  │  │     │  │           │  │           └─ CRC16
│  │  │  │  │  │           │  │  │  │        │  │  │     │  │           │  └─ Time of last sync: 5E0C539C₁₆ -> 1577866140₁₀ -> 2020-01-01T08:09:00+00:00
│  │  │  │  │  │           │  │  │  │        │  │  │     │  │           └─ Type of last time sync: 0x02 - NTP sync
│  │  │  │  │  │           │  │  │  │        │  │  │     │  └─ Time of last configuration: 0 — no configuration performed
│  │  │  │  │  │           │  │  │  │        │  │  │     └─ Current tariff: 4
│  │  │  │  │  │           │  │  │  │        │  │  └─ State: 0 (2 bytes)
│  │  │  │  │  │           │  │  │  │        │  └─ Error code: 0 (No error)
│  │  │  │  │  │           │  │  │  │        └─ Command type: 0x01 (Current state)
│  │  │  │  │  │           │  │  │  └─ Timestamp: 5E0C53A6₁₆ -> 1577866150₁₀ -> 2020-01-01T08:09:10+00:00
│  │  │  │  │  │           │  │  └─ Packet data
│  │  │  │  │  │           │  └─ Packet number
│  │  │  │  │  │           └─ Command number
│  │  │  │  │  └─ Serial number
│  │  │  │  └─ Serial number size
│  │  │  └─ Device type
│  │  └─ Parameters: 03 -> 00000011 - initial and final command packet
│  └─ Protocol version
└─ Packet length: 1E - 30 bytes (all fields except the length field).
````

Command `0x04` [Current device values](#current-device-values).
 - Command number: `0x01` (int8);
 - Serial number: `ABCD1234` (utf8);
 - Time: `2020-11-01 02:03:04` (time);
 - Battery charge: `87%` (int16);
 - Latitude: `55.754157803652780` (decimal_degree);
 - Longitude: `37.620306072829976` (decimal_degree).

Command in hexadecimal:
````
 1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 38 39 40 41 42 43 44 45 46 47 48
2F 01 03 A1 08 41 42 43 44 31 32 33 34 01 00 58 17 9E 5F 04 00 03 00 02 FC 21 01 08 1F 5B 2F 3E 88 E0 4B 40 02 08 50 28 7C 30 66 CF 42 40 14 49
│  │  │  │  │  │                       │  │  ├──┐        │  │  │  │  │  │     │  │  │                       │  │  │                       └─ CRC16: 0x4914 -> 18708
│  │  │  │  │  │                       │  │  │  │        │  │  │  │  │  │     │  │  │                       │  │  └─ 4042CF66307C2850₁₆ -> 37.620306072829976₁₀
│  │  │  │  │  │                       │  │  │  │        │  │  │  │  │  │     │  │  │                       │  └─ Value size: 8 bytes
│  │  │  │  │  │                       │  │  │  │        │  │  │  │  │  │     │  │  │                       └─ Value type: 2 - Longitude
│  │  │  │  │  │                       │  │  │  │        │  │  │  │  │  │     │  │  └─ 404BE0883E2F5B1F₁₆ -> 55.754157803652780₁₀
│  │  │  │  │  │                       │  │  │  │        │  │  │  │  │  │     │  └─ Value size: 8 bytes
│  │  │  │  │  │                       │  │  │  │        │  │  │  │  │  │     └─ Value type: 1 - Latitude
│  │  │  │  │  │                       │  │  │  │        │  │  │  │  │  └─ 21FC₁₆ -> 8700
│  │  │  │  │  │                       │  │  │  │        │  │  │  │  └─ Value size: 2 bytes
│  │  │  │  │  │                       │  │  │  │        │  │  │  └─ Value type: 0 - Battery charge
│  │  │  │  │  │                       │  │  │  │        │  │  └─ Value count: 3
│  │  │  │  │  │                       │  │  │  │        │  └─ Error code: 0 (No error)
│  │  │  │  │  │                       │  │  │  │        └─ Command type: 0x04 (Current device values)
│  │  │  │  │  │                       │  │  │  └─ Timestamp: 5F9E1758₁₆ -> 1604196184₁₀ -> 2020-11-01T02:03:04+00:00
│  │  │  │  │  │                       │  │  └─ Packet data
│  │  │  │  │  │                       │  └─ Packet number
│  │  │  │  │  │                       └─ Command number
│  │  │  │  │  └─ Serial number: ABCD1234
│  │  │  │  └─ Serial number size: 8
│  │  │  └─ Device type: 0xA1 - Android mobile device
│  │  └─ Parameters: 03 -> 00000011 - initial and final command packet
│  └─ Protocol version: 1
└─ Packet length: 2F - 47 bytes (all fields except the length field).
````

### Data types

* All data types are transmitted little-endian (least significant byte first).

**uint8**, **uint16**, **uint32**, **uint64** — unsigned integer, 1, 2, 4, or 8 bytes respectively.

**uint48** — unsigned integer, 6 bytes. Equivalent to uint64 without the two most significant bytes.

**uint24** — unsigned integer, 3 bytes. Equivalent to uint32 without the most significant byte.

**int8**, **int16** — signed integer, 1 or 2 bytes respectively (two's complement).

**bool** — boolean, 1 byte; 0 = false, 1 = true.

**time** — UNIX timestamp (uint32, seconds since 1970-01-01 00:00:00). Local time.

**utf8** — UTF-8 encoded string.

**length** — data length (1 or 2 bytes). 1 byte: bits 0–6 are the low bits of the length; bit 7 set means a 2-byte length follows. 2nd byte: present if bit 7 of the first byte is set; bits 0–7 are the high bits of the length.

**crc16** — polynomial x16 + x15 + x2 + 1 (CRC16 Modbus). See [CRC16](#crc16) for a calculation example.

**freq** — frequency (uint16). Hz × 100 (12345 → 123.45 Hz).

**temper** — temperature (int8), degrees Celsius.

**degree** — degrees (uint16).

**decimal_degree** — decimal degrees (ieee754_64).

**percent** — percentage (uint16), percent × 100 (10000 → 100.00%).

**meter_second** — metres per second.

**ieee754_32** — [IEEE 754](https://www.softelectro.ru/ieee754.html) 32-bit floating point.

**ieee754_64** — [IEEE 754](https://www.softelectro.ru/ieee754.html) 64-bit floating point.

**time_sync** — time synchronisation type:
* 0x00 — Undefined;
* 0x01 — Manual synchronisation via "Set time" command;
* 0x02 — NTP synchronisation.

**device_type** — device type:
* 0xA0 — IoT (Internet of Things)
* 0xA1 — Android mobile device
* 0xA2 — iOS mobile device

**value_type** — value type (1 byte):
* 0 — Battery charge (percent)
* 1 — Latitude (decimal_degree)
* 2 — Longitude (decimal_degree)
* 3 — Altitude above sea level (meter)
* 4 — Accuracy of latitude and longitude (meter)
* 5 — Accuracy of altitude (meter)
* 6 — Bearing — direction of movement (degree)
* 7 — Speed of movement (meter_second)

#### CRC16

CRC16 calculation example.

In C#:
````csharp
/// CRC16 calculation
public static ushort Crc16(byte[] buffer, long index, long count)
{
    int crc = 0xFFFF;

    for (long i = index; i < index + count; i++)
    {
        crc = crc ^ buffer[i];

        for (int j = 0; j < 8; ++j)
        {
            if ((crc & 0x01) == 1)
                crc = (crc >> 1 ^ 0xA001);
            else
                crc >>= 1;
        }
    }
    return (ushort)crc;
}
````

[^crm]: **Apostol CRM** is an abstract term, not a standalone product. It refers to any project that uses both the [Apostol](https://github.com/apostoldevel/apostol) C++ framework and [db-platform](https://github.com/apostoldevel/db-platform) together through purpose-built modules and processes. Each framework can be used independently; combined, they form a full-stack backend platform.
