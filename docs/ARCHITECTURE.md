# Architecture

This document describes the internal design of `filexferlib`.

---

## Table of contents

- [Overview](#overview)
- [Protocol](#protocol)
- [State machines](#state-machines)
- [Bandwidth throttling](#bandwidth-throttling)
- [Compression](#compression)
- [Resume](#resume)
- [Path resolution](#path-resolution)
- [Concurrency model](#concurrency-model)
- [Wire format details](#wire-format-details)
- [Error handling](#error-handling)
- [Testing strategy](#testing-strategy)

---

## Overview

```
┌──────────────────────────────────────────┐
│         Public API                        │
│  Client              Server               │
├──────────────────────────────────────────┤
│         State Machines                    │
│  UploadState         DownloadState        │
│  syncFolder()        syncFile()           │
├──────────────────────────────────────────┤
│         Session Layer                     │
│  Session    TransferRegistry              │
│  Framing    TransferTimer                 │
│  RateLimiter                              │
├──────────────────────────────────────────┤
│         Asio (TCP, async I/O)             │
└──────────────────────────────────────────┘
```

---

## Protocol

### Frame format

Every message on the wire is a frame:

```
┌──────────┬──────────┬──────────┬──────────┬───────────────┐
│ magic(2) │ version  │  type(1) │ length(4)│ payload       │
│  0xF1EE  │  (1)     │          │          │ (length bytes)│
└──────────┴──────────┴──────────┴──────────┴───────────────┘
```

- `magic` — always `0xF1EE` (little-endian)
- `version` — protocol version (currently `1`)
- `type` — see message types below
- `length` — payload size in bytes (max 64 MB by default)

All multi-byte integers are **little-endian**.

### Message types

| Type | Value | Purpose |
|---|---|---|
| `Hello` | 0x01 | Client handshake |
| `HelloAck` | 0x02 | Server handshake ack |
| `Error` | 0x50 | Fatal error |
| `FileBegin` | 0x10 | Start transfer (upload or pull) |
| `FileChunk` | 0x11 | Data chunk |
| `FileEnd` | 0x12 | End of transfer |
| `FileAck` | 0x13 | Acknowledgement |
| `FileResumeReq` | 0x17 | Upload resume probe |
| `FileResumeResp` | 0x18 | Upload resume response |
| `DownloadResumeReq` | 0x19 | Download resume probe |
| `DownloadResumeResp` | 0x1A | Download resume response |
| `ListReq` | 0x20 | List directory |
| `ListResp` | 0x21 | List response |
| `MkdirReq` | 0x24 | Create directory |
| `MkdirAck` | 0x25 | Create ack |
| `DeleteReq` | 0x30 | Delete file/dir |
| `DeleteAck` | 0x31 | Delete ack |
| `RenameReq` | 0x32 | Rename or move on server |
| `RenameAck` | 0x33 | Rename ack |
| `SetMtimeReq` | 0x40 | Set mtime |
| `SetMtimeAck` | 0x41 | Set mtime ack |
| `RemoteCopyReq` | 0x60 | Server-side copy |
| `RemoteCopyAck` | 0x61 | Copy ack |
| `GenericReq` | 0xE0 | Generic request |
| `GenericResp` | 0xE1 | Generic response |

### Payload formats

**Hello:**
```
version(1) | tokenLen(2) | token
```

**HelloAck:**
```
version(1) | serverVersion(str)
         | maxBytesPerSecond(8)
         | recommendedTimeoutMs(4)
         | recommendedChunkSize(4)
```

**FileBegin:**
```
transferId(4) | path(str) | size(8) | mtimeNs(8) | xxhash(8)
```
- `mtimeNs == -1` → pull request (download)
- high bit of `xxhash` → compression flag

**FileChunk:**
```
transferId(4) | offset(8) | data
```
- if compressed: `data` = `origSize(4) | zstd-data`

**FileEnd:**
```
transferId(4) | xxhash(8)
```

**FileAck:**
```
transferId(4) | ok(1) | xxhash(8) | msg(str)
```

**FileResumeReq:**
```
transferId(4) | path(str)
```

**FileResumeResp:**
```
transferId(4) | offset(8) | ok(1)
```

**DownloadResumeReq:**
```
transferId(4) | path(str) | haveOffset(8)
```

**DownloadResumeResp:**
```
transferId(4) | serverOffset(8) | totalSize(8)
            | xxhash(8) | mtimeNs(8) | ok(1) | compressed(1)
```

**RenameReq:**
```
oldPath(str) | newPath(str)
```

**RenameAck:**
```
ok(1) | msg(str)
```

### Upload flow

```
Client                              Server
  │                                   │
  │─── Hello + token ────────────────►│
  │◄── HelloAck + rate info ──────────│
  │                                   │
  │─── FileBegin (tid, path, size,   │
  │             mtime, hash, comp) ──►│  (server opens .tmp file)
  │◄── FileAck (ok, resumeOffset) ────│
  │                                   │
  │─── FileChunk (tid, 0, data) ─────►│
  │─── FileChunk (tid, 1, data) ─────►│
  │─── FileChunk (tid, 2, data) ─────►│
  │◄── FileAck (ok) ──────────────────│
  │◄── FileAck (ok) ──────────────────│
  │◄── FileAck (ok) ──────────────────│
  │                                   │
  │─── FileEnd (tid, hash) ──────────►│  (server renames .tmp → final)
  │◄── FileAck (ok, hash) ────────────│
```

### Download flow

```
Client                              Server
  │                                   │
  │─── DownloadResumeReq ────────────►│  (client tells its local partial size)
  │◄── DownloadResumeResp ────────────│  (server tells offset + metadata)
  │                                   │
  │─── FileBegin (pull request,      │
  │             mtime = -1) ─────────►│
  │◄── FileBegin (real metadata) ─────│
  │─── FileAck ──────────────────────►│
  │                                   │
  │◄── FileChunk (tid, 0, data) ──────│
  │◄── FileChunk (tid, 1, data) ──────│
  │─── FileAck ──────────────────────►│
  │─── FileAck ──────────────────────►│
  │                                   │
  │◄── FileEnd (tid, hash) ───────────│
  │─── FileAck (ok, hash) ───────────►│
```

### Rename flow

```
Client                              Server
  │                                   │
  │─── RenameReq (oldPath, newPath) ─►│
  │                                   │  (server renames or moves the file)
  │◄── RenameAck (ok, msg) ───────────│
```

A single message pair. No data crosses the network — the rename is
entirely server-side.

### Pull request detection

A `FileBegin` frame with `mtimeNs == -1` indicates a **pull request**
(download). Otherwise it's an upload.

This allows zero-byte files to be uploaded correctly (they have
`size == 0` but `mtime != -1`).

---

## State machines

### UploadState

```
       ┌─────┐
       │Begin│
       └──┬──┘
          │ FileAck (resume offset)
          ▼
       ┌─────┐
    ┌─►│Chunk│◄─┐
    │  └──┬──┘  │
    │     │ FileAck
    │     ▼
    │  (window full? wait)
    │     │
    └─────┘
          │ offset == totalSize
          ▼
       ┌─────┐
       │ End │
       └──┬──┘
          │ FileAck (final hash)
          ▼
       ┌──────┐
       │ Done │
       └──────┘
```

**Pipelining:** up to `maxInflightChunks` chunks can be in flight at
once. The window slides as acks arrive.

**Cancellation:** checked at every state transition. On cancel, the
promise is resolved with `ErrorCode::Cancelled`.

### DownloadState

Similar, but with:

- An initial `DownloadResumeReq` / `DownloadResumeResp` exchange
- Decompression after each chunk (if compressed)
- The pull request's `FileBegin` is sent by the client

### Sync (folder)

```
1. scan_local()       → map<relativePath, FileMeta>
2. listRemote()       → map<relativePath, FileMeta>
3. build plan         → vector<Op>
4. execute plan       → sequential transfers

Op kinds:
  Upload        local → remote
  Download      remote → local
  DeleteLocal   remove local file (MirrorDownload)
  DeleteRemote  remove remote file (MirrorUpload)
  Skip          no action
```

Directories are **skipped in the planning phase**. They are created
automatically by the receiver when a file inside them is transferred.

### Single-file sync

`syncFile` performs a single comparison and one operation:

```
1. Stat local file
2. Stat remote file (via listRemote on the parent directory)
3. Compare with the configured CompareMethod
4. Decide direction (or skip)
5. Execute: upload, download, delete, or nothing
```

The result is a `SyncFileResult` with an `Action` enum indicating what
happened.

---

## Bandwidth throttling

### Server-side, both directions

One token bucket per server (`RateLimiter`).

**For download:** the server acquires tokens before sending each chunk.

**For upload:** the server delays the ACK by the number of bytes it
just received. The client's pipelining window naturally limits its
send rate to match.

### Token bucket

```
tokens += rate * elapsed / 1s
if tokens > rate: tokens = rate    (cap at 1 second's worth)
if tokens >= request: grant, tokens -= request
else: schedule wakeup in 10ms
```

The bucket **starts empty** to avoid an initial burst.

### Adaptive chunk size

When the server is throttled, it advertises a `recommendedChunkSize` in
`HelloAck`. The client uses this to override larger chunk sizes,
ensuring progress callbacks fire at reasonable intervals.

Recommended chunk size = `max(1KB, min(256KB, rate / 5))`.

Target: one chunk every ~200 ms.

---

## Compression

### Format

When compression is enabled (`Compression::Zstd`), each chunk on the
wire has this format:

```
┌─────────────┬────────────────────┐
│ origSize(4) │ zstd-compressed    │
│             │ data               │
└─────────────┴────────────────────┘
```

The `compressed` flag is carried in the **high bit of the `xxhash`
field** in `FileBegin`.

**Trade-off:** hashes with bit 63 set will lose that bit on the wire.
Probability: 1 in 2^63 — acceptable.

### Hash verification

XXHash3 always runs on the **uncompressed** data. This means:

- Compression can be enabled/disabled without changing verification
- The receiver decompresses first, then hashes

### Fallback

If zstd returns an empty buffer (shouldn't happen for valid input),
the chunk is sent uncompressed. The receiver detects this by checking
the `compressed` flag in `FileBegin`, not by inspecting the payload.

---

## Resume

### Upload resume

- Server keeps a `.fxfer.tmp` file
- On `FileBegin`, if `.tmp` exists and is smaller than the target size,
  the server replies with `resumeOffset = file_size(.tmp)`
- Client seeks to that offset and continues
- On `FileEnd`, the server renames `.tmp` → final (with retries on
  Windows due to AV locks)

### Download resume

- Client keeps a `<local>.fxfer.part` file
- Sends `DownloadResumeReq` with the partial file's size
- Server replies with the offset to start from
- Client opens `.part` and seeks to that offset
- On `FileEnd`, client renames `.part` → final

### Hash on resume

Because streaming hashers can't be resumed, both sides re-hash the
full file at the end. This is O(n) but simpler and correct.

**Note:** a future optimization could store hasher state in a
sidecar file, but this is not implemented.

### Windows file locking

Windows antivirus software may hold the `.tmp` file briefly after it
is closed. The server handles this with a fallback chain:

1. Direct rename
2. Rename with retries (5 × 50 ms)
3. `copy_file` + best-effort remove
4. Rename `.tmp` to `.tmp.leftover` (cleaned at next `Server::start`)

---

## Path resolution

All remote paths are resolved through `Server::resolvePath`:

1. Convert to `std::filesystem::path`
2. Canonicalize the root with `weakly_canonical`
3. If the input is truly absolute (has root_name + root_directory):
   - Use it directly (must be inside root unless allowed)
4. Otherwise:
   - Strip leading `/` and `\`
   - Join with root
5. Verify the result is inside root (prefix check with boundary)

**Security:** path traversal (`../etc/passwd`) is rejected.

`renameRemote` uses the same resolution for both `oldPath` and
`newPath`, and creates the parent of `newPath` if needed.

---

## Concurrency model

### Client

- One `io_context` per `Client`
- One io thread (`ioThread_`)
- All state machines run on the io thread
- Public API can be called from any thread
- `disconnect()` posts to the io thread

### Server

- One `io_context` per `Server`
- One io thread (`ioThread_`)
- All sessions share the same io thread
- Accept loop runs on the io thread
- Per-session `TransferRegistry`

### TransferRegistry

Maps `transferId` → handler. Used to route `File*` frames to the
correct state machine on a per-session basis.

This allows a single `Session` to have multiple concurrent transfers.

### IncomingFile key

`IncomingFile` entries are keyed by `(Session*, transferId)` so that
two clients with the same `transferId` do not collide.

### Close handlers

- `Session::add_close_handler` allows multiple callbacks
- Used to fail pending transfers when a session drops
- Prevents `broken promise` on unexpected disconnects

---

## Wire format details

### Integers

All integers are little-endian:

```cpp
put_u16(byte*, u16)
put_u32(byte*, u32)
put_u64(byte*, u64)
put_i64(byte*, i64)
```

### Strings

Length-prefixed:

```
len(2) | bytes
```

Strings are UTF-8 (assumed, not validated).

### TransferId

Every `File*` frame starts with a 4-byte transfer id. This is used
to demultiplex concurrent transfers on the same session.

### Version negotiation

The `Hello` frame carries `version(1)`. If the server doesn't support
it, the session is closed with `ErrorCode::VersionMismatch`.

---

## Error handling

- **Network errors:** session is closed; all pending transfers are
  failed with `ErrorCode::NetworkError`.
- **Protocol errors:** session is closed immediately.
- **Transfer errors:** promise is resolved with an exception.
- **Timeout:** `TransferTimer` fires after `readTimeout` ms of
  inactivity.

### Error codes

See `include/filexferlib/error.hpp`:

| Code | Meaning |
|---|---|
| `Ok` | success |
| `NetworkError` | socket error |
| `ProtocolError` | malformed frame |
| `VersionMismatch` | protocol version mismatch |
| `PayloadTooLarge` | frame exceeds limit |
| `MethodNotFound` | generic method not registered |
| `PathOutsideRoot` | path traversal attempt |
| `PathNotFound` | remote path doesn't exist |
| `PermissionDenied` | filesystem permission error |
| `Cancelled` | transfer cancelled by user |
| `Timeout` | transfer timed out |
| `InternalError` | server-side error |
| `HashMismatch` | XXHash verification failed |
| `AlreadyExists` | destination exists and overwrite is off |

---

## Testing strategy

| Test file | Coverage |
|---|---|
| `test_protocol` | framing encode/decode |
| `test_units` | encoding, hashing, rate limiter, logger, error codes |
| `test_edge` | empty files, 1-byte files, unicode names, deep trees, path traversal, missing files |
| `test_integration` | end-to-end: auth, upload, download, sync, resume, throttle, compression, rename, reconnect |
| `test_stress` | 100 parallel files, 256 MB file, 30 connect/disconnect, mixed load, 5 clients |

**Total: 50+ tests**, all running on Linux (GCC) and Windows (MSVC)
via GitHub Actions.

### CI

`.github/workflows/ci.yml` runs on every push:

1. Setup vcpkg (with manifest mode)
2. Configure CMake (with vcpkg toolchain)
3. Build (Release)
4. Run ctest

Both **Linux** and **Windows** must pass before a merge.
