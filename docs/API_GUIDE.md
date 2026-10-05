# API Guide

Complete reference for every public method, type, and error code in
`filexferlib`.

- New here? Start with [README](../README.md).
- Want runnable snippets? See [EXAMPLES.md](EXAMPLES.md).
- Want internals (protocol, state machines)? See [ARCHITECTURE.md](ARCHITECTURE.md).

All public API lives in the `filexferlib` namespace and is available
via the following headers:

```cpp
#include <filexferlib/client.hpp>
#include <filexferlib/server.hpp>
#include <filexferlib/types.hpp>
#include <filexferlib/error.hpp>
#include <filexferlib/logger.hpp>
#include <filexferlib/outcome.hpp>
#include <filexferlib/transfer_handle.hpp>
```

---

## Table of contents

1. [Types overview](#1-types-overview)
2. [Client](#2-client)
3. [Server](#3-server)
4. [Configuration](#4-configuration)
5. [Sync types](#5-sync-types)
6. [Transfer types](#6-transfer-types)
7. [Outcome](#7-outcome)
8. [TransferHandle](#8-transferhandle)
9. [CancellationToken](#9-cancellationtoken)
10. [Error codes](#10-error-codes)
11. [Logger](#11-logger)
12. [Callbacks](#12-callbacks)
13. [Threading and lifetime](#13-threading-and-lifetime)
14. [Common patterns](#14-common-patterns)

---

## 1. Types overview

| Type | Header | Purpose |
| --- | --- | --- |
| `Client` | `client.hpp` | Connects to a server and drives all transfers. |
| `Server` | `server.hpp` | Accepts connections and serves files. |
| `ServerConfig` | `types.hpp` | Server configuration (root, auth, rate limit). |
| `ConnectOptions` | `types.hpp` | Client connection settings. |
| `CopyOptions` | `types.hpp` | Per-transfer settings. |
| `SyncOptions` | `types.hpp` | Folder sync settings. |
| `SyncMode` | `types.hpp` | Direction of sync. |
| `CompareMethod` | `types.hpp` | How to compare two files. |
| `Compression` | `types.hpp` | Compression algorithm for a transfer. |
| `TransferProgress` | `types.hpp` | Progress info passed to callbacks. |
| `TransferResult` | `types.hpp` | Result of a file transfer. |
| `SyncProgress` | `types.hpp` | Progress info for a folder sync. |
| `SyncResult` | `types.hpp` | Aggregate result of a folder sync. |
| `SyncFileResult` | `types.hpp` | Result of a single-file sync. |
| `FileMeta` | `types.hpp` | Metadata of one remote entry. |
| `GenericResponse` | `types.hpp` | Response to a generic `call`. |
| `CallOptions` | `types.hpp` | Options for a generic `call`. |
| `ByteBuffer` | `types.hpp` | `std::vector<std::byte>`. |
| `Outcome<T>` | `outcome.hpp` | Result or error, no exceptions. |
| `TransferHandle` | `transfer_handle.hpp` | Controls a live transfer. |
| `CancellationToken` | `types.hpp` | Cancels a transfer from outside. |
| `ErrorCode` | `error.hpp` | Enumerated error conditions. |
| `LogLevel` | `logger.hpp` | Logging verbosity. |
| `Logger` | `logger.hpp` | Global logger singleton. |

---

## 2. Client

The `Client` class is the main entry point. All methods are safe to
call from any thread (except `connect` and `disconnect`, which must not
be called concurrently).

```cpp
Client client;
```

### 2.1 Connection

#### `connect`

```cpp
SessionInfo connect(const std::string& host,
                    std::uint16_t port,
                    ConnectOptions opts = {});
```

Connects to a server **synchronously**. Returns when the session is
established or fails.

- **Parameters**
  - `host` — server hostname or IP address.
  - `port` — server TCP port.
  - `opts` — see [`ConnectOptions`](#42-connectoptions).
- **Returns** — `SessionInfo` with the negotiated server version.
- **Throws** — `std::system_error` on any network or protocol error.
- **See also** — `connectAsync`, `tryConnect`, `disconnect`.

#### `connectAsync`

```cpp
std::future<SessionInfo> connectAsync(std::string host,
                                      std::uint16_t port,
                                      ConnectOptions opts = {});
```

Same as `connect` but non-blocking.

- **Returns** — a future; `.get()` throws on failure.

#### `tryConnect`

```cpp
Outcome<SessionInfo> tryConnect(const std::string& host,
                                std::uint16_t port,
                                ConnectOptions opts = {});
```

Same as `connect` but never throws. Returns an `Outcome<SessionInfo>`.

#### `tryConnectAsync`

```cpp
std::future<Outcome<SessionInfo>> tryConnectAsync(std::string host,
                                                  std::uint16_t port,
                                                  ConnectOptions opts = {});
```

Non-blocking, exception-free.

#### `disconnect`

```cpp
void disconnect();
```

Closes the session and stops the io thread. Blocking (posts a close to
the io thread and waits up to 2 seconds). Safe to call multiple times.

#### `isConnected`

```cpp
bool isConnected() const;
```

Returns `true` while the session is open.

---

### 2.2 File transfer

#### `upload`

```cpp
std::future<TransferResult> upload(
    const std::filesystem::path& local,
    const std::string& remotePath,
    CopyOptions opts = {},
    ProgressCallback onProgress = {},
    CancellationToken cancel = {});
```

Uploads a local file to the remote. Non-blocking; returns a future.

- **Automatic resume** — if the server has a partial `.fxfer.tmp` for
  `remotePath`, the upload resumes from that offset.
- **Verification** — if `opts.verifyHash` is `true` (default), the
  server computes XXHash3 and compares it against the client's hash.
- **Progress** — `onProgress` is called periodically (throttled by
  `opts.progressInterval`).
- **Cancellation** — `cancel` (or `opts` with a paused handle) stops
  the transfer early.
- **Throws** — in the future's `.get()` on any error.
- **See also** — `tryUpload`, `uploadWithHandle`, `download`.

#### `download`

```cpp
std::future<TransferResult> download(
    const std::string& remotePath,
    const std::filesystem::path& local,
    CopyOptions opts = {},
    ProgressCallback onProgress = {},
    CancellationToken cancel = {});
```

Downloads a remote file to the local filesystem. Uses a
`<local>.fxfer.part` file internally for resume support.

- **Automatic resume** — if `<local>.fxfer.part` exists, the client
  tells the server how many bytes it already has, and continues from
  there.
- **Atomic finalize** — on success the `.part` is renamed to `local`.
- **See also** — `tryDownload`, `downloadWithHandle`, `upload`.

#### `uploadWithHandle`

```cpp
struct UploadResult {
    TransferHandle handle;
    std::future<TransferResult> future;
};

UploadResult uploadWithHandle(
    const std::filesystem::path& local,
    const std::string& remotePath,
    CopyOptions opts = {},
    ProgressCallback onProgress = {});
```

Like `upload`, but also returns a `TransferHandle` to pause, resume,
or cancel the transfer at any time.

#### `downloadWithHandle`

Same shape as `uploadWithHandle` but for downloads.

#### `copyRemoteToRemote`

```cpp
std::future<TransferResult> copyRemoteToRemote(
    const std::string& srcRemote,
    const std::string& dstRemote,
    CopyOptions opts = {},
    ProgressCallback onProgress = {},
    CancellationToken cancel = {});
```

Copies a file on the server **without transferring data over the
network**. Executes as a server-side filesystem copy.

- **Fast path** — for same-volume copies, the server can use
  hardlink/reflink when the filesystem supports it.
- **Parent directories** — created automatically.
- **See also** — `renameRemote`, `tryCopyRemoteToRemote`.

---

### 2.3 Sync

#### `syncFolder`

```cpp
std::future<SyncResult> syncFolder(
    const std::filesystem::path& localDir,
    const std::string& remoteDir,
    SyncOptions opts = {},
    SyncProgressCallback onProgress = {},
    CancellationToken cancel = {});
```

Synchronizes two folders according to `opts.mode`.

- **Scan phase** — reads metadata of both sides.
- **Plan phase** — decides what to upload, download, delete, or skip.
- **Execute phase** — performs operations sequentially.
- **Directories** — created automatically when files inside them are
  transferred. Empty directories are not synced by default.
- **See also** — [`SyncMode`](#51-syncmode), [`SyncOptions`](#44-syncoptions).

#### `syncFile`

```cpp
std::future<SyncFileResult> syncFile(
    const std::filesystem::path& localFile,
    const std::string& remotePath,
    SyncOptions opts = {},
    ProgressCallback onProgress = {});
```

Syncs a single file with the same comparison and direction logic as
`syncFolder`. Useful for one-off comparisons of a config file or
database.

- **Returns** — `SyncFileResult` with an `Action` enum indicating what
  happened (uploaded, downloaded, skipped, deleted, not found).

---

### 2.4 Remote directory operations

#### `listRemote`

```cpp
std::future<std::vector<FileMeta>> listRemote(const std::string& remoteDir);
```

Recursively lists `remoteDir`. Each `FileMeta` has `relativePath` (POSIX
style, starting with `/`), `size`, `mtimeNs`, and `isDirectory`.

Directories are included in the list (unlike in `syncFolder`, where
they are filtered out).

#### `deleteRemote`

```cpp
std::future<void> deleteRemote(const std::string& remotePath,
                               bool recursive = false);
```

Deletes a file, or a directory when `recursive = true`.

#### `renameRemote`

```cpp
std::future<void> renameRemote(const std::string& oldPath,
                               const std::string& newPath);
```

Renames or moves a file or directory on the server. Server-side only.

- If `oldPath` and `newPath` are in the same directory → rename.
- If they are in different directories → move (and rename).
- Parent of `newPath` is created automatically.

**Errors:**

- `PathNotFound` — the source doesn't exist.
- `PathOutsideRoot` — one of the paths escapes the server root.
- `AlreadyExists` — the destination exists and overwrite is off
  (currently not enforced; `std::filesystem::rename` overwrites).

#### `mkdirRemote`

```cpp
std::future<void> mkdirRemote(const std::string& remotePath,
                              bool parents = true);
```

Creates a directory. With `parents = true`, intermediate directories
are created.

#### `setRemoteMtime`

```cpp
std::future<void> setRemoteMtime(const std::string& remotePath,
                                 std::int64_t mtimeNs);
```

Sets the modification time of a remote file.

- `mtimeNs` — nanoseconds since the Unix epoch. Always UTC.

---

### 2.5 Generic calls

#### `call`

```cpp
std::future<GenericResponse> call(std::string method,
                                  ByteBuffer payload = {},
                                  CallOptions opts = {});
```

Sends a custom request to the server. The server must have registered
a handler for `method` (see [`Server::registerHandler`](#32-methods)).

- **Returns** — `GenericResponse { statusCode, payload }`.
- **statusCode 0** means success; any other value is defined by the
  server-side handler.
- **Throws** — `std::runtime_error` if the client is not connected.
- **`CallOptions::timeout`** — currently informational; the underlying
  read timeout from `ConnectOptions` applies.

---

### 2.6 Accessors

```cpp
asio::io_context& io();
std::shared_ptr<detail::Session> session() const;
const ConnectOptions& connectOpts() const;
```

For advanced use (embedding the client into an existing io_context,
inspection). Not needed for typical use.

---

## 3. Server

### 3.1 Construction

```cpp
explicit Server(ServerConfig cfg);
```

The server reads `cfg` at construction and does not modify it later.

### 3.2 Methods

#### `start`

```cpp
std::uint16_t start(const std::string& bindAddress,
                    std::uint16_t port);
```

Binds and starts listening on a background thread. Returns the actual
bound port (useful when `port == 0` lets the OS pick one).

- **Throws** — `std::system_error` on bind or listen failure.

#### `stop`

```cpp
void stop();
```

Graceful shutdown: closes the acceptor, disconnects all sessions,
stops the io thread. Idempotent.

#### `registerHandler`

```cpp
using GenericHandler = std::function<void(GenericContext&)>;
void registerHandler(std::string method, GenericHandler handler);
```

Registers a handler for a custom method. When a client sends
`call(method, payload)`, the handler is invoked on the io thread with
a `GenericContext`.

**`GenericContext`:**

```cpp
class GenericContext {
public:
    const ByteBuffer& request() const;
    void respond(ByteBuffer payload = {}, std::uint16_t statusCode = 0);
};
```

Calling `respond` more than once is a no-op (only the first response
is sent).

---

## 4. Configuration

### 4.1 ServerConfig

```cpp
struct ServerConfig {
    std::filesystem::path rootDir;
    std::string           requiredAuthToken;
    std::uint64_t         maxBytesPerSecond = 0;
    std::uint32_t         downloadWindowSize = 4;
    bool                  allowAbsoluteOutsideRoot = false;
    bool                  followSymlinks = false;
    Limits                limits;
};
```

| Field | Default | Purpose |
| --- | --- | --- |
| `rootDir` | required | All remote paths are resolved relative to this. |
| `requiredAuthToken` | `""` | Empty = no auth. Otherwise clients must send this in `Hello`. |
| `maxBytesPerSecond` | `0` | Total bandwidth limit for **both** directions. 0 = unlimited. Shared across all sessions. |
| `downloadWindowSize` | `4` | Number of unacked chunks during a download. |
| `allowAbsoluteOutsideRoot` | `false` | **Unsafe.** Allow paths outside `rootDir`. |
| `followSymlinks` | `false` | Reserved; symlinks are not followed by default. |
| `limits` | see below | Protocol-level size limits. |

**`Limits`:**

```cpp
struct Limits {
    std::uint32_t maxPayloadSize      = 64 * 1024 * 1024;  // 64 MB
    std::uint32_t maxMethodNameLength = 256;
    std::uint32_t maxPathLength       = 4096;
};
```

### 4.2 ConnectOptions

```cpp
struct ConnectOptions {
    std::chrono::milliseconds connectTimeout{5000};
    std::chrono::milliseconds readTimeout{30000};
    std::chrono::milliseconds writeTimeout{30000};

    std::string authToken;

    bool          autoReconnect           = false;
    std::uint32_t maxReconnectAttempts    = 5;
    std::chrono::milliseconds reconnectBaseDelay{500};
    std::chrono::milliseconds reconnectMaxDelay{10000};

    // Populated by the server after connect:
    std::uint64_t maxBytesPerSecond = 0;
    std::uint32_t recommendedChunkSize = 0;
};
```

| Field | Purpose |
| --- | --- |
| `authToken` | Sent to the server in `Hello`. Must match `ServerConfig::requiredAuthToken` if set. |
| `connectTimeout` | Max time to wait for TCP connect. |
| `readTimeout` | Max time between received messages. The server may raise this via `recommendedTimeoutMs`. |
| `autoReconnect` | If `true`, reconnects automatically after a session drop. |
| `maxReconnectAttempts` | Number of reconnect tries before giving up. |
| `reconnectBaseDelay`, `reconnectMaxDelay` | Exponential backoff bounds. |
| `maxBytesPerSecond` | Read-only. Set by server after connect. |
| `recommendedChunkSize` | Read-only. Set by server after connect. |

### 4.3 CopyOptions

```cpp
struct CopyOptions {
    bool          overwrite      = true;
    bool          preserveTimes  = true;
    bool          verifyHash     = true;
    bool          createDirs     = true;
    std::uint32_t chunkSize      = 256 * 1024;
    std::uint32_t maxInflightChunks = 4;

    std::chrono::milliseconds progressInterval{50};

    std::uint32_t downloadWindowSize = 4;

    Compression compression      = Compression::None;
    int         compressionLevel = 3;
};
```

| Field | Default | Purpose |
| --- | --- | --- |
| `overwrite` | true | Overwrite existing destination. |
| `preserveTimes` | true | Preserve mtime. |
| `verifyHash` | true | XXHash3 verify after transfer. |
| `createDirs` | true | Create intermediate directories. |
| `chunkSize` | 256 KB | Size of each chunk. Auto-reduced if server is throttled. |
| `maxInflightChunks` | 4 | Upload pipelining window. |
| `progressInterval` | 50 ms | Minimum time between progress callbacks. |
| `downloadWindowSize` | 4 | Download pipelining window. |
| `compression` | None | `None` or `Zstd`. |
| `compressionLevel` | 3 | zstd level (1–22). |

### 4.4 SyncOptions

```cpp
struct SyncOptions {
    SyncMode      mode           = SyncMode::Upload;
    CompareMethod compareBy      = CompareMethod::SizeAndMtime;
    bool          preserveTimes  = true;
    bool          verifyHash     = true;
    bool          deleteExtra    = false;
    bool          dryRun         = false;
    std::uint32_t maxConcurrentTransfers = 4;
    std::chrono::seconds mtimeTolerance{2};

    std::function<bool(const std::string&)> filter;
};
```

| Field | Default | Purpose |
| --- | --- | --- |
| `mode` | Upload | See [`SyncMode`](#51-syncmode). |
| `compareBy` | SizeAndMtime | See [`CompareMethod`](#52-comparemethod). |
| `preserveTimes` | true | Preserve mtime on transfer. |
| `verifyHash` | true | XXHash3 verify per file. |
| `deleteExtra` | false | Delete extra files. Only honored by Mirror modes. |
| `dryRun` | false | Report only, do not execute. |
| `mtimeTolerance` | 2 s | Tolerance for mtime comparison. |
| `filter` | none | Optional predicate on relative path. Return `false` to skip. |

---

## 5. Sync types

### 5.1 SyncMode

```cpp
enum class SyncMode {
    Upload,
    Download,
    MirrorUpload,
    MirrorDownload,
    Bidirectional
};
```

| Mode | Direction | Deletes extras? |
| --- | --- | --- |
| `Upload` | local → remote | no |
| `Download` | remote → local | no |
| `MirrorUpload` | local → remote | yes (on remote) |
| `MirrorDownload` | remote → local | yes (on local) |
| `Bidirectional` | both ways | no |

**Behavior table:**

| State | Upload | Download | MirrorUpload | MirrorDownload | Bidirectional |
| --- | --- | --- | --- | --- | --- |
| only local | upload | skip | upload | delete local | upload |
| only remote | skip | download | delete remote | download | download |
| both, same | skip | skip | skip | skip | skip |
| both, different | upload | download | upload | download | newer wins |

### 5.2 CompareMethod

```cpp
enum class CompareMethod { SizeOnly, SizeAndMtime, XXHash };
```

| Method | Compares | Speed | Accuracy |
| --- | --- | --- | --- |
| `SizeOnly` | size | fastest | lowest |
| `SizeAndMtime` | size + mtime | fast | high |
| `XXHash` | XXHash3 digest | slow | highest |

### 5.3 Compression

```cpp
enum class Compression { None, Zstd };
```

- `None` — sends raw bytes.
- `Zstd` — compresses each chunk.

Hash verification always runs on uncompressed data.

---

## 6. Transfer types

### 6.1 TransferProgress

```cpp
struct TransferProgress {
    std::uint64_t bytesTransferred = 0;
    std::uint64_t totalBytes       = 0;
    std::uint64_t bytesPerSecond   = 0;
    std::string   currentPath;

    double percent() const;
    double speedMBps() const;
};
```

### 6.2 TransferResult

```cpp
struct TransferResult {
    std::uint64_t bytesTransferred = 0;
    std::uint64_t elapsedMs        = 0;
    std::string   xxhashSource;
    std::string   xxhashDest;
    bool          verified         = false;
};
```

### 6.3 SyncProgress

```cpp
struct SyncProgress {
    std::uint32_t filesTotal = 0;
    std::uint32_t filesDone  = 0;
    std::uint64_t bytesTotal = 0;
    std::uint64_t bytesDone  = 0;
    std::string   currentOperation;
    std::string   currentPath;
    TransferProgress currentFile;

    double overallPercent() const;
};
```

`currentOperation` values: `"upload"`, `"download"`,
`"delete-remote"`, `"delete-local"`, `"skip-delete-remote"`,
`"skip-delete-local"`, `"would-upload"`, `"would-download"`,
`"would-delete-remote"`, `"would-delete-local"`.

### 6.4 SyncResult

```cpp
struct SyncResult {
    std::uint32_t uploaded  = 0;
    std::uint32_t downloaded= 0;
    std::uint32_t deleted   = 0;
    std::uint32_t skipped   = 0;
    std::uint32_t failed    = 0;
    std::uint64_t bytesTransferred = 0;
    std::vector<std::string> errors;
};
```

### 6.5 SyncFileResult

```cpp
struct SyncFileResult {
    enum class Action {
        Skipped,
        Uploaded,
        Downloaded,
        DeletedLocal,
        DeletedRemote,
        None
    };

    Action        action = Action::None;
    std::uint64_t bytesTransferred = 0;
    bool          verified = false;
    std::string   message;
};
```

### 6.6 FileMeta

```cpp
struct FileMeta {
    std::string   relativePath;
    std::uint64_t size = 0;
    std::int64_t  mtimeNs = 0;   // nanoseconds since Unix epoch, UTC
    std::uint64_t xxhash = 0;
    bool          isDirectory = false;
};
```

### 6.7 GenericResponse

```cpp
struct GenericResponse {
    std::uint16_t statusCode = 0;
    ByteBuffer    payload;
};
```

### 6.8 CallOptions

```cpp
struct CallOptions {
    std::chrono::milliseconds timeout{30000};
};
```

---

## 7. Outcome

```cpp
template <typename T>
class Outcome;
```

Holds a value of `T` or an `std::error_code` + message.

```cpp
bool ok() const noexcept;
explicit operator bool() const noexcept;

T& value() &;                    // throws std::logic_error if !ok()
const T& value() const&;
T&& value() &&;
T value_or(T fallback) const;    // safe, never throws

T* operator->();                 // throws std::logic_error if !ok()
const T* operator->() const;
T& operator*();                  // throws std::logic_error if !ok()
const T& operator*() const;

std::error_code error() const noexcept;
const std::string& message() const noexcept;
```

**Example:**

```cpp
auto r = client.tryConnect("127.0.0.1", 9000);
if (!r) {
    std::cerr << "error: " << r.error().message()
              << " (" << r.message() << ")\n";
    return 1;
}
std::cout << "connected: " << r.value().serverVersion << "\n";
```

`Outcome<void>` is a specialization with no value.

---

## 8. TransferHandle

```cpp
class TransferHandle {
public:
    enum class State {
        Running = 0,
        Paused  = 1,
        Cancelled = 2,
        Done    = 3,
        Failed  = 4
    };

    void pause();
    void resume();
    void cancel();

    State state() const;
    bool valid() const;

    std::uint64_t bytesTransferred() const;
    std::uint64_t totalBytes() const;
};
```

Returned by `uploadWithHandle` / `downloadWithHandle`. Thread-safe.

`pause()` is asynchronous: the transfer stops at the next safe point
(after the current chunk has been sent or received).

`resume()` continues from where it stopped.

`cancel()` stops the transfer and causes the future to throw
`ErrorCode::Cancelled`.

---

## 9. CancellationToken

```cpp
using CancellationToken = std::shared_ptr<std::atomic<bool>>;

CancellationToken make_cancellation_token();
```

A shared atomic flag. Pass the same token to multiple transfers to
cancel them all:

```cpp
auto token = make_cancellation_token();

auto f1 = c.upload("a.bin", "/a.bin", {}, {}, token);
auto f2 = c.upload("b.bin", "/b.bin", {}, {}, token);

// Later:
token->store(true);   // cancels both
```

The check is polled — the transfer stops at the next callback.

---

## 10. Error codes

```cpp
enum class ErrorCode {
    Ok = 0,
    NetworkError,
    ProtocolError,
    VersionMismatch,
    PayloadTooLarge,
    MethodNotFound,
    PathOutsideRoot,
    PathNotFound,
    PermissionDenied,
    Cancelled,
    Timeout,
    InternalError,
    HashMismatch,
    AlreadyExists,
};
```

| Code | Meaning | Typically caused by |
| --- | --- | --- |
| `Ok` | Success | — |
| `NetworkError` | Socket error | Server down, network drop |
| `ProtocolError` | Malformed frame | Bug, incompatible peer |
| `VersionMismatch` | Protocol version mismatch | Old client vs new server |
| `PayloadTooLarge` | Frame exceeds `maxPayloadSize` | Malicious or buggy peer |
| `MethodNotFound` | Generic method not registered | Client called wrong method |
| `PathOutsideRoot` | Path traversal attempt | Client tried `../etc/passwd` |
| `PathNotFound` | Remote path doesn't exist | Typo, deleted file |
| `PermissionDenied` | Filesystem permission error | OS-level denial |
| `Cancelled` | Transfer cancelled by user | `CancellationToken` or `handle.cancel()` |
| `Timeout` | Transfer timed out | Server too slow |
| `InternalError` | Server-side or client-side error | Any generic failure |
| `HashMismatch` | XXHash verification failed | Corruption, disk error |
| `AlreadyExists` | Destination exists and overwrite off | Reserved |

Convert to `std::error_code`:

```cpp
std::error_code ec = make_error_code(ErrorCode::Cancelled);
```

---

## 11. Logger

```cpp
class Logger {
public:
    static Logger& instance();

    void setSink(LogSink sink);
    void setLevel(LogLevel lv);

    LogLevel level() const;
    bool hasSink() const;
    void log(LogLevel lv, const std::string& msg);
};

enum class LogLevel { Trace, Debug, Info, Warn, Error, Off };

using LogSink = std::function<void(LogLevel, const std::string&)>;
```

The logger is a global singleton. Setting a sink enables logging; the
level filters which messages pass through.

**Zero-cost mode:** define `FILEXFERLIB_DISABLE_LOGGING` at compile
time to remove all logging code.

```cmake
target_compile_definitions(filexferlib PUBLIC FILEXFERLIB_DISABLE_LOGGING)
```

**Macros:** `FX_TRACE`, `FX_DEBUG`, `FX_INFO`, `FX_WARN`, `FX_ERROR`.

---

## 12. Callbacks

```cpp
using ProgressCallback =
    std::function<void(const TransferProgress&)>;

using SyncProgressCallback =
    std::function<void(const SyncProgress&)>;

using GenericHandler =
    std::function<void(GenericContext&)>;
```

**Threading:** callbacks run on the client's io thread (or the server's
io thread for handlers). Do not block for long; copy the data you need
and return quickly.

**Throttling:** `ProgressCallback` is throttled by
`CopyOptions::progressInterval`. `SyncProgressCallback` is called after
each file operation completes.

---

## 13. Threading and lifetime

### Client

- The client runs a single background thread (`ioThread_`) with an
  `asio::io_context`.
- All state machines run on that thread.
- The public API is thread-safe, except:
  - `connect` / `connectAsync` / `disconnect` — must not run
    concurrently.
- `disconnect` is blocking (up to 2 s).

### Server

- The server runs a single background thread.
- All sessions are handled on that thread.
- `start` / `stop` must not run concurrently.

### Callbacks

- Progress callbacks are called on the io thread.
- Do not block in a callback; do not call `disconnect` from a callback.

### Lifetime

- `Client` must outlive any pending transfers. Destroying a `Client`
  cancels all pending transfers and fails their futures.
- `Server` must outlive all sessions. `stop()` closes everything.
- `TransferHandle` remains valid until the transfer completes; after
  that its state is `Done` or `Failed` and `pause()`/`resume()` are
  no-ops.

### Clean shutdown

```cpp
client.disconnect();   // stops the io thread
// client destructor is now safe

server.stop();          // closes sessions, stops the io thread
// server destructor is now safe
```

---

## 14. Common patterns

### Retry on connect

```cpp
ConnectOptions o;
o.autoReconnect = true;
o.maxReconnectAttempts = 20;
o.reconnectBaseDelay = std::chrono::milliseconds(500);

try {
    c.connect("127.0.0.1", 9000, o);
} catch (const std::exception& e) {
    std::cerr << "giving up: " << e.what() << "\n";
}
```

### Upload with progress and cancel

```cpp
auto res = c.uploadWithHandle("big.iso", "/big.iso", {},
    [](const TransferProgress& p) {
        std::cout << p.percent() << "%\n";
    });

std::thread watcher([&]{
    std::this_thread::sleep_for(std::chrono::seconds(10));
    res.handle.cancel();
});

try {
    auto r = res.future.get();
    std::cout << "done, verified=" << r.verified << "\n";
} catch (const std::exception& e) {
    std::cout << "cancelled: " << e.what() << "\n";
}
watcher.join();
```

### Dry-run sync, then apply

```cpp
SyncOptions o;
o.mode = SyncMode::MirrorUpload;
o.deleteExtra = true;

// Preview
o.dryRun = true;
auto preview = c.syncFolder("C:/local", "/backup", o).get();
std::cout << "would upload " << preview.uploaded
          << ", would delete " << preview.deleted << "\n";

if (userConfirms()) {
    o.dryRun = false;
    auto real = c.syncFolder("C:/local", "/backup", o).get();
    std::cout << "uploaded " << real.uploaded
              << ", deleted "  << real.deleted << "\n";
}
```

### Safe exception-free upload

```cpp
auto u = c.tryUpload("big.iso", "/big.iso").get();
if (!u) {
    if (u.error() == make_error_code(ErrorCode::Cancelled))
        std::cout << "user cancelled\n";
    else
        std::cerr << "failed: " << u.message() << "\n";
    return 1;
}
std::cout << "uploaded " << u.value().bytesTransferred << " bytes\n";
```
