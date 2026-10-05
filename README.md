# filexferlib

A modern C++17 file transfer and sync library over TCP.

Simple API, async-first, with progress, resume, sync, compression, integrity verification, and bandwidth throttling.

> **Status:** v1.0.0 — stable. API is frozen for 1.x.

[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![Version](https://img.shields.io/badge/version-1.0.0-blue.svg)](https://github.com/MortezaAghazamani/filexferlib/releases)
[![CI](https://github.com/MortezaAghazamani/filexferlib/actions/workflows/ci.yml/badge.svg)](https://github.com/MortezaAghazamani/filexferlib/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-lightgrey.svg)]()
[![Tests](https://img.shields.io/badge/tests-50%2B-brightgreen.svg)]()

---

## Why filexferlib?

Moving files between machines should not require a framework. `filexferlib` gives you a small, complete, and dependable toolkit for transferring and syncing files over TCP — without pulling in a heavy dependency, without hiding what happens under the hood, and without making you write the boring parts yourself.

- **Simple by design.** A connect, an upload, a disconnect. That's it.
- **Async without ceremony.** Everything returns `std::future`. No threads to manage, no callbacks to wire.
- **Fast.** Pipelining, adaptive chunk size, optional zstd compression.
- **Safe.** XXHash3 verification on every transfer, automatic resume, secure path resolution.
- **Predictable.** Same API shape for upload, download, and sync. Exception-based and exception-free variants for every method.
- **Cross-platform.** Windows (MSVC 2019+) and Linux (GCC 9+). Continuously tested on both.
- **Well-tested.** 50+ tests covering units, edge cases, integrations, and stress scenarios.

---

## A taste of the API

### Server

```cpp
#include <filexferlib/server.hpp>
#include <iostream>
#include <thread>

using namespace filexferlib;

int main() {
    ServerConfig cfg;
    cfg.rootDir = "D:/shared";              // root for all remote paths
    cfg.requiredAuthToken = "my-secret";    // optional shared token
    cfg.maxBytesPerSecond = 10 * 1024 * 1024;  // 10 MB/s (both directions)

    Server server(cfg);

    // Optional: register a custom method the client can call.
    server.registerHandler("echo", [](GenericContext& ctx) {
        ctx.respond(ctx.request());
    });

    auto port = server.start("0.0.0.0", 9000);
    std::cout << "server listening on port " << port << "\n";
    std::cout << "root: " << cfg.rootDir << "\n";
    std::cout << "press Ctrl+C to stop\n";

    std::this_thread::sleep_for(std::chrono::hours(24));
}
```

### Client

```cpp
#include <filexferlib/client.hpp>
#include <iostream>

using namespace filexferlib;

int main() {
    Client client;

    ConnectOptions opts;
    opts.authToken = "my-secret";
    opts.autoReconnect = true;

    // Connect — no exceptions, returns an Outcome.
    auto cr = client.tryConnect("127.0.0.1", 9000, opts);
    if (!cr) {
        std::cerr << "connect failed: " << cr.error().message()
                  << " (" << cr.message() << ")\n";
        return 1;
    }
    std::cout << "connected: " << cr.value().serverVersion << "\n";

    // Upload with progress — also exception-free.
    auto ur = client.tryUpload("C:/data/file.bin", "/backup/file.bin", {},
        [](const TransferProgress& p) {
            std::cout << "\r" << p.percent() << "% "
                      << p.speedMBps() << " MB/s" << std::flush;
        }).get();

    if (!ur) {
        std::cerr << "\nupload failed: " << ur.error().message()
                  << " (" << ur.message() << ")\n";
        client.disconnect();
        return 1;
    }
    std::cout << "\nuploaded " << ur.value().bytesTransferred
              << " bytes, verified="
              << (ur.value().verified ? "yes" : "no") << "\n";

    client.disconnect();
    return 0;
}
```

That's the whole thing. Copy, paste, build, run.

---

## Features

- **Async API** — `std::future` based
- **Upload / Download** with chunked transfer and progress callbacks
- **XXHash3 verification** of every transfer
- **Resume** interrupted uploads and downloads
- **Pause / Resume / Cancel** live transfers
- **Pipelining** (sliding window) for high throughput
- **Compression (zstd)** — optional per-chunk compression
- **Bandwidth throttling** — one config controls both directions
- **Adaptive chunk size** — auto-tunes to rate limit
- **Sync** two folders: Upload, Download, MirrorUpload, MirrorDownload, Bidirectional
- **Single-file sync** — `syncFile()` for one-off comparisons
- **Dry run** — preview sync operations without executing
- **Server-side operations** — copy, rename/move, delete without transferring data
- **Auth** via shared token
- **Auto-reconnect** with exponential backoff
- **Exception-free API** — every method has a `try*` variant
- **Secure path resolution** inside the server root
- **Zero-cost logging** in release builds
- **Graceful shutdown** on Ctrl+C

---

## Requirements

- **C++17**
- **[Asio](https://think-async.com/Asio/)** (standalone or Boost.Asio)
- **[xxHash](https://github.com/Cyan4973/xxHash)**
- **[zstd](https://github.com/facebook/zstd)**
- **CMake** 3.20+
- **Compiler**: MSVC 2019+, GCC 9+, or Clang 10+

---

## Build

### Using vcpkg

```bash
vcpkg install asio xxhash zstd

# Debug build (with logging)
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Debug

# Release build (zero-cost logging)
cmake -B build-rel -S . \
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake \
  -DFILEXFERLIB_DISABLE_LOGGING=ON
cmake --build build-rel --config Release
```

### With system packages (Linux)

```bash
sudo apt install libasio-dev libxxhash-dev libzstd-dev cmake g++

cmake -B build -S .
cmake --build build -j
```

---

## Sync modes

`syncFolder(local, remote, opts, ...)` uses `SyncOptions::mode` to control
direction and deletion. Only `MirrorUpload` and `MirrorDownload` propagate
deletions.

| Mode | Direction | Deletes extras? | Notes |
|---|---|---|---|
| `Upload` | local → remote | no | Local is source of truth |
| `Download` | remote → local | no | Remote is source of truth |
| `MirrorUpload` | local → remote | yes (on remote) | Remote becomes exact copy of local |
| `MirrorDownload` | remote → local | yes (on local) | Local becomes exact copy of remote |
| `Bidirectional` | both ways | no | Newer (by mtime) wins |

```cpp
SyncOptions o;
o.mode = SyncMode::Upload;
o.compareBy = CompareMethod::XXHash;

auto r = client.syncFolder("C:/local", "/backup", o).get();
std::cout << "uploaded=" << r.uploaded
          << " skipped="  << r.skipped
          << " failed="   << r.failed << "\n";
```

→ Full examples in [docs/EXAMPLES.md](docs/EXAMPLES.md#6-sync-a-folder).

---

## Comparison methods

`CompareMethod` decides when two files are considered the same.

| Method | Compares | Speed | Accuracy |
|---|---|---|---|
| `SizeOnly` | size | fastest | lowest |
| `SizeAndMtime` | size + mtime | fast | **recommended** |
| `XXHash` | XXHash3 digest | slow | highest |

Use `SizeAndMtime` unless mtimes are unreliable (different filesystems,
timezone issues). Use `XXHash` when you cannot trust mtimes. Use
`SizeOnly` only for very large trees where speed matters more than
correctness.

---

## Compression

Optional, off by default. When enabled, each chunk is compressed with
[zstd](https://github.com/facebook/zstd) before being sent over the wire.
Hash verification always runs on the **uncompressed** data.

```cpp
CopyOptions o;
o.compression = Compression::Zstd;
o.compressionLevel = 3;   // 1 (fast) to 22 (max)

client.upload("large.log", "/backup/large.log", o).get();
```

Best for text, logs, CSV, JSON, XML, source code (5–10× smaller).
For already-compressed files (JPEG, MP4, ZIP), zstd detects this
automatically and sends raw — you can safely enable it globally.

→ Full examples in [docs/EXAMPLES.md](docs/EXAMPLES.md#8-compression).

---

## Exception-free API

Every throwing method has a `try*` variant that returns an `Outcome<T>`
instead. Use these when you don't want exceptions.

```cpp
auto r = client.tryConnect("127.0.0.1", 9000, opts);
if (!r) {
    std::cerr << "connect failed: " << r.error().message() << "\n";
    return 1;
}
std::cout << "connected: " << r.value().serverVersion << "\n";
```

`Outcome<T>` holds either a value or an `std::error_code` + message.
Always check `ok()` (or `if (!r)`) before calling `value()`.

→ Full list and details in
[docs/API_GUIDE.md](docs/API_GUIDE.md#outcome) and
[docs/EXAMPLES.md](docs/EXAMPLES.md#5-exception-free-api).

---

## API reference

### `Client`

| Method | Purpose |
|---|---|
| `connect` / `tryConnect` | Connect (blocking) |
| `connectAsync` / `tryConnectAsync` | Connect, returns `future` |
| `upload` / `tryUpload` | Upload a file |
| `download` / `tryDownload` | Download a file |
| `uploadWithHandle` / `downloadWithHandle` | With pause/resume/cancel |
| `syncFolder` / `trySyncFolder` | Sync two folders |
| `syncFile` / `trySyncFile` | Sync a single file |
| `copyRemoteToRemote` / `tryCopyRemoteToRemote` | Server-side copy |
| `renameRemote` / `tryRenameRemote` | Rename or move on server |
| `listRemote` / `tryListRemote` | List a remote directory |
| `deleteRemote` / `tryDeleteRemote` | Delete a file or directory |
| `setRemoteMtime` / `trySetRemoteMtime` | Set remote file mtime |
| `mkdirRemote` / `tryMkdirRemote` | Create a remote directory |
| `call` / `tryCall` | Generic request/response |
| `disconnect` | Close the session |

### `Server`

| Method | Purpose |
|---|---|
| `start` | Bind and start listening |
| `stop` | Graceful shutdown |
| `registerHandler` | Register a generic handler |

### Configuration types

| Type | Purpose |
|---|---|
| `ServerConfig` | Server settings (root, auth, rate limit, window) |
| `ConnectOptions` | Client connection settings (auth, timeouts, reconnect) |
| `CopyOptions` | Per-transfer settings (chunk, hash, compression) |
| `SyncOptions` | Sync settings (mode, comparison, filters, dry run) |

→ Full field list in
[docs/API_GUIDE.md](docs/API_GUIDE.md#configuration).

---

## Documentation

- **[docs/EXAMPLES.md](docs/EXAMPLES.md)** — 20+ working examples, one per task
- **[docs/API_GUIDE.md](docs/API_GUIDE.md)** — full reference for every method, type, and error code
- **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** — protocol, state machines, data flow
- **[CHANGELOG.md](CHANGELOG.md)** — version history

---

## Build options

| CMake option | Default | Purpose |
|---|---|---|
| `FILEXFERLIB_BUILD_TESTS` | ON | Build tests |
| `FILEXFERLIB_BUILD_EXAMPLES` | ON | Build example programs |
| `FILEXFERLIB_BUILD_BENCH` | ON | Build benchmark |
| `FILEXFERLIB_DISABLE_LOGGING` | OFF | Zero-cost logging (compile-time) |

---

## Quality

- **50+ tests** across five suites:
  - `test_protocol` — framing encode/decode
  - `test_units` — encoding, hashing, rate limiter, logger
  - `test_edge` — empty files, unicode, deep trees, path traversal
  - `test_integration` — end-to-end: upload, download, sync, resume,
    throttle, compression, rename, reconnect
  - `test_stress` — 100 parallel files, 256 MB file, 5 concurrent clients
- **Continuous integration** on **Linux (GCC)** and **Windows (MSVC)**
  via GitHub Actions.
- **Zero-cost logging** available at compile time.

Run locally:

```bash
ctest --test-dir build -C Release --output-on-failure
```

---

## Benchmark

```bash
./build/Release/bench_transfer
```

Measures throughput with different chunk sizes, pipelining windows,
verify on/off, throttle, and small-file latency.

---

## License

MIT — see [LICENSE](LICENSE).

Copyright (c) 2025 Morteza Aghazamani
