# sikradio 📻

![C++](https://img.shields.io/badge/c++-17-blue.svg)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS-lightgrey)
![Dependencies](https://img.shields.io/badge/dependencies-OpenSSL-brightgreen.svg)

A robust, non-blocking console internet radio client written in C++17.

The application connects to standard and encrypted internet radio streams, demultiplexes interleaved audio and ICY metadata using a deterministic finite-state machine (FSM), and pipes the raw audio stream to standard output for playback via external media players.

## Core Architecture

This project was built to demonstrate low-level system and network programming concepts:
* **Custom Network Interface:** An abstract `NetConn` layer manages both standard TCP (`TcpConn`) and encrypted TLS/SSL (`SslConn`) connections using raw POSIX sockets and OpenSSL. Server Name Indication (SNI) is natively supported.
* **I/O Multiplexing:** The core event loop uses `poll()` to simultaneously monitor network sockets and `STDIN`. This ensures non-blocking execution and allows graceful termination without threading overhead.
* **Deterministic Demultiplexing:** Interleaved ICY metadata and audio streams are separated on the fly using a custom state machine (`StreamProcessor`). Audio is piped directly to `STDOUT`, while metadata is routed to `STDERR`.
* **Resilience:** Implements socket-level hardware timeouts (`SO_RCVTIMEO`, `SO_SNDTIMEO`) to mitigate slowloris-style network stalling, along with automatic HTTP redirect resolution.

## Build Requirements

The project relies on POSIX APIs and the OpenSSL library.

* A C++17 compliant compiler (`g++` or `clang++`)
* GNU Make
* OpenSSL development headers (`libssl-dev` or `openssl`)

### Compilation

Clone the repository and build using the provided `Makefile`:

```bash
git clone https://github.com/Pavelpuff/cpp-internet-radio-client.git
cd cpp-internet-radio-client
make
```

## Usage

`sikradio` outputs raw audio data to `STDOUT`. You must pipe this output to a media player capable of reading from standard input, such as `mpv` or `sox` (via the `play` command).

### Basic Streaming
Connect to a stream and play it using `mpv`:
```bash
./sikradio -u http://example.com/stream | mpv --really-quiet -
```

### Advanced Options
Request ICY metadata (`-m`) to see the currently playing track on `STDERR`, and set a custom socket timeout of 2 seconds (`-t 2000`):
```bash
./sikradio -u https://secure-stream.net/live -m -t 2000 | mpv --really-quiet -
```

Force IPv4 resolution, disable metadata, and reduce verbosity to quiet mode (`-q`):
```bash
./sikradio -u http://ipv4-only.com:8080/ -4 -q | mpv -
```

### CLI Arguments

| Flag | Description |
|------|-------------|
| `-u <url>` | **(Required)** The URL of the internet radio stream. Supports HTTP/HTTPS and IPv4/IPv6. |
| `-m` | Request ICY metadata from the server (multiplexed text information). |
| `-t <ms>` | Connection and read timeout in milliseconds (100 - 100000). Default is 5000. |
| `-4` | Force IPv4 DNS resolution. |
| `-6` | Force IPv6 DNS resolution. |
| `-v <level>`| Verbosity level (0 to 4). Default is 2. |
| `-q` | Quiet mode. Equivalent to `-v 0`. |

*Note: You can gracefully terminate the client at any time by typing `quit` into the console and pressing Enter.*