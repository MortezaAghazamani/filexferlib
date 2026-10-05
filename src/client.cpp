#include "filexferlib/client.hpp"
#include "filexferlib/detail/protocol.hpp"
#include "filexferlib/detail/transfer_util.hpp"
#include "filexferlib/logger.hpp"

#include <stdexcept>
#include <cstring>
#include <fstream>
#include <thread>

namespace filexferlib {

Client::Client()
    : work_(asio::make_work_guard(io_)) {}

Client::~Client() {
    disconnect();
    work_.reset();

    if (ioThread_.joinable()) {
        if (std::this_thread::get_id() == ioThread_.get_id()) {
            ioThread_.detach();
        } else {
            ioThread_.join();
        }
    }

    // If a pending connect promise is still around, resolve it with
    // an error so no one is left waiting.
    resolve_pending_connect(std::make_exception_ptr(
        std::runtime_error("client destroyed during connect")));
}

void Client::ensure_io_thread() {
    if (!ioThread_.joinable()) {
        ioThread_ = std::thread([this]{
            try {
                io_.run();
            } catch (const std::exception& e) {
                FX_ERROR("client io thread exception: ", e.what());
            } catch (...) {
                FX_ERROR("client io thread unknown exception");
            }
        });
    }
}

// ---------------------------------------------------------------
// connect / connectAsync
// ---------------------------------------------------------------

SessionInfo Client::connect(const std::string& host, std::uint16_t port,
                            ConnectOptions opts) {
    manualDisconnect_.store(false);
    auto fut = connectAsync(host, port, opts);
    return fut.get();
}

std::future<SessionInfo>
Client::connectAsync(std::string host, std::uint16_t port, ConnectOptions opts) {
    using namespace detail;

    manualDisconnect_.store(false);
    ensure_io_thread();
    connectOpts_ = opts;
    host_ = host;
    port_ = port;
    reconnectAttempt_.store(0);

    auto pr  = std::make_shared<std::promise<SessionInfo>>();
    auto fut = pr->get_future();

    {
        std::lock_guard<std::mutex> lk(connectPromiseMu_);
        pendingConnectPromise_ = pr;
    }

    do_connect_attempt(host, port, opts, pr, 1);
    return fut;
}

// ---------------------------------------------------------------
// do_connect_attempt
// ---------------------------------------------------------------

void Client::do_connect_attempt(std::string host, std::uint16_t port,
                                ConnectOptions opts,
                                std::shared_ptr<std::promise<SessionInfo>> pr,
                                int attempt) {
    using namespace detail;

    FX_INFO("connect attempt ", attempt, " -> ", host, ":", port);

    auto resolver = std::make_shared<asio::ip::tcp::resolver>(io_);
    resolver->async_resolve(host, std::to_string(port),
        [this, pr, resolver, opts, host, port, attempt]
        (std::error_code ec, asio::ip::tcp::resolver::results_type results) {
            if (ec) {
                if (shouldRetry(attempt, opts, pr, ec)) return;
                return;
            }

            auto sock = std::make_shared<asio::ip::tcp::socket>(io_);
            asio::async_connect(*sock, results,
                [this, pr, sock, opts, host, port, attempt]
                (std::error_code ec, const asio::ip::tcp::endpoint&) {
                    if (ec) {
                        if (shouldRetry(attempt, opts, pr, ec)) return;
                        return;
                    }

                    auto sess = std::make_shared<detail::Session>(io_, Limits{});
                    sess->set_close_handler([this, opts](std::error_code closeEc){
                        connected_.store(false);
                        if (closeEc) {
                            FX_WARN("session closed: ", closeEc.message());
                        } else {
                            FX_DEBUG("session closed (normal)");
                        }
                        if (opts.autoReconnect && !manualDisconnect_.load()) {
                            scheduleReconnect(opts);
                        }
                    });
                    session_ = sess;
                    sess->start(std::move(*sock));

                    ByteBuffer hello;
                    const std::string& tok = opts.authToken;
                    hello.resize(3 + tok.size());
                    hello[0] = std::byte(detail::kProtocolVersion);
                    detail::put_u16(hello.data() + 1,
                                    static_cast<std::uint16_t>(tok.size()));
                    if (!tok.empty())
                        std::memcpy(hello.data() + 3, tok.data(), tok.size());

                    sess->set_message_handler(
                        [this, pr, sess](detail::Frame f) mutable {
                            if (f.type == detail::MsgType::HelloAck) {
                                detail::HelloAckData ad;
                                SessionInfo info;

                                if (detail::decode_hello_ack(f.payload, ad)) {
                                    info.serverVersion = ad.serverVersion;

                                    connectOpts_.maxBytesPerSecond =
                                        ad.maxBytesPerSecond;
                                    connectOpts_.recommendedChunkSize =
                                        ad.recommendedChunkSize;

                                    if (ad.recommendedTimeoutMs >
                                        static_cast<std::uint32_t>(
                                            connectOpts_.readTimeout.count()))
                                    {
                                        connectOpts_.readTimeout =
                                            std::chrono::milliseconds(
                                                ad.recommendedTimeoutMs);
                                        FX_INFO("adjusted readTimeout to ",
                                                ad.recommendedTimeoutMs, " ms");
                                    }

                                    if (ad.recommendedChunkSize > 0) {
                                        FX_INFO("server recommends chunkSize=",
                                                ad.recommendedChunkSize,
                                                " bytes (rate limit ",
                                                ad.maxBytesPerSecond, " B/s)");
                                    }
                                } else {
                                    // Fallback: old-style HelloAck
                                    if (f.payload.size() >= 1) {
                                        info.serverVersion.assign(
                                            reinterpret_cast<const char*>(
                                                f.payload.data() + 1),
                                            f.payload.size() - 1);
                                    }
                                }

                                FX_INFO("connected: ", info.serverVersion);

                                // Resolve whichever promise we have (the
                                // one passed in, or the pending one).
                                if (pr) {
                                    try { pr->set_value(std::move(info)); }
                                    catch (...) {}
                                }
                                resolve_pending_connect(nullptr);

                                sess->set_message_handler({});
                            } else if (f.type == detail::MsgType::Error) {
                                FX_ERROR("server rejected hello");
                                auto eptr = std::make_exception_ptr(
                                    std::runtime_error("server rejected hello"));
                                if (pr) {
                                    try { pr->set_exception(eptr); }
                                    catch (...) {}
                                }
                                resolve_pending_connect(eptr);
                                sess->close();
                            }
                        });

                    sess->send(detail::MsgType::Hello, std::move(hello));
                    connected_.store(true);
                });
        });
}

// ---------------------------------------------------------------
// shouldRetry
// ---------------------------------------------------------------

bool Client::shouldRetry(int attempt, const ConnectOptions& opts,
                         std::shared_ptr<std::promise<SessionInfo>> pr,
                         std::error_code ec) {
    FX_ERROR("connect failed (attempt ", attempt, "): ", ec.message());

    if (!opts.autoReconnect ||
        attempt >= static_cast<int>(opts.maxReconnectAttempts)) {
        // All retries exhausted Ã¢â€ â€™ resolve the pending promise with error
        auto eptr = std::make_exception_ptr(
            std::system_error(ec, "connect"));
        if (pr) {
            try { pr->set_exception(eptr); } catch (...) {}
        }
        resolve_pending_connect(eptr);
        return false;
    }

    scheduleReconnect(opts);
    return true;
}

// ---------------------------------------------------------------
// scheduleReconnect
// ---------------------------------------------------------------

void Client::scheduleReconnect(const ConnectOptions& opts) {
    if (reconnecting_.exchange(true)) return;

    int attempt = reconnectAttempt_.load();
    if (attempt > 6) attempt = 6;
    auto delay = opts.reconnectBaseDelay * (1 << attempt);
    if (delay > opts.reconnectMaxDelay) delay = opts.reconnectMaxDelay;
    reconnectAttempt_.fetch_add(1);

    FX_INFO("reconnect in ", delay.count(), "ms (attempt ",
            reconnectAttempt_.load(), ")");

    auto timer = std::make_shared<asio::steady_timer>(io_, delay);
    timer->async_wait([this, opts, timer](std::error_code ec) {
        if (ec) { reconnecting_.store(false); return; }
        if (manualDisconnect_.load()) {
            reconnecting_.store(false);
            return;
        }

        // Pick up the pending connect promise so a successful retry
        // can resolve the original `connect()` / `connectAsync()` call.
        std::shared_ptr<std::promise<SessionInfo>> pr;
        {
            std::lock_guard<std::mutex> lk(connectPromiseMu_);
            pr = pendingConnectPromise_;
        }

        do_connect_attempt(host_, port_, opts, pr, reconnectAttempt_.load());
        reconnecting_.store(false);
    });
}

// ---------------------------------------------------------------
// resolve_pending_connect
// ---------------------------------------------------------------

void Client::resolve_pending_connect(std::exception_ptr error) {
    std::shared_ptr<std::promise<SessionInfo>> pr;
    {
        std::lock_guard<std::mutex> lk(connectPromiseMu_);
        pr = pendingConnectPromise_;
        pendingConnectPromise_.reset();
    }
    if (!pr) return;

    if (error) {
        try { pr->set_exception(error); } catch (...) {}
    } else {
        // We already set the value above (see the HelloAck handler);
        // only set here if the caller explicitly says so.
        // This function is used to clear pending on error or when
        // the caller already set the value and wants to release it.
    }
}

// ---------------------------------------------------------------
// disconnect
// ---------------------------------------------------------------

void Client::disconnect() {
    manualDisconnect_.store(true);

    // If a connect is still pending, fail it.
    resolve_pending_connect(std::make_exception_ptr(
        std::runtime_error("client disconnected")));

    if (session_) {
        auto sess = session_;
        auto promise = std::make_shared<std::promise<void>>();
        auto fut = promise->get_future();

        try {
            asio::post(io_, [sess, promise]{
                try { sess->close(); } catch (...) {}
                try { promise->set_value(); } catch (...) {}
            });
            fut.wait_for(std::chrono::seconds(2));
        } catch (...) {
            // ignore
        }
    }
    connected_.store(false);
}

bool Client::isConnected() const { return connected_.load(); }

// ---------------------------------------------------------------
// call
// ---------------------------------------------------------------

std::future<GenericResponse>
Client::call(std::string method, ByteBuffer payload, CallOptions opts) {
    if (!session_)
        throw std::runtime_error("not connected");
    return session_->call_generic(std::move(method), std::move(payload), opts);
}

// ---------------------------------------------------------------
// listRemote
// ---------------------------------------------------------------

std::future<std::vector<FileMeta>>
Client::listRemote(const std::string& remoteDir) {
    return std::async(std::launch::async, [this, remoteDir]() {
        using namespace detail;
        if (!session_) throw std::runtime_error("not connected");

        ByteBuffer req;
        put_str(req, remoteDir);

        auto pr = std::make_shared<std::promise<ByteBuffer>>();
        auto fut = pr->get_future();
        session_->set_message_handler(
            [pr](detail::Frame f) {
                if (f.type == detail::MsgType::ListResp)
                    pr->set_value(std::move(f.payload));
            });
        session_->send(detail::MsgType::ListReq, std::move(req));
        ByteBuffer resp = fut.get();

        std::vector<FileMeta> out;
        std::size_t off = 0;
        while (off < resp.size()) {
            std::size_t used = 0;
            std::string rel = get_str(resp.data() + off, resp.size() - off, used);
            if (used == 0) break;
            off += used;
            if (resp.size() < off + 25) break;
            FileMeta m;
            m.relativePath = std::move(rel);
            m.size    = get_u64(resp.data() + off);
            m.mtimeNs = get_i64(resp.data() + off + 8);
            m.xxhash  = get_u64(resp.data() + off + 16);
            m.isDirectory = std::to_integer<u8>(resp[off + 24]) != 0;
            off += 25;
            out.push_back(std::move(m));
        }
        return out;
    });
}

// ---------------------------------------------------------------
// deleteRemote
// ---------------------------------------------------------------

std::future<void> Client::deleteRemote(const std::string& remotePath,
                                       bool recursive) {
    return std::async(std::launch::async, [this, remotePath, recursive]() {
        using namespace detail;
        if (!session_) throw std::runtime_error("not connected");

        ByteBuffer req;
        put_str(req, remotePath);
        req.push_back(std::byte(recursive ? 1 : 0));

        auto pr = std::make_shared<std::promise<std::pair<bool,std::string>>>();
        auto fut = pr->get_future();
        session_->set_message_handler(
            [pr](detail::Frame f) {
                if (f.type == detail::MsgType::DeleteAck) {
                    bool ok = f.payload.size() >= 1 &&
                              std::to_integer<u8>(f.payload[0]) != 0;
                    std::string msg;
                    if (f.payload.size() > 1) {
                        std::size_t used = 0;
                        msg = get_str(f.payload.data() + 1,
                                      f.payload.size() - 1, used);
                    }
                    pr->set_value({ok, std::move(msg)});
                }
            });
        session_->send(detail::MsgType::DeleteReq, std::move(req));
        auto kv = fut.get();
        if (!kv.first) throw std::runtime_error("delete failed: " + kv.second);
    });
}

// ---------------------------------------------------------------
// setRemoteMtime
// ---------------------------------------------------------------

std::future<void> Client::setRemoteMtime(const std::string& remotePath,
                                         std::int64_t mtimeNs) {
    return std::async(std::launch::async, [this, remotePath, mtimeNs]() {
        using namespace detail;
        if (!session_) throw std::runtime_error("not connected");

        ByteBuffer req;
        put_str(req, remotePath);
        const auto base = req.size();
        req.resize(base + 8);
        put_i64(req.data() + base, mtimeNs);

        auto pr = std::make_shared<std::promise<std::pair<bool,std::string>>>();
        auto fut = pr->get_future();
        session_->set_message_handler(
            [pr](detail::Frame f) {
                if (f.type == detail::MsgType::SetMtimeAck) {
                    bool ok = f.payload.size() >= 1 &&
                              std::to_integer<u8>(f.payload[0]) != 0;
                    std::string msg;
                    if (f.payload.size() > 1) {
                        std::size_t used = 0;
                        msg = get_str(f.payload.data() + 1,
                                      f.payload.size() - 1, used);
                    }
                    pr->set_value({ok, std::move(msg)});
                }
            });
        session_->send(detail::MsgType::SetMtimeReq, std::move(req));
        auto kv = fut.get();
        if (!kv.first) throw std::runtime_error("set_mtime failed: " + kv.second);
    });
}

// ---------------------------------------------------------------
// mkdirRemote
// ---------------------------------------------------------------

std::future<void> Client::mkdirRemote(const std::string& remotePath,
                                      bool parents) {
    return std::async(std::launch::async, [this, remotePath, parents]() {
        using namespace detail;
        if (!session_) throw std::runtime_error("not connected");

        ByteBuffer req;
        put_str(req, remotePath);
        req.push_back(std::byte(parents ? 1 : 0));

        auto pr = std::make_shared<std::promise<std::pair<bool,std::string>>>();
        auto fut = pr->get_future();
        session_->set_message_handler(
            [pr](detail::Frame f) {
                if (f.type == detail::MsgType::MkdirAck) {
                    bool ok = f.payload.size() >= 1 &&
                              std::to_integer<u8>(f.payload[0]) != 0;
                    std::string msg;
                    if (f.payload.size() > 1) {
                        std::size_t used = 0;
                        msg = get_str(f.payload.data() + 1,
                                      f.payload.size() - 1, used);
                    }
                    pr->set_value({ok, std::move(msg)});
                }
            });
        session_->send(detail::MsgType::MkdirReq, std::move(req));
        auto kv = fut.get();
        if (!kv.first) throw std::runtime_error("mkdir failed: " + kv.second);
    });
}

// ---------------------------------------------------------------
// copyRemoteToRemote
// ---------------------------------------------------------------

std::future<TransferResult>
Client::copyRemoteToRemote(const std::string& srcRemote,
                           const std::string& dstRemote,
                           CopyOptions,
                           ProgressCallback onProgress,
                           CancellationToken)
{
    return std::async(std::launch::async,
        [this, srcRemote, dstRemote, onProgress]() {
        using namespace detail;
        if (!session_) throw std::runtime_error("not connected");

        ByteBuffer req;
        put_str(req, srcRemote);
        put_str(req, dstRemote);

        auto pr = std::make_shared<std::promise<std::pair<bool,ByteBuffer>>>();
        auto fut = pr->get_future();
        session_->set_message_handler(
            [pr](detail::Frame f) {
                if (f.type == detail::MsgType::RemoteCopyAck) {
                    bool ok = f.payload.size() >= 1 &&
                              std::to_integer<u8>(f.payload[0]) != 0;
                    pr->set_value({ok, std::move(f.payload)});
                }
            });
        session_->send(detail::MsgType::RemoteCopyReq, std::move(req));

        auto kv = fut.get();
        TransferResult r;
        if (kv.second.size() >= 9) r.bytesTransferred = get_u64(kv.second.data() + 1);
        if (!kv.first) {
            std::string msg;
            if (kv.second.size() > 9) {
                std::size_t used = 0;
                msg = get_str(kv.second.data() + 9, kv.second.size() - 9, used);
            }
            throw std::runtime_error("remote copy failed: " + msg);
        }
        if (onProgress) {
            TransferProgress p;
            p.bytesTransferred = r.bytesTransferred;
            p.totalBytes = r.bytesTransferred;
            p.currentPath = dstRemote;
            onProgress(p);
        }
        return r;
    });
}

// ---------------------------------------------------------------
// syncFile
// ---------------------------------------------------------------

std::future<SyncFileResult> Client::syncFile(
    const std::filesystem::path& localFile,
    const std::string& remotePath,
    SyncOptions opts,
    ProgressCallback onProgress)
{
    return std::async(std::launch::async,
        [this, localFile, remotePath, opts, onProgress]() {
        using namespace detail;

        if (!session_) throw std::runtime_error("not connected");

        SyncFileResult result;

        // ---- Gather local metadata ----
        bool hasLocal = false;
        FileMeta localMeta;
        std::error_code ec;
        if (std::filesystem::exists(localFile, ec) &&
            std::filesystem::is_regular_file(localFile, ec))
        {
            hasLocal = true;
            localMeta.relativePath = remotePath;
            localMeta.isDirectory = false;
            localMeta.size = std::filesystem::file_size(localFile, ec);
            if (ec) {
                result.action = SyncFileResult::Action::None;
                result.message = "cannot stat local file: " + ec.message();
                return result;
            }
            auto t = std::filesystem::last_write_time(localFile, ec);
            if (!ec) {
                localMeta.mtimeNs = std::chrono::duration_cast<
                    std::chrono::nanoseconds>(t.time_since_epoch()).count();
            }
        }

        // ---- Gather remote metadata ----
        bool hasRemote = false;
        FileMeta remoteMeta;
        {
            // Extract parent dir and filename from remotePath
            std::string parent = "/";
            std::string name = remotePath;
            auto slash = remotePath.find_last_of('/');
            if (slash != std::string::npos) {
                parent = (slash == 0) ? "/" : remotePath.substr(0, slash);
                name = remotePath.substr(slash + 1);
            }

            try {
                auto list = listRemote(parent).get();
                for (auto& m : list) {
                    if (m.isDirectory) continue;
                    std::string fullPath = parent;
                    if (fullPath.size() > 1 &&
                        fullPath.back() == '/') {
                        fullPath.pop_back();
                    }
                    if (m.relativePath == ("/" + name) ||
                        m.relativePath == name)
                    {
                        hasRemote = true;
                        remoteMeta = m;
                        remoteMeta.relativePath = remotePath;
                        break;
                    }
                }
            } catch (const std::exception&) {
                // parent doesn't exist — treat as no remote
            }
        }

        // ---- Decide action ----
        if (!hasLocal && !hasRemote) {
            result.action = SyncFileResult::Action::None;
            result.message = "file does not exist on either side";
            return result;
        }

        if (hasLocal && !hasRemote) {
            // Only local
            switch (opts.mode) {
            case SyncMode::Upload:
            case SyncMode::MirrorUpload:
            case SyncMode::Bidirectional: {
                if (opts.dryRun) {
                    result.action = SyncFileResult::Action::Uploaded;
                    result.message = "would upload";
                    return result;
                }
                auto tr = upload(localFile.string(), remotePath,
                    {}, onProgress, {}).get();
                result.action = SyncFileResult::Action::Uploaded;
                result.bytesTransferred = tr.bytesTransferred;
                result.verified = tr.verified;
                return result;
            }
            case SyncMode::Download:
                result.action = SyncFileResult::Action::Skipped;
                result.message = "only exists locally; download mode ignores";
                return result;
            case SyncMode::MirrorDownload:
                if (!opts.deleteExtra) {
                    result.action = SyncFileResult::Action::Skipped;
                    result.message = "only exists locally; deleteExtra is off";
                    return result;
                }
                if (opts.dryRun) {
                    result.action = SyncFileResult::Action::DeletedLocal;
                    result.message = "would delete local";
                    return result;
                }
                std::filesystem::remove(localFile, ec);
                result.action = SyncFileResult::Action::DeletedLocal;
                return result;
            }
        }

        if (!hasLocal && hasRemote) {
            // Only remote
            switch (opts.mode) {
            case SyncMode::Download:
            case SyncMode::MirrorDownload:
            case SyncMode::Bidirectional: {
                if (opts.dryRun) {
                    result.action = SyncFileResult::Action::Downloaded;
                    result.message = "would download";
                    return result;
                }
                auto tr = download(remotePath, localFile.string(),
                    {}, onProgress, {}).get();
                result.action = SyncFileResult::Action::Downloaded;
                result.bytesTransferred = tr.bytesTransferred;
                result.verified = tr.verified;
                return result;
            }
            case SyncMode::Upload:
                result.action = SyncFileResult::Action::Skipped;
                result.message = "only exists on remote; upload mode ignores";
                return result;
            case SyncMode::MirrorUpload:
                if (!opts.deleteExtra) {
                    result.action = SyncFileResult::Action::Skipped;
                    result.message = "only exists on remote; deleteExtra is off";
                    return result;
                }
                if (opts.dryRun) {
                    result.action = SyncFileResult::Action::DeletedRemote;
                    result.message = "would delete remote";
                    return result;
                }
                deleteRemote(remotePath, false).get();
                result.action = SyncFileResult::Action::DeletedRemote;
                return result;
            }
        }

        // ---- Both exist — compare ----
        bool same = (localMeta.size == remoteMeta.size);
        if (same && opts.compareBy != CompareMethod::SizeOnly) {
            const auto diff = std::llabs(localMeta.mtimeNs - remoteMeta.mtimeNs);
            const auto tol = std::chrono::duration_cast<
                std::chrono::nanoseconds>(opts.mtimeTolerance).count();
            same = diff <= tol;
        }

        if (same) {
            result.action = SyncFileResult::Action::Skipped;
            result.message = "files are identical";
            return result;
        }

        // Different — decide direction
        bool uploadIt = false;
        switch (opts.mode) {
        case SyncMode::Upload:
        case SyncMode::MirrorUpload:
            uploadIt = true;
            break;
        case SyncMode::Download:
        case SyncMode::MirrorDownload:
            uploadIt = false;
            break;
        case SyncMode::Bidirectional:
            uploadIt = (localMeta.mtimeNs >= remoteMeta.mtimeNs);
            break;
        }

        if (opts.dryRun) {
            result.action = uploadIt ? SyncFileResult::Action::Uploaded
                                     : SyncFileResult::Action::Downloaded;
            result.message = uploadIt ? "would upload" : "would download";
            return result;
        }

        if (uploadIt) {
            auto tr = upload(localFile.string(), remotePath,
                {}, onProgress, {}).get();
            result.action = SyncFileResult::Action::Uploaded;
            result.bytesTransferred = tr.bytesTransferred;
            result.verified = tr.verified;
        } else {
            auto tr = download(remotePath, localFile.string(),
                {}, onProgress, {}).get();
            result.action = SyncFileResult::Action::Downloaded;
            result.bytesTransferred = tr.bytesTransferred;
            result.verified = tr.verified;
        }
        return result;
    });
}

// ---------- renameRemote ----------

std::future<void> Client::renameRemote(const std::string& oldPath,
                                       const std::string& newPath) {
    return std::async(std::launch::async, [this, oldPath, newPath]() {
        using namespace detail;
        if (!session_) throw std::runtime_error("not connected");

        ByteBuffer req = encode_rename_req(oldPath, newPath);

        auto pr = std::make_shared<std::promise<std::pair<bool, std::string>>>();
        auto fut = pr->get_future();

        session_->set_message_handler(
            [pr](detail::Frame f) {
                if (f.type == detail::MsgType::RenameAck) {
                    RenameAckData ack;
                    if (decode_rename_ack(f.payload, ack)) {
                        pr->set_value({ack.ok, ack.msg});
                    } else {
                        pr->set_value({false, "malformed ack"});
                    }
                }
            });

        session_->send(detail::MsgType::RenameReq, std::move(req));
        auto kv = fut.get();
        if (!kv.first) {
            throw std::runtime_error("rename failed: " + kv.second);
        }
    });
}

// =====================================================================
// Exception-free wrappers
// =====================================================================
//
// Each try* method calls the corresponding non-try method inside a
// try/catch and wraps the result (or the exception) into an Outcome.

// ---------- connect ----------

Outcome<SessionInfo>
Client::tryConnect(const std::string& host, std::uint16_t port,
                   ConnectOptions opts)
{
    try {
        return Outcome<SessionInfo>(connect(host, port, opts));
    } catch (const std::system_error& e) {
        return Outcome<SessionInfo>(e.code(), e.what());
    } catch (const std::exception& e) {
        return Outcome<SessionInfo>(
            make_error_code(ErrorCode::InternalError), e.what());
    } catch (...) {
        return Outcome<SessionInfo>(
            make_error_code(ErrorCode::InternalError), "unknown error");
    }
}

std::future<Outcome<SessionInfo>>
Client::tryConnectAsync(std::string host, std::uint16_t port,
                        ConnectOptions opts)
{
    // Wrap the async connect result, mapping exceptions to Outcome.
    auto inner = connectAsync(host, port, opts);
    return std::async(std::launch::deferred,
        [inner = std::move(inner)]() mutable -> Outcome<SessionInfo> {
            try {
                return Outcome<SessionInfo>(inner.get());
            } catch (const std::system_error& e) {
                return Outcome<SessionInfo>(e.code(), e.what());
            } catch (const std::exception& e) {
                return Outcome<SessionInfo>(
                    make_error_code(ErrorCode::InternalError), e.what());
            } catch (...) {
                return Outcome<SessionInfo>(
                    make_error_code(ErrorCode::InternalError), "unknown error");
            }
        });
}

// ---------- upload ----------

std::future<Outcome<TransferResult>>
Client::tryUpload(const std::filesystem::path& local,
                  const std::string& remotePath,
                  CopyOptions opts,
                  ProgressCallback onProgress,
                  CancellationToken cancel)
{
    // Start the transfer (this itself may throw for file_size errors).
    std::future<TransferResult> inner;
    try {
        inner = upload(local, remotePath,
                       std::move(opts), std::move(onProgress),
                       std::move(cancel));
    } catch (const std::system_error& e) {
        std::promise<Outcome<TransferResult>> p;
        p.set_value(Outcome<TransferResult>(e.code(), e.what()));
        return p.get_future();
    } catch (const std::exception& e) {
        std::promise<Outcome<TransferResult>> p;
        p.set_value(Outcome<TransferResult>(
            make_error_code(ErrorCode::InternalError), e.what()));
        return p.get_future();
    }

    // Wrap the result in Outcome.
    return std::async(std::launch::deferred,
        [inner = std::move(inner)]() mutable -> Outcome<TransferResult> {
            try {
                return Outcome<TransferResult>(inner.get());
            } catch (const std::system_error& e) {
                return Outcome<TransferResult>(e.code(), e.what());
            } catch (const std::exception& e) {
                return Outcome<TransferResult>(
                    make_error_code(ErrorCode::InternalError), e.what());
            } catch (...) {
                return Outcome<TransferResult>(
                    make_error_code(ErrorCode::InternalError), "unknown error");
            }
        });
}

// ---------- download ----------

std::future<Outcome<TransferResult>>
Client::tryDownload(const std::string& remotePath,
                    const std::filesystem::path& local,
                    CopyOptions opts,
                    ProgressCallback onProgress,
                    CancellationToken cancel)
{
    std::future<TransferResult> inner;
    try {
        inner = download(remotePath, local,
                         std::move(opts), std::move(onProgress),
                         std::move(cancel));
    } catch (const std::system_error& e) {
        std::promise<Outcome<TransferResult>> p;
        p.set_value(Outcome<TransferResult>(e.code(), e.what()));
        return p.get_future();
    } catch (const std::exception& e) {
        std::promise<Outcome<TransferResult>> p;
        p.set_value(Outcome<TransferResult>(
            make_error_code(ErrorCode::InternalError), e.what()));
        return p.get_future();
    }

    return std::async(std::launch::deferred,
        [inner = std::move(inner)]() mutable -> Outcome<TransferResult> {
            try {
                return Outcome<TransferResult>(inner.get());
            } catch (const std::system_error& e) {
                return Outcome<TransferResult>(e.code(), e.what());
            } catch (const std::exception& e) {
                return Outcome<TransferResult>(
                    make_error_code(ErrorCode::InternalError), e.what());
            } catch (...) {
                return Outcome<TransferResult>(
                    make_error_code(ErrorCode::InternalError), "unknown error");
            }
        });
}

// ---------- syncFolder ----------

std::future<Outcome<SyncResult>>
Client::trySyncFolder(const std::filesystem::path& localDir,
                      const std::string& remoteDir,
                      SyncOptions opts,
                      SyncProgressCallback onProgress,
                      CancellationToken cancel)
{
    std::future<SyncResult> inner;
    try {
        inner = syncFolder(localDir, remoteDir,
                           std::move(opts), std::move(onProgress),
                           std::move(cancel));
    } catch (const std::system_error& e) {
        std::promise<Outcome<SyncResult>> p;
        p.set_value(Outcome<SyncResult>(e.code(), e.what()));
        return p.get_future();
    } catch (const std::exception& e) {
        std::promise<Outcome<SyncResult>> p;
        p.set_value(Outcome<SyncResult>(
            make_error_code(ErrorCode::InternalError), e.what()));
        return p.get_future();
    }

    return std::async(std::launch::deferred,
        [inner = std::move(inner)]() mutable -> Outcome<SyncResult> {
            try {
                return Outcome<SyncResult>(inner.get());
            } catch (const std::system_error& e) {
                return Outcome<SyncResult>(e.code(), e.what());
            } catch (const std::exception& e) {
                return Outcome<SyncResult>(
                    make_error_code(ErrorCode::InternalError), e.what());
            } catch (...) {
                return Outcome<SyncResult>(
                    make_error_code(ErrorCode::InternalError), "unknown error");
            }
        });
}

// ---------- syncFile ----------

std::future<Outcome<SyncFileResult>>
Client::trySyncFile(const std::filesystem::path& localFile,
                    const std::string& remotePath,
                    SyncOptions opts,
                    ProgressCallback onProgress)
{
    std::future<SyncFileResult> inner;
    try {
        inner = syncFile(localFile, remotePath,
                         std::move(opts), std::move(onProgress));
    } catch (const std::system_error& e) {
        std::promise<Outcome<SyncFileResult>> p;
        p.set_value(Outcome<SyncFileResult>(e.code(), e.what()));
        return p.get_future();
    } catch (const std::exception& e) {
        std::promise<Outcome<SyncFileResult>> p;
        p.set_value(Outcome<SyncFileResult>(
            make_error_code(ErrorCode::InternalError), e.what()));
        return p.get_future();
    }

    return std::async(std::launch::deferred,
        [inner = std::move(inner)]() mutable -> Outcome<SyncFileResult> {
            try {
                return Outcome<SyncFileResult>(inner.get());
            } catch (const std::system_error& e) {
                return Outcome<SyncFileResult>(e.code(), e.what());
            } catch (const std::exception& e) {
                return Outcome<SyncFileResult>(
                    make_error_code(ErrorCode::InternalError), e.what());
            } catch (...) {
                return Outcome<SyncFileResult>(
                    make_error_code(ErrorCode::InternalError), "unknown error");
            }
        });
}

// ---------- listRemote ----------

std::future<Outcome<std::vector<FileMeta>>>
Client::tryListRemote(const std::string& remoteDir)
{
    std::future<std::vector<FileMeta>> inner;
    try {
        inner = listRemote(remoteDir);
    } catch (const std::system_error& e) {
        std::promise<Outcome<std::vector<FileMeta>>> p;
        p.set_value(Outcome<std::vector<FileMeta>>(e.code(), e.what()));
        return p.get_future();
    } catch (const std::exception& e) {
        std::promise<Outcome<std::vector<FileMeta>>> p;
        p.set_value(Outcome<std::vector<FileMeta>>(
            make_error_code(ErrorCode::InternalError), e.what()));
        return p.get_future();
    }

    return std::async(std::launch::deferred,
        [inner = std::move(inner)]() mutable
            -> Outcome<std::vector<FileMeta>> {
            try {
                return Outcome<std::vector<FileMeta>>(inner.get());
            } catch (const std::system_error& e) {
                return Outcome<std::vector<FileMeta>>(e.code(), e.what());
            } catch (const std::exception& e) {
                return Outcome<std::vector<FileMeta>>(
                    make_error_code(ErrorCode::InternalError), e.what());
            } catch (...) {
                return Outcome<std::vector<FileMeta>>(
                    make_error_code(ErrorCode::InternalError), "unknown error");
            }
        });
}

// ---------- deleteRemote ----------

std::future<Outcome<void>>
Client::tryDeleteRemote(const std::string& remotePath, bool recursive)
{
    std::future<void> inner;
    try {
        inner = deleteRemote(remotePath, recursive);
    } catch (const std::system_error& e) {
        std::promise<Outcome<void>> p;
        p.set_value(Outcome<void>(e.code(), e.what()));
        return p.get_future();
    } catch (const std::exception& e) {
        std::promise<Outcome<void>> p;
        p.set_value(Outcome<void>(
            make_error_code(ErrorCode::InternalError), e.what()));
        return p.get_future();
    }

    return std::async(std::launch::deferred,
        [inner = std::move(inner)]() mutable -> Outcome<void> {
            try {
                inner.get();
                return Outcome<void>();
            } catch (const std::system_error& e) {
                return Outcome<void>(e.code(), e.what());
            } catch (const std::exception& e) {
                return Outcome<void>(
                    make_error_code(ErrorCode::InternalError), e.what());
            } catch (...) {
                return Outcome<void>(
                    make_error_code(ErrorCode::InternalError), "unknown error");
            }
        });
}

// ---------- setRemoteMtime ----------

std::future<Outcome<void>>
Client::trySetRemoteMtime(const std::string& remotePath,
                          std::int64_t mtimeNs)
{
    std::future<void> inner;
    try {
        inner = setRemoteMtime(remotePath, mtimeNs);
    } catch (const std::system_error& e) {
        std::promise<Outcome<void>> p;
        p.set_value(Outcome<void>(e.code(), e.what()));
        return p.get_future();
    } catch (const std::exception& e) {
        std::promise<Outcome<void>> p;
        p.set_value(Outcome<void>(
            make_error_code(ErrorCode::InternalError), e.what()));
        return p.get_future();
    }

    return std::async(std::launch::deferred,
        [inner = std::move(inner)]() mutable -> Outcome<void> {
            try {
                inner.get();
                return Outcome<void>();
            } catch (const std::system_error& e) {
                return Outcome<void>(e.code(), e.what());
            } catch (const std::exception& e) {
                return Outcome<void>(
                    make_error_code(ErrorCode::InternalError), e.what());
            } catch (...) {
                return Outcome<void>(
                    make_error_code(ErrorCode::InternalError), "unknown error");
            }
        });
}

// ---------- mkdirRemote ----------

std::future<Outcome<void>>
Client::tryMkdirRemote(const std::string& remotePath, bool parents)
{
    std::future<void> inner;
    try {
        inner = mkdirRemote(remotePath, parents);
    } catch (const std::system_error& e) {
        std::promise<Outcome<void>> p;
        p.set_value(Outcome<void>(e.code(), e.what()));
        return p.get_future();
    } catch (const std::exception& e) {
        std::promise<Outcome<void>> p;
        p.set_value(Outcome<void>(
            make_error_code(ErrorCode::InternalError), e.what()));
        return p.get_future();
    }

    return std::async(std::launch::deferred,
        [inner = std::move(inner)]() mutable -> Outcome<void> {
            try {
                inner.get();
                return Outcome<void>();
            } catch (const std::system_error& e) {
                return Outcome<void>(e.code(), e.what());
            } catch (const std::exception& e) {
                return Outcome<void>(
                    make_error_code(ErrorCode::InternalError), e.what());
            } catch (...) {
                return Outcome<void>(
                    make_error_code(ErrorCode::InternalError), "unknown error");
            }
        });
}

// ---------- renameRemote ----------

std::future<Outcome<void>>
Client::tryRenameRemote(const std::string& oldPath,
                        const std::string& newPath)
{
    std::future<void> inner;
    try {
        inner = renameRemote(oldPath, newPath);
    } catch (const std::system_error& e) {
        std::promise<Outcome<void>> p;
        p.set_value(Outcome<void>(e.code(), e.what()));
        return p.get_future();
    } catch (const std::exception& e) {
        std::promise<Outcome<void>> p;
        p.set_value(Outcome<void>(
            make_error_code(ErrorCode::InternalError), e.what()));
        return p.get_future();
    }

    return std::async(std::launch::deferred,
        [inner = std::move(inner)]() mutable -> Outcome<void> {
            try {
                inner.get();
                return Outcome<void>();
            } catch (const std::system_error& e) {
                return Outcome<void>(e.code(), e.what());
            } catch (const std::exception& e) {
                return Outcome<void>(
                    make_error_code(ErrorCode::InternalError), e.what());
            } catch (...) {
                return Outcome<void>(
                    make_error_code(ErrorCode::InternalError), "unknown error");
            }
        });
}

// ---------- copyRemoteToRemote ----------

std::future<Outcome<TransferResult>>
Client::tryCopyRemoteToRemote(const std::string& srcRemote,
                              const std::string& dstRemote,
                              CopyOptions opts,
                              ProgressCallback onProgress,
                              CancellationToken cancel)
{
    std::future<TransferResult> inner;
    try {
        inner = copyRemoteToRemote(srcRemote, dstRemote,
                                   std::move(opts), std::move(onProgress),
                                   std::move(cancel));
    } catch (const std::system_error& e) {
        std::promise<Outcome<TransferResult>> p;
        p.set_value(Outcome<TransferResult>(e.code(), e.what()));
        return p.get_future();
    } catch (const std::exception& e) {
        std::promise<Outcome<TransferResult>> p;
        p.set_value(Outcome<TransferResult>(
            make_error_code(ErrorCode::InternalError), e.what()));
        return p.get_future();
    }

    return std::async(std::launch::deferred,
        [inner = std::move(inner)]() mutable -> Outcome<TransferResult> {
            try {
                return Outcome<TransferResult>(inner.get());
            } catch (const std::system_error& e) {
                return Outcome<TransferResult>(e.code(), e.what());
            } catch (const std::exception& e) {
                return Outcome<TransferResult>(
                    make_error_code(ErrorCode::InternalError), e.what());
            } catch (...) {
                return Outcome<TransferResult>(
                    make_error_code(ErrorCode::InternalError), "unknown error");
            }
        });
}

// ---------- call ----------

std::future<Outcome<GenericResponse>>
Client::tryCall(std::string method, ByteBuffer payload, CallOptions opts)
{
    std::future<GenericResponse> inner;
    try {
        inner = call(std::move(method), std::move(payload), std::move(opts));
    } catch (const std::system_error& e) {
        std::promise<Outcome<GenericResponse>> p;
        p.set_value(Outcome<GenericResponse>(e.code(), e.what()));
        return p.get_future();
    } catch (const std::exception& e) {
        std::promise<Outcome<GenericResponse>> p;
        p.set_value(Outcome<GenericResponse>(
            make_error_code(ErrorCode::InternalError), e.what()));
        return p.get_future();
    }

    return std::async(std::launch::deferred,
        [inner = std::move(inner)]() mutable -> Outcome<GenericResponse> {
            try {
                return Outcome<GenericResponse>(inner.get());
            } catch (const std::system_error& e) {
                return Outcome<GenericResponse>(e.code(), e.what());
            } catch (const std::exception& e) {
                return Outcome<GenericResponse>(
                    make_error_code(ErrorCode::InternalError), e.what());
            } catch (...) {
                return Outcome<GenericResponse>(
                    make_error_code(ErrorCode::InternalError), "unknown error");
            }
        });
}

} // namespace filexferlib
