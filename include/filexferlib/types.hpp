#pragma once
#include "filexferlib/detail/common.hpp"

#include <string>
#include <vector>
#include <filesystem>
#include <chrono>
#include <functional>
#include <atomic>
#include <memory>
#include <cstddef>
#include <cstdint>

namespace filexferlib {

struct LocalPath {
    std::filesystem::path p;
    LocalPath() = default;
    LocalPath(std::filesystem::path x) : p(std::move(x)) {}
    LocalPath(const char* x) : p(x) {}
};

struct RemotePath {
    std::string p;
    RemotePath() = default;
    RemotePath(std::string x) : p(std::move(x)) {}
    RemotePath(const char* x) : p(x) {}
};

struct Limits {
    std::uint32_t maxPayloadSize      = 64u * 1024 * 1024;
    std::uint32_t maxMethodNameLength = 256;
    std::uint32_t maxPathLength       = 4096;
};

struct ServerConfig {
    std::filesystem::path rootDir;
    bool allowAbsoluteOutsideRoot = false;
    bool followSymlinks = false;

    std::string requiredAuthToken;

    std::uint64_t maxBytesPerSecond = 0;

    std::uint32_t downloadWindowSize = 4;

    Limits limits;
};

struct ConnectOptions {
    std::chrono::milliseconds connectTimeout{5000};
    std::chrono::milliseconds readTimeout{30000};
    std::chrono::milliseconds writeTimeout{30000};

    std::string authToken;

    bool          autoReconnect           = false;
    std::uint32_t maxReconnectAttempts    = 5;
    std::chrono::milliseconds reconnectBaseDelay{500};
    std::chrono::milliseconds reconnectMaxDelay {10000};

    // Populated automatically after connect from the server.
    std::uint64_t maxBytesPerSecond = 0;
    std::uint32_t recommendedChunkSize = 0;
};

using ByteBuffer = std::vector<std::byte>;

struct GenericResponse {
    std::uint16_t statusCode = 0;
    ByteBuffer    payload;
};

struct CallOptions {
    std::chrono::milliseconds timeout{30000};
};

struct SessionInfo {
    std::string serverVersion;
    std::string clientId;
};

// ---- Transfer ----

struct TransferProgress {
    std::uint64_t bytesTransferred = 0;
    std::uint64_t totalBytes       = 0;
    std::uint64_t bytesPerSecond   = 0;
    std::string   currentPath;
    double percent() const {
        return totalBytes ? (100.0 * double(bytesTransferred) / double(totalBytes)) : 0.0;
    }
    double speedMBps() const { return double(bytesPerSecond) / (1024.0 * 1024.0); }
};

// ---------------------------------------------------------------
// Enums used by CopyOptions (must be declared BEFORE CopyOptions)
// ---------------------------------------------------------------

/**
 * @brief Compression mode for file transfers.
 *
 * `None` sends raw bytes (fastest, no CPU overhead).
 * `Zstd` compresses each chunk with zstd. Good for text files,
 * logs, source code. Almost no benefit for already-compressed
 * files (JPEG, ZIP, MP4).
 *
 * Hash verification always runs on the uncompressed data.
 */
enum class Compression {
    None,
    Zstd
};

/**
 * @brief How to decide if two files are considered identical.
 *
 * | Method        | Compares           | Cost     |
 * |---------------|--------------------|----------|
 * | SizeOnly      | size               | cheapest |
 * | SizeAndMtime  | size + mtime       | cheap    |
 * | XXHash        | XXHash3 digest     | expensive|
 */
enum class CompareMethod { SizeOnly, SizeAndMtime, XXHash };

/**
 * @brief Direction and semantics of a sync operation.
 *
 * | Mode           | Direction       | Deletes extras?      |
 * |----------------|-----------------|----------------------|
 * | Upload         | local → remote  | no                   |
 * | Download       | remote → local  | no                   |
 * | MirrorUpload   | local → remote  | yes (on remote)      |
 * | MirrorDownload | remote → local  | yes (on local)       |
 * | Bidirectional  | both ways       | no (newer wins)      |
 */
enum class SyncMode {
    Upload,
    Download,
    MirrorUpload,
    MirrorDownload,
    Bidirectional
};

// ---------------------------------------------------------------
// CopyOptions
// ---------------------------------------------------------------

struct CopyOptions {
    bool overwrite      = true;
    bool preserveTimes  = true;
    bool verifyHash     = true;
    bool createDirs     = true;
    std::uint32_t chunkSize = 256 * 1024;
    std::uint32_t maxInflightChunks = 4;

    std::chrono::milliseconds progressInterval{50};

    std::uint32_t downloadWindowSize = 4;

    // ---- Compression ----
    Compression compression = Compression::None;
    int compressionLevel    = 3;   // zstd level: 1 (fast) to 22 (max)
};

// ---------------------------------------------------------------
// Sync
// ---------------------------------------------------------------

struct SyncOptions {
    SyncMode mode = SyncMode::Upload;
    CompareMethod compareBy = CompareMethod::SizeAndMtime;
    bool preserveTimes = true;
    bool verifyHash = true;
    bool deleteExtra = false;
    bool dryRun = false;
    std::uint32_t maxConcurrentTransfers = 4;
    std::chrono::seconds mtimeTolerance{2};
    std::function<bool(const std::string& relativePath)> filter;
};

struct SyncProgress {
    std::uint32_t filesTotal       = 0;
    std::uint32_t filesDone        = 0;
    std::uint64_t bytesTotal       = 0;
    std::uint64_t bytesDone        = 0;
    std::string   currentOperation;
    std::string   currentPath;
    TransferProgress currentFile;
    double overallPercent() const {
        return bytesTotal ? (100.0 * double(bytesDone) / double(bytesTotal)) : 0.0;
    }
};

struct SyncResult {
    std::uint32_t uploaded  = 0;
    std::uint32_t downloaded= 0;
    std::uint32_t deleted   = 0;
    std::uint32_t skipped   = 0;
    std::uint32_t failed    = 0;
    std::uint64_t bytesTransferred = 0;
    std::vector<std::string> errors;
};

struct SyncFileResult {
    enum class Action {
        Skipped,
        Uploaded,
        Downloaded,
        DeletedLocal,
        DeletedRemote,
        None
    };

    Action action = Action::None;
    std::uint64_t bytesTransferred = 0;
    bool verified = false;
    std::string message;
};

struct TransferResult {
    std::uint64_t bytesTransferred = 0;
    std::uint64_t elapsedMs        = 0;
    std::string   xxhashSource;
    std::string   xxhashDest;
    bool          verified         = false;
};

using ProgressCallback     = std::function<void(const TransferProgress&)>;
using SyncProgressCallback = std::function<void(const SyncProgress&)>;

using CancellationToken = std::shared_ptr<std::atomic<bool>>;
inline CancellationToken make_cancellation_token() {
    return std::make_shared<std::atomic<bool>>(false);
}

struct FileMeta {
    std::string   relativePath;
    std::uint64_t size = 0;
    std::int64_t  mtimeNs = 0;
    std::uint64_t xxhash = 0;
    bool isDirectory = false;
};

} // namespace filexferlib
