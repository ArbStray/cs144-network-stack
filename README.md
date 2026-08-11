# CS144 Sponge — A User-Space TCP/IP Stack

A from-scratch implementation of a TCP/IP network stack, built as part of **Stanford CS144** (Introduction to Computer Networking). The project walks the full path from a reliable in-memory byte stream up to a longest-prefix-match IP router, with **TCP Reno congestion control** and **ICMP** support added on top of the course framework.

> Built on the [Sponge](https://web.stanford.edu/class/cs144/) 2021 framework. All seven core labs (Lab 0–6) are fully implemented with real (non-dummy) code, plus three engineering enhancements described below.

---

## ✨ Highlights

This isn't just "the labs done." Three substantive enhancements were added on top of the baseline course requirements:

| Enhancement | What it adds | Where |
|-------------|--------------|-------|
| **TCP Reno Congestion Control** | Slow start, congestion avoidance, fast retransmit, fast recovery — toggleable via `TCPConfig::congestion_control` so the original flow-control-only behavior is preserved | `libsponge/tcp_sender.{hh,cc}` |
| **Trie-based IP Router** | Replaced the linear-scan routing table with a binary radix trie for O(32) longest-prefix-match lookup | `libsponge/router.{hh,cc}` |
| **ICMP Support** | Echo Request/Reply (ping), Destination Unreachable, Time Exceeded — with a from-scratch ICMP message parser/serializer | `libsponge/tcp_helpers/icmp_message.{hh,cc}` |

---

## 📚 What's Implemented

The stack is built bottom-up across seven labs:

| Layer | Module | Responsibility |
|-------|--------|----------------|
| Lab 0 | `byte_stream` | A flow-controlled in-memory reliable byte stream |
| Lab 1 | `stream_reassembler` | Reassembles out-of-order byte substrings into an in-order stream |
| Lab 2 | `tcp_receiver` | 32-bit ↔ 64-bit seqno conversion, hands payload to the reassembler, computes ACK no. & window |
| Lab 3 | `tcp_sender` | Sliding-window send, retx with exponential backoff, **+ Reno congestion control** |
| Lab 4 | `tcp_connection` | Full TCP FSM — 3-way handshake, active/passive close, RST, keep-alive, TIME_WAIT |
| Lab 5 | `network_interface` | IP↔Ethernet framing, ARP request/reply, ARP cache with TTL |
| Lab 6 | `router` | **Trie** longest-prefix-match routing, TTL decrement, **+ ICMP** |

---

## 🏗️ Architecture

```
 Application (webget / lab7)
        │
        ▼
   TCPConnection ◄──── Lab 4 (FSM: handshake / close / RST)
    ├── TCPSender  ◄── Lab 3 + Reno CC (slow start / AIMD / fast retransmit)
    └── TCPReceiver ◄─ Lab 2 (seqno unwrap / ACK / flow control)
        │
        ▼
   StreamReassembler ◄ Lab 1 (out-of-order → in-order)
        │
        ▼
      ByteStream  ◄─── Lab 0 (reliable in-memory stream)
        │
        ▼
   NetworkInterface ◄ Lab 5 (ARP / Ethernet framing)
        │
        ▼
      Router  ◄────── Lab 6 (Trie LPM routing + ICMP)
```

---

## 🚀 Build & Test

### Prerequisites

This is a **Linux-only** project — it depends on the Linux TUN/TAP kernel API, POSIX sockets, and `libpcap`. The canonical environment is Ubuntu (Stanford provides a VM). On Windows, use **WSL2**.

```bash
# Ubuntu / WSL2
sudo apt update && sudo apt install -y build-essential cmake libpcap-dev
```

### Build

```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

### Run the tests

```bash
make check_lab0   # byte stream
make check_lab1   # stream reassembler
make check_lab2   # TCP receiver
make check_lab3   # TCP sender (+ congestion control test)
make check_lab4   # TCP connection (needs TUN devices, runs sudo)
make check_lab5   # network interface / ARP (needs TAP device)
make check_lab6   # router + ICMP
```

### Build variants

CMake supports several build types — pass `-DCMAKE_BUILD_TYPE=<type>` to `cmake`:

| Type | Description |
|------|-------------|
| `Release` *(default)* | Optimizations |
| `Debug` | Debug symbols, `-Og` |
| `RelASan` | Release + AddressSanitizer + UBSan |
| `RelTSan` | Release + ThreadSanitizer |
| `DebugASan` | Debug + ASan + UBSan |
| `DebugTSan` | Debug + ThreadSan |

---

## 🔬 The Enhancements in Detail

### 1. TCP Reno Congestion Control

The baseline `TCPSender` only applies **flow control** (the receiver's advertised window). This project adds a full **TCP Reno** implementation behind a config flag:

- **Slow start** — `cwnd` starts at 1 MSS and grows by `newly_acked_bytes` per ACK until `ssthresh`
- **Congestion avoidance** — additive increase of ~1 MSS/RTT above `ssthresh`
- **Fast retransmit** — 3 duplicate ACKs trigger immediate retransmit of the oldest unacked segment
- **Fast recovery** — `ssthresh = max(cwnd/2, 2·MSS)`, `cwnd = ssthresh + 3·MSS`, inflate on further dupacks, deflate on new ACK

```cpp
TCPConfig cfg;
cfg.congestion_control = true;   // opt in; default false preserves original behavior
```

Default-off means **zero regression** on the original `send_*` test suite, which assumes no congestion control.

### 2. Trie-based Longest-Prefix-Match Routing

The original `Router::route_one_datagram` did an O(n) linear scan of a `std::vector` route table. This project introduces a `RoutingTrie` — a binary radix tree indexed by the destination IP's bits (MSB first):

- **Insert** — O(prefix_length)
- **Lookup** — O(32), walking the trie and remembering the last node carrying a route
- **Memory** — `std::unique_ptr`-managed nodes, auto-cleaned

For a 100k-entry table, lookup drops from ~3.2M operations to ~320.

### 3. ICMP

A from-scratch `ICMPMessage` class (`parse` / `serialize` / `compute_checksum`) implementing RFC 792:

| Type | Name | Trigger |
|------|------|---------|
| 0 | Echo Reply | Router receives Echo Request addressed to one of its own interface IPs |
| 3 | Destination Unreachable | No route matches the destination (opt-in via `router.set_icmp_enabled(true)`) |
| 8 | Echo Request | (parsed, triggers Reply) |
| 11 | Time Exceeded | TTL reaches 0 in transit (opt-in) |

Error messages carry the original IP header + first 8 payload bytes per RFC 792.

---

## 📁 Project Structure

```
.
├── libsponge/                 # Core library — all student code lives here
│   ├── byte_stream.{hh,cc}        # Lab 0
│   ├── stream_reassembler.{hh,cc} # Lab 1
│   ├── tcp_receiver.{hh,cc}       # Lab 2
│   ├── tcp_sender.{hh,cc}         # Lab 3 + Reno congestion control
│   ├── tcp_connection.{hh,cc}     # Lab 4
│   ├── network_interface.{hh,cc}  # Lab 5
│   ├── router.{hh,cc}             # Lab 6 + Trie routing + ICMP
│   ├── tcp_helpers/               # Provided: Ethernet/IPv4/TCP headers, ARP, TUN adapter
│   │   └── icmp_message.{hh,cc}   # NEW — ICMP parser/serializer
│   └── util/                      # Provided: Address, Buffer, Parser, sockets
├── apps/                      # Executables: webget, tcp_ipv4, lab7, network_simulator, ...
├── tests/                     # Automated test harnesses (send_*, recv_*, fsm_*, net_interface)
├── doctests/                  # Doxygen examples
├── etc/                       # CMake scripts, Doxygen config
├── labs_pdf_21/               # Official lab PDFs
├── writeups/                  # Per-lab notes (lab0.md – lab7.md)
└── CMakeLists.txt
```

---

## 📝 Notes

- **Platform**: Linux (Ubuntu / WSL2). Not buildable on Windows native — the TUN/TAP and POSIX socket APIs are Linux-only.
- **Origin**: Built on Stanford's [Sponge](https://web.stanford.edu/class/cs144/) 2021 framework. Lab PDFs are included in `labs_pdf_21/` for reference.
- **Testing**: All seven labs pass their `make check_labN` suites (verified on Linux/WSL2). The three enhancements are additive and gated behind config flags so they don't regress the baseline tests.

---

## 📜 License

Educational project based on Stanford CS144 course materials. See the original [CS144 site](https://web.stanford.edu/class/cs144/) for course-specific terms.
