# Examples

20+ working examples, one per task. Every snippet is complete and
ready to copy-paste.

All examples assume:

```cpp
using namespace filexferlib;
```

---

## Table of contents

1. [Basic upload](#1-basic-upload)
2. [Basic download](#2-basic-download)
3. [Upload with progress](#3-upload-with-progress)
4. [Pause / resume / cancel](#4-pause--resume--cancel)
5. [Exception-free API](#5-exception-free-api)
6. [Cancel with a token](#6-cancel-with-a-token)
7. [Sync a folder](#7-sync-a-folder)
8. [Compression](#8-compression)
9. [Server-side operations](#9-server-side-operations)
10. [Custom generic method](#10-custom-generic-method)
11. [Auto-reconnect](#11-auto-reconnect)
12. [Bandwidth throttling](#12-bandwidth-throttling)
13. [Logging](#13-logging)
14. [Error handling](#14-error-handling)
15. [Concurrent uploads](#15-concurrent-uploads)
16. [Resume after network drop](#16-resume-after-network-drop)
17. [Complete example: backup](#17-complete-example-backup)
18. [Complete example: download mirror](#18-complete-example-download-mirror)

---

## 1. Basic upload

```cpp
Client c;
c.connect("127.0.0.1", 9000);
c.upload("C:/data/file.bin", "/backup/file.bin").get();
c.disconnect();
```

---

## 2. Basic download

```cpp
Client c;
c.connect("127.0.0.1", 9000);
c.download("/backup/file.bin", "C:/data/file.bin").get();
c.disconnect();
```

---

## 3. Upload with progress

```cpp
auto fut = c.upload("big.iso", "/big.iso", {},
    [](const TransferProgress& p) {
        std::cout << "\r" << p.percent() << "% "
                  << p.speedMBps() << " MB/s" << std::flush;
    });
auto result = fut.get();
std::cout << "\nverified=" << result.verified << "\n";
```

The callback is throttled by `CopyOptions::progressInterval` (default
50 ms), so you won't get thousands of updates per second.

---

## 4. Pause / resume / cancel

```cpp
auto res = c.uploadWithHandle("big.iso", "/big.iso");

std::this_thread::sleep_for(std::chrono::seconds(2));
res.handle.pause();
std::cout << "paused\n";

std::this_thread::sleep_for(std::chrono::seconds(5));
res.handle.resume();
std::cout << "resumed\n";

auto result = res.future.get();
```

To cancel instead of resume:

```cpp
res.handle.cancel();
```

Check the state at any time:

```cpp
switch (res.handle.state()) {
case TransferHandle::State::Running:   /* ... */ break;
case TransferHandle::State::Paused:    /* ... */ break;
case TransferHandle::State::Done:      /* ... */ break;
case TransferHandle::State::Cancelled: /* ... */ break;
case TransferHandle::State::Failed:    /* ... */ break;
}
```

---

## 5. Exception-free API

Every throwing method has a `try*` variant that returns an
`Outcome<T>` instead of raising an exception. Use these when you want
to avoid exceptions (e.g. in `noexcept` contexts, or when mixing with
C-style code).

### Connect

```cpp
Client c;
ConnectOptions opts;
opts.authToken = "my-secret";

auto r = c.tryConnect("127.0.0.1", 9000, opts);
if (!r) {
    std::cerr << "connect failed: "
              << r.error().message()
              << " (" << r.message() << ")\n";
    return 1;
}
std::cout << "connected: " << r.value().serverVersion << "\n";
```

### Upload

```cpp
auto u = c.tryUpload("big.iso", "/big.iso").get();
if (!u) {
    std::cerr << "upload failed: " << u.error().message() << "\n";
    return 1;
}
std::cout << "uploaded " << u.value().bytesTransferred << " bytes\n";
```

### List a directory

```cpp
auto l = c.tryListRemote("/backup").get();
if (l) {
    for (auto& m : l.value()) {
        std::cout << m.relativePath << "\n";
    }
} else {
    std::cerr << "list failed: " << l.message() << "\n";
}
```

### Important rule

If `ok()` is `false`, calling `value()` throws `std::logic_error`.
Always check `ok()` (or `if (!r)`) before accessing the value.

```cpp
auto r = c.tryUpload("a", "/a").get();
if (r) {
    use(r.value());       // ✅ safe
}

// use(r.value());        // ❌ would throw if r is an error
```

### Full list of `try*` methods

| Exception version | Exception-free version |
|---|---|
| `connect` | `tryConnect` |
| `connectAsync` | `tryConnectAsync` |
| `upload` | `tryUpload` |
| `download` | `tryDownload` |
| `syncFolder` | `trySyncFolder` |
| `syncFile` | `trySyncFile` |
| `listRemote` | `tryListRemote` |
| `deleteRemote` | `tryDeleteRemote` |
| `setRemoteMtime` | `trySetRemoteMtime` |
| `mkdirRemote` | `tryMkdirRemote` |
| `renameRemote` | `tryRenameRemote` |
| `copyRemoteToRemote` | `tryCopyRemoteToRemote` |
| `call` | `tryCall` |

---

## 6. Cancel with a token

```cpp
auto cancel = make_cancellation_token();
std::thread w([&]{
    std::this_thread::sleep_for(std::chrono::seconds(3));
    cancel->store(true);
});

try {
    c.upload("big.iso", "/big.iso", {}, {}, cancel).get();
} catch (const std::system_error& e) {
    if (e.code() == make_error_code(ErrorCode::Cancelled))
        std::cout << "cancelled\n";
}
w.join();
```

You can pass the same token to multiple transfers to cancel them all
at once.

---

## 7. Sync a folder

### Upload (backup local to remote, no deletion)

```cpp
SyncOptions o;
o.mode = SyncMode::Upload;              // local -> remote
o.compareBy = CompareMethod::XXHash;
o.preserveTimes = true;

auto r = c.syncFolder("C:/local", "/backup", o).get();
std::cout << "uploaded=" << r.uploaded
          << " skipped=" << r.skipped
          << " failed=" << r.failed << "\n";
```

### Download (remote is source of truth)

```cpp
SyncOptions o;
o.mode = SyncMode::Download;            // remote -> local
o.compareBy = CompareMethod::SizeAndMtime;

auto r = c.syncFolder("C:/local", "/backup", o).get();
std::cout << "downloaded=" << r.downloaded
          << " skipped=" << r.skipped << "\n";
```

### MirrorUpload (remote becomes exact copy of local)

```cpp
SyncOptions o;
o.mode = SyncMode::MirrorUpload;
o.compareBy = CompareMethod::XXHash;
o.deleteExtra = true;                   // required to actually delete

auto r = c.syncFolder("C:/local", "/backup", o).get();
std::cout << "uploaded=" << r.uploaded
          << " deleted=" << r.deleted
          << " failed=" << r.failed << "\n";
```

### MirrorDownload (local becomes exact copy of remote)

```cpp
SyncOptions o;
o.mode = SyncMode::MirrorDownload;
o.compareBy = CompareMethod::XXHash;
o.deleteExtra = true;

auto r = c.syncFolder("C:/local", "/backup", o).get();
std::cout << "downloaded=" << r.downloaded
          << " deleted=" << r.deleted
          << " failed=" << r.failed << "\n";
```

### Bidirectional (newer wins on conflicts)

```cpp
SyncOptions o;
o.mode = SyncMode::Bidirectional;
o.compareBy = CompareMethod::XXHash;

auto r = c.syncFolder("C:/local", "/backup", o).get();
std::cout << "uploaded=" << r.uploaded
          << " downloaded=" << r.downloaded << "\n";
```

### Dry run (preview only)

```cpp
SyncOptions o;
o.mode = SyncMode::MirrorUpload;
o.deleteExtra = true;
o.dryRun = true;

auto r = c.syncFolder("C:/local", "/backup", o).get();
std::cout << "would upload=" << r.uploaded
          << " would delete=" << r.deleted << "\n";
```

### With a filter

```cpp
SyncOptions o;
o.mode = SyncMode::Upload;
o.filter = [](const std::string& rel) {
    return rel.find("/cache/") == std::string::npos &&
           rel.find("/tmp/") == std::string::npos;
};
c.syncFolder("C:/local", "/backup", o).get();
```

### With progress

```cpp
SyncOptions o;
o.mode = SyncMode::Upload;
o.compareBy = CompareMethod::XXHash;

auto r = c.syncFolder("C:/local", "/backup", o,
    [](const SyncProgress& sp) {
        std::cout << "\r" << sp.overallPercent() << "% "
                  << sp.currentOperation << " "
                  << sp.currentPath << "        " << std::flush;
    }).get();
```

### Sync a single file

```cpp
SyncOptions o;
o.mode = SyncMode::Upload;
o.compareBy = CompareMethod::XXHash;

auto r = c.syncFile("C:/local/config.json",
                    "/backup/config.json", o).get();

switch (r.action) {
case SyncFileResult::Action::Uploaded:    std::cout << "uploaded\n"; break;
case SyncFileResult::Action::Downloaded:  std::cout << "downloaded\n"; break;
case SyncFileResult::Action::Skipped:     std::cout << "in sync\n"; break;
case SyncFileResult::Action::DeletedLocal:
case SyncFileResult::Action::DeletedRemote:
    std::cout << "deleted\n"; break;
case SyncFileResult::Action::None:
    std::cout << "not found\n"; break;
}
```

---

## 8. Compression

Enable zstd compression for a transfer:

```cpp
CopyOptions o;
o.compression = Compression::Zstd;
o.compressionLevel = 3;   // 1 (fast) to 22 (max)

c.upload("large.log", "/backup/large.log", o).get();
```

Compressed download:

```cpp
CopyOptions o;
o.compression = Compression::Zstd;

c.download("/backup/large.log", "large.log", o).get();
```

Compression is best for text files (logs, CSV, JSON, source code).
For already-compressed files (JPEG, MP4, ZIP), zstd detects this
automatically and sends raw data — you can safely enable it globally.

For maximum compression (slow):

```cpp
o.compressionLevel = 19;
```

For fastest (minimal compression):

```cpp
o.compressionLevel = 1;
```

Hash verification always runs on the **uncompressed** data, so
enabling compression does not change the integrity guarantees.

---

## 9. Server-side operations

All of these run entirely on the server — no data crosses the network.

### Copy a file

```cpp
c.copyRemoteToRemote("/a/file.bin", "/b/file.bin").get();
```

### Rename a file (same directory)

```cpp
c.renameRemote("/old.txt", "/new.txt").get();
```

### Move a file (different directory)

```cpp
c.renameRemote("/incoming/file.bin",
               "/archive/2025/file.bin").get();
```

### Rename or move a directory

```cpp
c.renameRemote("/incoming", "/archive/2025-01").get();
```

Parent directories are created automatically.

### Delete

```cpp
c.deleteRemote("/old_file.bin").get();          // file
c.deleteRemote("/old_dir", true).get();         // directory (recursive)
```

### Create a directory

```cpp
c.mkdirRemote("/new_dir").get();                // creates parents
c.mkdirRemote("/new_dir", false).get();         // single level
```

### List a directory

```cpp
auto list = c.listRemote("/backup").get();
for (auto& m : list) {
    std::cout << (m.isDirectory ? "d " : "- ")
              << m.relativePath
              << "  (" << m.size << " bytes)\n";
}
```

### Set modification time

```cpp
// mtimeNs is nanoseconds since Unix epoch
auto now = std::chrono::system_clock::now();
auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
    now.time_since_epoch()).count();

c.setRemoteMtime("/backup/file.bin", ns).get();
```

---

## 10. Custom generic method

### Server: register a handler

```cpp
server.registerHandler("sum", [](GenericContext& ctx) {
    const auto& in = ctx.request();
    if (in.size() % 4 != 0) { ctx.respond({}, 2); return; }

    std::int64_t s = 0;
    for (std::size_t i = 0; i + 4 <= in.size(); i += 4) {
        std::int32_t v;
        std::memcpy(&v, in.data() + i, 4);
        s += v;
    }

    ByteBuffer out(8);
    std::memcpy(out.data(), &s, 8);
    ctx.respond(std::move(out));
});
```

### Client: call it

```cpp
ByteBuffer req(12);
std::int32_t vals[3] = { 10, 20, 12 };
std::memcpy(req.data(), vals, sizeof(vals));

auto resp = c.call("sum", std::move(req)).get();
std::int64_t total = 0;
std::memcpy(&total, resp.payload.data(), 8);
std::cout << "sum = " << total << "\n";   // 42
```

### With a timeout

```cpp
CallOptions co;
co.timeout = std::chrono::seconds(5);

auto resp = c.call("slow.method", std::move(req), co).get();
```

### With error handling

```cpp
auto resp = c.tryCall("maybe.missing").get();
if (resp) {
    if (resp.value().statusCode == 0) {
        // success
    } else {
        // server returned an error code
    }
} else {
    // network or protocol error
}
```

---

## 11. Auto-reconnect

```cpp
ConnectOptions o;
o.authToken = "my-secret";
o.autoReconnect = true;
o.maxReconnectAttempts = 20;
o.reconnectBaseDelay = std::chrono::milliseconds(500);
o.reconnectMaxDelay = std::chrono::seconds(10);

c.connect("127.0.0.1", 9000, o);
// If the session drops, the client reconnects automatically
// with exponential backoff.
```

If you want to know when a reconnect happens, watch the logger:

```cpp
Logger::instance().setSink([](LogLevel lv, const std::string& msg) {
    std::cout << msg << "\n";
});
Logger::instance().setLevel(LogLevel::Info);
```

---

## 12. Bandwidth throttling

One config on the server controls both directions (upload and download):

```cpp
ServerConfig cfg;
cfg.rootDir = "D:/shared";
cfg.maxBytesPerSecond = 1024 * 1024;   // 1 MB/s total

Server server(cfg);
server.start("0.0.0.0", 9000);
```

With this config:

- A single client gets at most 1 MB/s combined.
- Multiple clients share the same budget.
- The server also advertises a `recommendedChunkSize` to the client so
  that progress callbacks remain smooth even at low rates.

To disable throttling, leave `maxBytesPerSecond = 0` (default).

---

## 13. Logging

Logging is off by default in release builds (zero-cost). Enable it at
runtime:

```cpp
#include <filexferlib/logger.hpp>

using namespace filexferlib;

int main() {
    // Send logs to std::cout
    Logger::instance().setSink([](LogLevel lv, const std::string& msg) {
        std::cout << msg << "\n";
    });

    // Levels: Trace, Debug, Info, Warn, Error, Off
    Logger::instance().setLevel(LogLevel::Info);

    Client c;
    c.connect("127.0.0.1", 9000);
    // Logs from the client will now appear.
}
```

Send logs to a file:

```cpp
#include <fstream>
#include <memory>

auto logFile = std::make_shared<std::ofstream>("filexfer.log",
                                               std::ios::app);

Logger::instance().setSink([logFile](LogLevel lv, const std::string& msg) {
    (*logFile) << msg << "\n";
});
Logger::instance().setLevel(LogLevel::Debug);
```

### Zero-cost in production

Disable logging entirely at compile time by defining
`FILEXFERLIB_DISABLE_LOGGING`:

```bash
cmake -B build -S . -DFILEXFERLIB_DISABLE_LOGGING=ON
cmake --build build --config Release
```

Every `FX_*` macro becomes `((void)0)` — the compiler removes it
completely.

---

## 14. Error handling

`filexferlib` uses `std::error_code` and exceptions in two ways.

### Exception-based

```cpp
try {
    c.connect("127.0.0.1", 9000, opts);
    c.upload("a.bin", "/a.bin").get();
} catch (const std::system_error& e) {
    std::cerr << "system error: " << e.code().message() << "\n";
} catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
}
```

### Exception-free (`try*` + `Outcome`)

```cpp
auto r = c.tryConnect("127.0.0.1", 9000, opts);
if (!r) {
    std::cerr << "code: "     << r.error().value()    << "\n";
    std::cerr << "category: " << r.error().category().name() << "\n";
    std::cerr << "message: "  << r.error().message()  << "\n";
    std::cerr << "detail: "   << r.message()          << "\n";
}
```

### Error codes

| Code | Meaning |
|---|---|
| `Ok` | Success |
| `NetworkError` | Socket error |
| `ProtocolError` | Malformed frame |
| `VersionMismatch` | Protocol version mismatch |
| `PayloadTooLarge` | Frame exceeds limit |
| `MethodNotFound` | Generic method not registered |
| `PathOutsideRoot` | Path traversal attempt |
| `PathNotFound` | Remote path does not exist |
| `PermissionDenied` | Filesystem permission error |
| `Cancelled` | Transfer cancelled by user |
| `Timeout` | Transfer timed out |
| `InternalError` | Server-side error |
| `HashMismatch` | XXHash verification failed |
| `AlreadyExists` | Destination exists and overwrite is off |

### Checking a specific error

```cpp
auto r = c.tryUpload("a.bin", "/a.bin").get();
if (!r && r.error() == make_error_code(ErrorCode::Cancelled)) {
    std::cout << "user cancelled the upload\n";
}
```

---

## 15. Concurrent uploads

Upload multiple files at the same time:

```cpp
std::vector<std::future<TransferResult>> futures;

for (const auto& file : {"a.bin", "b.bin", "c.bin"}) {
    std::string remote = "/backup/" + std::string(file);
    futures.push_back(c.upload(file, remote));
}

for (auto& f : futures) {
    auto r = f.get();
    std::cout << "uploaded " << r.bytesTransferred << " bytes\n";
}
```

### With a concurrency limit

If you have hundreds of files, limit concurrency to avoid exhausting
sockets:

```cpp
constexpr std::size_t kMaxConcurrent = 4;
std::vector<std::string> files = /* ... */;
std::size_t next = 0;

std::vector<std::future<TransferResult>> running;

auto launch = [&]() {
    while (running.size() < kMaxConcurrent && next < files.size()) {
        running.push_back(c.upload(
            files[next],
            "/backup/" + files[next]));
        ++next;
    }
};

launch();
while (!running.empty()) {
    auto r = running.front().get();
    running.erase(running.begin());
    // use r ...
    launch();
}
```

---

## 16. Resume after network drop

Both upload and download resume automatically. The pattern is:

```cpp
try {
    c.upload("big.iso", "/big.iso").get();
} catch (const std::exception& e) {
    // The connection dropped.
    std::cerr << "upload interrupted: " << e.what() << "\n";

    // Wait for reconnect (or just retry).
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Retry — the server has a .fxfer.tmp, so it resumes.
    auto r = c.upload("big.iso", "/big.iso").get();
    std::cout << "resumed, verified=" << r.verified << "\n";
}
```

For downloads, the client keeps a `<local>.fxfer.part` file and tells
the server where to continue from.

To disable automatic resume (start fresh every time), delete the
partial file before retrying:

```cpp
std::filesystem::remove("big.iso.fxfer.part");
```

---

## 17. Complete example: backup

Back up a local folder to a remote server, with progress and error
reporting.

```cpp
#include <filexferlib/client.hpp>
#include <filexferlib/logger.hpp>
#include <iostream>

using namespace filexferlib;

int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "usage: backup <host> <port> "
                     "<local-dir> <remote-dir>\n";
        return 1;
    }

    Logger::instance().setSink([](LogLevel, const std::string& m){
        std::cout << m << "\n";
    });
    Logger::instance().setLevel(LogLevel::Info);

    Client client;
    ConnectOptions opts;
    opts.authToken = "my-secret";
    opts.autoReconnect = true;

    client.connect(argv[1],
                   static_cast<std::uint16_t>(std::stoi(argv[2])),
                   opts);

    SyncOptions so;
    so.mode = SyncMode::Upload;
    so.compareBy = CompareMethod::XXHash;
    so.preserveTimes = true;
    so.verifyHash = true;

    auto result = client.syncFolder(argv[3], argv[4], so,
        [](const SyncProgress& sp) {
            std::cout << "\r" << sp.overallPercent() << "% "
                      << sp.currentOperation << " "
                      << sp.currentPath << "        " << std::flush;
        }).get();

    std::cout << "\n"
              << "uploaded=" << result.uploaded << " "
              << "skipped="  << result.skipped  << " "
              << "failed="   << result.failed   << "\n";

    for (auto& e : result.errors) {
        std::cerr << "error: " << e << "\n";
    }

    client.disconnect();
    return result.failed == 0 ? 0 : 1;
}
```

Run it:

```bash
./backup 192.168.1.10 9000 C:/data D:/shared/backup
```

---

## 18. Complete example: download mirror

Mirror a remote folder to a local folder, deleting local files that
do not exist on the remote. Useful for pulling a "source of truth"
folder to many machines.

```cpp
#include <filexferlib/client.hpp>
#include <filexferlib/logger.hpp>
#include <iostream>

using namespace filexferlib;

int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "usage: mirror <host> <port> "
                     "<remote-dir> <local-dir>\n";
        return 1;
    }

    Logger::instance().setSink([](LogLevel, const std::string& m){
        std::cout << m << "\n";
    });
    Logger::instance().setLevel(LogLevel::Info);

    Client client;
    ConnectOptions opts;
    opts.authToken = "my-secret";
    opts.autoReconnect = true;

    client.connect(argv[1],
                   static_cast<std::uint16_t>(std::stoi(argv[2])),
                   opts);

    SyncOptions so;
    so.mode = SyncMode::MirrorDownload;
    so.compareBy = CompareMethod::XXHash;
    so.deleteExtra = true;   // delete local files not on remote
    so.preserveTimes = true;

    auto result = client.syncFolder(argv[4], argv[3], so,
        [](const SyncProgress& sp) {
            std::cout << "\r" << sp.overallPercent() << "% "
                      << sp.currentOperation << " "
                      << sp.currentPath << "        " << std::flush;
        }).get();

    std::cout << "\n"
              << "downloaded=" << result.downloaded << " "
              << "deleted="    << result.deleted    << " "
              << "skipped="    << result.skipped    << " "
              << "failed="     << result.failed     << "\n";

    for (auto& e : result.errors) {
        std::cerr << "error: " << e << "\n";
    }

    client.disconnect();
    return result.failed == 0 ? 0 : 1;
}
```

Run it:

```bash
# Pull a copy of the server folder, deleting anything extra locally.
./mirror 192.168.1.10 9000 D:/shared/releases C:/app/releases
```

**Warning:** `deleteExtra = true` will remove local files that are not
on the remote. Preview with `dryRun = true` first if you are not sure.
