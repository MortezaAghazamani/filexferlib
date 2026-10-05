#pragma once
#include "filexferlib/detail/session.hpp"
#include "filexferlib/detail/hash.hpp"
#include "filexferlib/detail/transfer_registry.hpp"
#include "filexferlib/detail/transfer_timer.hpp"
#include "filexferlib/detail/rate_limiter.hpp"
#include "filexferlib/types.hpp"
#include "filexferlib/logger.hpp"

#include <asio.hpp>
#include <memory>
#include <thread>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <fstream>
#include <filesystem>
#include <vector>
#include <map>
#include <chrono>
#include <cstdint>

namespace filexferlib {

class GenericContext {
public:
    GenericContext(std::shared_ptr<detail::Session> session,
                   std::uint32_t requestId,
                   ByteBuffer payload)
        : session_(std::move(session))
        , requestId_(requestId)
        , request_(std::move(payload)) {}

    const ByteBuffer& request() const { return request_; }
    void respond(ByteBuffer payload = {}, std::uint16_t statusCode = 0);

private:
    std::shared_ptr<detail::Session> session_;
    std::uint32_t requestId_;
    ByteBuffer    request_;
    bool          responded_ = false;
};

// Internal type for ongoing downloads (server -> client).
struct OutgoingFile {
    std::shared_ptr<detail::Session> sess;
    std::filesystem::path path;
    std::ifstream file;
    std::vector<char> buf;
    std::uint64_t size = 0;
    std::uint64_t offset = 0;
    std::uint64_t hash = 0;
    std::int64_t  mtimeNs = 0;
    std::uint32_t transferId = 0;
    std::size_t   chunkSize = 256 * 1024;
    bool          finished = false;

    // Pipelining
    std::uint32_t windowSize = 4;
    std::uint32_t inflight   = 0;
    std::uint64_t ackedOffset = 0;
    bool          waitingForToken = false;

    // Compression
    bool compressed = false;
    int  compressionLevel = 3;
};

class Server {
public:
    using GenericHandler = std::function<void(GenericContext&)>;

    explicit Server(ServerConfig cfg);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    std::uint16_t start(const std::string& bindAddress, std::uint16_t port);
    void stop();

    void registerHandler(std::string method, GenericHandler handler);

    asio::io_context& io() { return io_; }
    const ServerConfig& config() const { return cfg_; }

private:
    struct IncomingKey {
        detail::Session* sess;
        std::uint32_t tid;
        bool operator==(const IncomingKey& o) const {
            return sess == o.sess && tid == o.tid;
        }
    };
    struct IncomingKeyHash {
        std::size_t operator()(const IncomingKey& k) const noexcept {
            return std::hash<void*>{}(k.sess) ^
                   (std::hash<std::uint32_t>{}(k.tid) << 1);
        }
    };

    void do_accept();
    void handle_generic(std::shared_ptr<detail::Session> sess, detail::Frame f);

    std::filesystem::path resolvePath(const std::string& remote) const;

    detail::TransferRegistry::Ptr get_registry(std::shared_ptr<detail::Session> sess);

    // upload
    void handle_file_begin(std::shared_ptr<detail::Session>, detail::Frame);
    void handle_file_chunk(std::shared_ptr<detail::Session>, detail::Frame);
    void handle_file_end  (std::shared_ptr<detail::Session>, detail::Frame);
    void handle_file_resume_req(std::shared_ptr<detail::Session>, detail::Frame);

    // download
    void handle_download_request(std::shared_ptr<detail::Session>, detail::Frame,
                                 std::uint32_t tid);
    void handle_download_resume_req(std::shared_ptr<detail::Session>, detail::Frame);
    void handle_upload_ack(std::shared_ptr<detail::Session>, detail::Frame,
                           std::uint32_t tid);
    void send_next_chunk(std::shared_ptr<OutgoingFile> of);
    void fill_window(std::shared_ptr<OutgoingFile> of);

    // other ops
    void handle_list      (std::shared_ptr<detail::Session>, detail::Frame);
    void handle_delete    (std::shared_ptr<detail::Session>, detail::Frame);
    void handle_rename    (std::shared_ptr<detail::Session>, detail::Frame);
    void handle_set_mtime (std::shared_ptr<detail::Session>, detail::Frame);
    void handle_mkdir     (std::shared_ptr<detail::Session>, detail::Frame);
    void handle_remote_copy(std::shared_ptr<detail::Session>, detail::Frame);

    void cleanup_stale_temp_files();

    struct IncomingFile {
        void* owner = nullptr;
        std::filesystem::path tmpPath;
        std::filesystem::path finalPath;
        std::ofstream out;
        std::uint64_t expectedSize = 0;
        std::int64_t  mtimeNs = 0;
        std::uint64_t expectedHash = 0;
        std::uint64_t receivedBytes = 0;
        bool compressed = false;   // <-- compression flag
        detail::FileHasher hasher;
        std::map<std::uint64_t, ByteBuffer> pendingChunks;
    };

    ServerConfig cfg_;
    asio::io_context io_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    asio::ip::tcp::acceptor acceptor_;
    std::thread ioThread_;
    std::atomic<bool> running_{false};

    detail::RateLimiter::Ptr rateLimiter_;

    std::unordered_map<std::string, GenericHandler> handlers_;
    std::mutex handlersMu_;

    std::vector<std::shared_ptr<detail::Session>> sessions_;
    std::mutex sessionsMu_;

    std::unordered_map<IncomingKey, std::shared_ptr<IncomingFile>, IncomingKeyHash> incoming_;
    std::mutex incomingMu_;

    std::unordered_map<std::uint32_t, std::shared_ptr<OutgoingFile>> outgoing_;
    std::mutex outgoingMu_;

    std::unordered_map<detail::Session*, detail::TransferRegistry::Ptr> transferRegs_;
    std::mutex transferRegsMu_;

    std::unordered_map<detail::Session*, std::uint64_t> pendingDownloadResumeOffset_;
    std::mutex pendingResumeMu_;
};

} // namespace filexferlib
