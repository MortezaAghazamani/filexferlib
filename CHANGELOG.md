# Changelog

All notable changes to `filexferlib` will be documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/),
and this project adheres to [Semantic Versioning](https://semver.org/).

---

## [1.0.0] — 2025-10-06

First stable release.

### Added

- **Async API** based on `std::future`
- **Upload / Download** with chunked transfer and progress callbacks
- **XXHash3 verification** on every transfer
- **Resume** for uploads and downloads
- **Pause / Resume / Cancel** via `TransferHandle`
- **Pipelining** (sliding window) for high throughput
- **Compression (zstd)** — optional per-chunk compression via `CopyOptions::compression`
- **Bandwidth throttling** on the server — one config controls both directions
- **Adaptive chunk size** based on the advertised rate limit
- **Sync** two folders with five modes:
  - `Upload`
  - `Download`
  - `MirrorUpload`
  - `MirrorDownload`
  - `Bidirectional`
- **Single-file sync** — `Client::syncFile`
- **Dry run** — `SyncOptions::dryRun`
- **Server-side operations** — copy, rename/move, delete without transferring data
- **Exception-free API** — every throwing method has a `try*` variant
  returning `Outcome<T>`
- **Generic request / response** — `Client::call` + `Server::registerHandler`
- **Auth** via shared token
- **Auto-reconnect** with exponential backoff
- **Secure path resolution** inside the server root (rejects traversal)
- **Zero-cost logging** in release builds
- **Graceful shutdown** on Ctrl+C
- **50+ tests** across five suites:
  - `test_protocol` (framing)
  - `test_units` (encoding, hashing, rate limiter, logger, errors)
  - `test_edge` (empty files, unicode, deep trees, path traversal)
  - `test_integration` (end-to-end)
  - `test_stress` (100 files, 256 MB file, 5 clients)
- **CI** on Linux (GCC) and Windows (MSVC) via GitHub Actions
- **Documentation**: README, EXAMPLES, API_GUIDE, ARCHITECTURE
