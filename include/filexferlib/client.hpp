#pragma once
#include "filexferlib/outcome.hpp"
#include "filexferlib/detail/session.hpp"
#include "filexferlib/types.hpp"
#include "filexferlib/logger.hpp"
#include "filexferlib/transfer_handle.hpp"

#include <asio.hpp>
#include <memory>
#include <thread>
#include <future>
#include <atomic>
#include <string>
#include <cstdint>
#include <mutex>
#include <filesystem>

namespace filexferlib {

class Client {
public:
    Client();
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // ============================================================
    // Connection
    // ============================================================

    /**
     * @brief Connect to a server (blocking).
     *
     * Blocks until the session is established or fails.
     *
     * @param host Server hostname or IP address.
     * @param port Server port.
     * @param opts Connection options (auth, timeouts, reconnect).
     * @return Session info (server version, etc.).
     * @throws std::system_error on failure.
     *
     * @see connectAsync, disconnect
     */
    SessionInfo connect(const std::string& host,
                        std::uint16_t port,
                        ConnectOptions opts = {});

    std::future<SessionInfo> connectAsync(std::string host,
                                          std::uint16_t port,
                                          ConnectOptions opts = {});

    void disconnect();
    bool isConnected() const;

    // ============================================================
    // Generic unary call
    // ============================================================

    std::future<GenericResponse> call(std::string method,
                                      ByteBuffer payload = {},
                                      CallOptions opts = {});

    // ============================================================
    // File transfer Ã¢â‚¬â€ Local <-> Remote
    // ============================================================

    /**
     * @brief Upload a local file to the remote.
     *
     * Returns immediately with a future. The transfer runs on the
     * client's io thread. Progress is reported via the callback.
     *
     * If the server has a partial `.fxfer.tmp` for the destination,
     * the upload resumes from that offset automatically.
     *
     * @param local Path to the local file.
     * @param remotePath Destination path on the server (POSIX-style).
     * @param opts Transfer options (chunk size, verification, etc.).
     * @param onProgress Optional progress callback.
     * @param cancel Optional cancellation token.
     * @return A future that resolves to a TransferResult.
     * @throws std::system_error on failure (see ErrorCode).
     *
     * @see download, uploadWithHandle, make_cancellation_token
     */
    std::future<TransferResult> upload(
        const std::filesystem::path& local,
        const std::string& remotePath,
        CopyOptions opts = {},
        ProgressCallback onProgress = {},
        CancellationToken cancel = {});

    /**
     * @brief Download a remote file to the local filesystem.
     *
     * If a local `<local>.fxfer.part` file exists, the download
     * resumes from its size automatically. On success, the part
     * file is renamed to the final path.
     *
     * @param remotePath Path on the server (POSIX-style).
     * @param local Destination path on the local filesystem.
     * @param opts Transfer options.
     * @param onProgress Optional progress callback.
     * @param cancel Optional cancellation token.
     * @return A future that resolves to a TransferResult.
     * @throws std::system_error on failure.
     *
     * @see upload, downloadWithHandle
     */
    std::future<TransferResult> download(
        const std::string& remotePath,
        const std::filesystem::path& local,
        CopyOptions opts = {},
        ProgressCallback onProgress = {},
        CancellationToken cancel = {});

    struct UploadResult {
        TransferHandle handle;
        std::future<TransferResult> future;
    };

    struct DownloadResult {
        TransferHandle handle;
        std::future<TransferResult> future;
    };

    UploadResult uploadWithHandle(
        const std::filesystem::path& local,
        const std::string& remotePath,
        CopyOptions opts = {},
        ProgressCallback onProgress = {});

    DownloadResult downloadWithHandle(
        const std::string& remotePath,
        const std::filesystem::path& local,
        CopyOptions opts = {},
        ProgressCallback onProgress = {});

    // ============================================================
    // File transfer Copy Remote <-> Remote
    // ============================================================

    std::future<TransferResult> copyRemoteToRemote(
        const std::string& srcRemote,
        const std::string& dstRemote,
        CopyOptions opts = {},
        ProgressCallback onProgress = {},
        CancellationToken cancel = {});

    // ============================================================
    // Remote directory operations
    // ============================================================

    std::future<std::vector<FileMeta>> listRemote(const std::string& remoteDir);
    std::future<void> deleteRemote(const std::string& remotePath,
                                   bool recursive = false);
    std::future<void> renameRemote(const std::string& oldPath,
                                   const std::string& newPath);
    std::future<void> setRemoteMtime(const std::string& remotePath,
                                     std::int64_t mtimeNs);
    std::future<void> mkdirRemote(const std::string& remotePath,
                                  bool parents = true);

    // ============================================================
    // Exception-free variants
    // ============================================================
    //
    // Each try* method is identical to its non-try counterpart but
    // returns an Outcome<T> instead of throwing. Use these when you
    // don't want exceptions (e.g. in noexcept contexts, or when
    // mixing with C-style error handling).
    //
    // Every try* method is a thin wrapper that catches the exceptions
    // thrown by the corresponding non-try method.

    Outcome<SessionInfo> tryConnect(const std::string& host,
                                    std::uint16_t port,
                                    ConnectOptions opts = {});

    std::future<Outcome<SessionInfo>> tryConnectAsync(std::string host,
                                                      std::uint16_t port,
                                                      ConnectOptions opts = {});

    std::future<Outcome<TransferResult>> tryUpload(
        const std::filesystem::path& local,
        const std::string& remotePath,
        CopyOptions opts = {},
        ProgressCallback onProgress = {},
        CancellationToken cancel = {});

    std::future<Outcome<TransferResult>> tryDownload(
        const std::string& remotePath,
        const std::filesystem::path& local,
        CopyOptions opts = {},
        ProgressCallback onProgress = {},
        CancellationToken cancel = {});

    std::future<Outcome<SyncResult>> trySyncFolder(
        const std::filesystem::path& localDir,
        const std::string& remoteDir,
        SyncOptions opts = {},
        SyncProgressCallback onProgress = {},
        CancellationToken cancel = {});

    std::future<Outcome<SyncFileResult>> trySyncFile(
        const std::filesystem::path& localFile,
        const std::string& remotePath,
        SyncOptions opts = {},
        ProgressCallback onProgress = {});

    std::future<Outcome<std::vector<FileMeta>>> tryListRemote(
        const std::string& remoteDir);

    std::future<Outcome<void>> tryDeleteRemote(
        const std::string& remotePath,
        bool recursive = false);

    std::future<Outcome<void>> trySetRemoteMtime(
        const std::string& remotePath,
        std::int64_t mtimeNs);

    std::future<Outcome<void>> tryMkdirRemote(
        const std::string& remotePath,
        bool parents = true);

    std::future<Outcome<void>> tryRenameRemote(
        const std::string& oldPath,
        const std::string& newPath);

    std::future<Outcome<TransferResult>> tryCopyRemoteToRemote(
        const std::string& srcRemote,
        const std::string& dstRemote,
        CopyOptions opts = {},
        ProgressCallback onProgress = {},
        CancellationToken cancel = {});

    std::future<Outcome<GenericResponse>> tryCall(
        std::string method,
        ByteBuffer payload = {},
        CallOptions opts = {});

    // ============================================================
    // Folder sync
    // ============================================================

    /**
     * @brief Synchronize two folders.
     *
     * Supported modes: Upload, Download, Mirror, Bidirectional.
     * Comparison can be by size, size+mtime, or XXHash.
     *
     * @param localDir Local directory.
     * @param remoteDir Remote directory (POSIX-style).
     * @param opts Sync options (mode, comparison, filters).
     * @param onProgress Optional progress callback.
     * @param cancel Optional cancellation token.
     * @return A future that resolves to a SyncResult.
     * @throws std::system_error on failure.
     *
     * @see SyncOptions, SyncMode, CompareMethod
     */
    std::future<SyncResult> syncFolder(
        const std::filesystem::path& localDir,
        const std::string& remoteDir,
        SyncOptions opts = {},
        SyncProgressCallback onProgress = {},
        CancellationToken cancel = {});

    // ---------------------------------------------------------------
    // syncFile
    // ---------------------------------------------------------------

    /**
     * @brief Synchronize a single file.
     *
     * Compares the local and remote copies using `SyncOptions::compareBy`.
     * If they differ, the direction is decided by `SyncOptions::mode`:
     *
     * - `Upload`, `MirrorUpload`: local → remote (unless deleteExtra and
     *   file only exists on remote)
     * - `Download`, `MirrorDownload`: remote → local
     * - `Bidirectional`: whichever is newer (by mtime) wins
     *
     * @param localFile Path to the local file.
     * @param remotePath Path on the server (POSIX-style).
     * @param opts Sync options (mode, comparison method, etc.).
     * @param onProgress Optional progress callback for the transfer.
     * @return A future that resolves to a SyncFileResult.
     */
    std::future<SyncFileResult> syncFile(
        const std::filesystem::path& localFile,
        const std::string& remotePath,
        SyncOptions opts = {},
        ProgressCallback onProgress = {});

    // ============================================================
    // Internals (for detail/ use only)
    // ============================================================

    asio::io_context& io() { return io_; }
    std::shared_ptr<detail::Session> session() const { return session_; }
    const ConnectOptions& connectOpts() const { return connectOpts_; }

private:
    void ensure_io_thread();
    void do_connect_attempt(std::string host, std::uint16_t port,
                            ConnectOptions opts,
                            std::shared_ptr<std::promise<SessionInfo>> pr,
                            int attempt);
    bool shouldRetry(int attempt, const ConnectOptions& opts,
                     std::shared_ptr<std::promise<SessionInfo>> pr,
                     std::error_code ec);
    void scheduleReconnect(const ConnectOptions& opts);

    void resolve_pending_connect(std::exception_ptr error);

    UploadResult uploadInternal(
        const std::filesystem::path& local,
        const std::string& remotePath,
        CopyOptions opts,
        ProgressCallback onProgress,
        CancellationToken cancel);

    DownloadResult downloadInternal(
        const std::string& remotePath,
        const std::filesystem::path& local,
        CopyOptions opts,
        ProgressCallback onProgress,
        CancellationToken cancel);

    asio::io_context io_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    std::thread ioThread_;
    std::shared_ptr<detail::Session> session_;
    std::atomic<bool> connected_{false};
    ConnectOptions connectOpts_;

    std::string host_;
    std::uint16_t port_ = 0;
    std::atomic<bool> manualDisconnect_{false};
    std::atomic<bool> reconnecting_{false};
    std::atomic<int>  reconnectAttempt_{0};

    // Pending connect promise used to resolve a client's initial
    // connect() / connectAsync() call once (either on success or after
    // all retries are exhausted).
    std::shared_ptr<std::promise<SessionInfo>> pendingConnectPromise_;
    std::mutex connectPromiseMu_;
};

} // namespace filexferlib
