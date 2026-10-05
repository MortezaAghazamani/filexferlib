#include "filexferlib/server.hpp"
#include "filexferlib/detail/protocol.hpp"
#include "filexferlib/detail/hash.hpp"
#include "filexferlib/detail/compression.hpp"
#include "filexferlib/detail/transfer_registry.hpp"
#include "filexferlib/detail/transfer_util.hpp"
#include "filexferlib/error.hpp"
#include "filexferlib/logger.hpp"

#include <cstring>
#include <algorithm>
#include <fstream>
#include <chrono>
#include <thread>
#include <vector>
#include <filesystem>

namespace filexferlib {

namespace fs = std::filesystem;

// =====================================================================
// GenericContext
// =====================================================================

void GenericContext::respond(ByteBuffer payload, std::uint16_t statusCode) {
    if (responded_) return;
    responded_ = true;

    using namespace detail;
    ByteBuffer buf;
    buf.resize(6 + payload.size());
    put_u32(buf.data(), requestId_);
    put_u16(buf.data() + 4, statusCode);
    if (!payload.empty())
        std::memcpy(buf.data() + 6, payload.data(), payload.size());

    session_->send(MsgType::GenericResp, std::move(buf));
}

// =====================================================================
// Server lifecycle
// =====================================================================

Server::Server(ServerConfig cfg)
    : cfg_(std::move(cfg))
    , work_(asio::make_work_guard(io_))
    , acceptor_(io_)
{
    if (cfg_.maxBytesPerSecond > 0) {
        rateLimiter_ = std::make_shared<detail::RateLimiter>(
            io_, cfg_.maxBytesPerSecond);

        std::uint64_t chunkBytes = 256ull * 1024;
        std::uint64_t ms = (chunkBytes * 1000ull) / cfg_.maxBytesPerSecond;
        std::uint64_t recTimeout = ms * 3;
        if (recTimeout < 10000) recTimeout = 10000;
        if (recTimeout > 3600000) recTimeout = 3600000;

        std::uint64_t target = cfg_.maxBytesPerSecond / 5;
        if (target < 1024) target = 1024;
        if (target > 256 * 1024) target = 256 * 1024;

        FX_INFO("server bandwidth limit: ", cfg_.maxBytesPerSecond, " B/s");
        FX_INFO("  recommended chunkSize: ", target, " bytes");
        FX_INFO("  recommended timeout: ", recTimeout, " ms");
    }
}

Server::~Server() { stop(); }

std::uint16_t Server::start(const std::string& bindAddress,
                            std::uint16_t port) {
    using asio::ip::tcp;
    tcp::endpoint ep(asio::ip::make_address(bindAddress), port);
    acceptor_.open(ep.protocol());
    acceptor_.set_option(tcp::acceptor::reuse_address(true));
    acceptor_.bind(ep);
    acceptor_.listen();
    const std::uint16_t actualPort = acceptor_.local_endpoint().port();

    cleanup_stale_temp_files();

    running_.store(true);
    ioThread_ = std::thread([this]{
        try {
            io_.run();
        } catch (const std::exception& e) {
            FX_ERROR("server io thread exception: ", e.what());
        } catch (...) {
            FX_ERROR("server io thread unknown exception");
        }
    });
    do_accept();
    return actualPort;
}

void Server::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) return;

    asio::post(io_, [this]{
        std::error_code ec;
        acceptor_.close(ec);
        if (rateLimiter_) rateLimiter_->cancel_all();
        {
            std::lock_guard<std::mutex> lk(sessionsMu_);
            for (auto& s : sessions_) s->close();
            sessions_.clear();
        }
        {
            std::lock_guard<std::mutex> lk(incomingMu_);
            incoming_.clear();
        }
        {
            std::lock_guard<std::mutex> lk(outgoingMu_);
            outgoing_.clear();
        }
        {
            std::lock_guard<std::mutex> lk(transferRegsMu_);
            transferRegs_.clear();
        }
        {
            std::lock_guard<std::mutex> lk(pendingResumeMu_);
            pendingDownloadResumeOffset_.clear();
        }
        work_.reset();
    });

    if (ioThread_.joinable()) {
        if (std::this_thread::get_id() == ioThread_.get_id()) {
            ioThread_.detach();
        } else {
            ioThread_.join();
        }
    }
}

void Server::cleanup_stale_temp_files() {
    std::error_code ec;
    if (!fs::exists(cfg_.rootDir, ec)) return;

    const auto now = std::chrono::system_clock::now();
    const auto maxAge = std::chrono::hours(7 * 24);

    int removed = 0;
    for (auto it = fs::recursive_directory_iterator(
             cfg_.rootDir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const auto& entry = *it;
        if (!entry.is_regular_file(ec)) continue;
        const auto name = entry.path().filename().string();

        bool isLeftover = name.size() >= 8 &&
                          name.compare(name.size() - 8, 8, "leftover") == 0;
        bool isTmp = name.find(".fxfer.tmp") != std::string::npos;

        if (isLeftover) {
            std::error_code rmEc;
            fs::remove(entry.path(), rmEc);
            if (!rmEc) ++removed;
            continue;
        }

        if (isTmp) {
            auto t = fs::last_write_time(entry.path(), ec);
            if (ec) continue;
            auto sysT = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                t - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
            if (now - sysT > maxAge) {
                std::error_code rmEc;
                fs::remove(entry.path(), rmEc);
                if (!rmEc) ++removed;
            }
        }
    }
    if (removed > 0) {
        FX_INFO("cleanup: removed ", removed, " stale temp file(s)");
    }
}

// =====================================================================
// Accept loop
// =====================================================================

void Server::do_accept() {
    acceptor_.async_accept(
        [this](std::error_code ec, asio::ip::tcp::socket socket) {
            if (ec) {
                if (ec != asio::error::operation_aborted && running_.load())
                    do_accept();
                return;
            }

            auto sess = std::make_shared<detail::Session>(io_, cfg_.limits);
            {
                std::lock_guard<std::mutex> lk(sessionsMu_);
                sessions_.push_back(sess);
            }
            {
                std::lock_guard<std::mutex> lk(transferRegsMu_);
                transferRegs_[sess.get()] =
                    std::make_shared<detail::TransferRegistry>();
            }

            sess->set_close_handler([this, sess](std::error_code) {
                {
                    std::lock_guard<std::mutex> lk(sessionsMu_);
                    sessions_.erase(
                        std::remove(sessions_.begin(), sessions_.end(), sess),
                        sessions_.end());
                }
                {
                    std::lock_guard<std::mutex> lk(transferRegsMu_);
                    transferRegs_.erase(sess.get());
                }
                {
                    std::lock_guard<std::mutex> lk(pendingResumeMu_);
                    pendingDownloadResumeOffset_.erase(sess.get());
                }
                {
                    std::lock_guard<std::mutex> lk(incomingMu_);
                    for (auto it = incoming_.begin(); it != incoming_.end(); ) {
                        if (it->first.sess == sess.get())
                            it = incoming_.erase(it);
                        else
                            ++it;
                    }
                }
            });

            sess->set_message_handler(
                [this, sess](detail::Frame f) {
                    using namespace detail;

                    if (f.type == MsgType::Hello) {
                        if (f.payload.size() < 3) {
                            sess->close(make_error_code(ErrorCode::ProtocolError));
                            return;
                        }
                        const u8 ver = std::to_integer<u8>(f.payload[0]);
                        if (ver != kProtocolVersion) {
                            sess->close(make_error_code(ErrorCode::VersionMismatch));
                            return;
                        }
                        const u16 tlen = get_u16(f.payload.data() + 1);
                        if (f.payload.size() < 3u + tlen) {
                            sess->close(make_error_code(ErrorCode::ProtocolError));
                            return;
                        }
                        std::string token;
                        if (tlen > 0) {
                            token.assign(
                                reinterpret_cast<const char*>(f.payload.data() + 3),
                                tlen);
                        }

                        if (!cfg_.requiredAuthToken.empty() &&
                            token != cfg_.requiredAuthToken) {
                            FX_WARN("auth failed: bad token");
                            ByteBuffer err;
                            const std::string msg = "auth failed";
                            err.resize(msg.size());
                            std::memcpy(err.data(), msg.data(), msg.size());
                            sess->send(MsgType::Error, std::move(err));
                            sess->close();
                            return;
                        }

                        // Compute recommended timeout and chunkSize
                        u32 recommendedTimeoutMs = 30000;
                        u32 recommendedChunkSize = 0;

                        if (cfg_.maxBytesPerSecond > 0) {
                            u64 chunkBytes = 256ull * 1024;
                            u64 ms = (chunkBytes * 1000ull) / cfg_.maxBytesPerSecond;
                            u64 recommended = ms * 3;
                            if (recommended < 10000) recommended = 10000;
                            if (recommended > 3600000) recommended = 3600000;
                            recommendedTimeoutMs = static_cast<u32>(recommended);

                            u64 target = cfg_.maxBytesPerSecond / 5;
                            if (target < 1024) target = 1024;
                            if (target > 256 * 1024) target = 256 * 1024;
                            recommendedChunkSize = static_cast<u32>(target);
                        }

                        ByteBuffer ack = encode_hello_ack(
                            "filexferlib/1.0",
                            cfg_.maxBytesPerSecond,
                            recommendedTimeoutMs,
                            recommendedChunkSize);
                        sess->send(MsgType::HelloAck, std::move(ack));
                        FX_INFO("client connected (auth=",
                                cfg_.requiredAuthToken.empty() ? "off" : "on",
                                ")");
                        return;
                    }

                    if (f.type == MsgType::GenericReq) {
                        handle_generic(sess, std::move(f));
                        return;
                    }
                    if (f.type == MsgType::ListReq) {
                        handle_list(sess, std::move(f));
                        return;
                    }
                    if (f.type == MsgType::DeleteReq) {
                        handle_delete(sess, std::move(f));
                        return;
                    }
                    if (f.type == MsgType::RenameReq) {
                        handle_rename(sess, std::move(f));
                        return;
                    }
                    if (f.type == MsgType::SetMtimeReq) {
                        handle_set_mtime(sess, std::move(f));
                        return;
                    }
                    if (f.type == MsgType::MkdirReq) {
                        handle_mkdir(sess, std::move(f));
                        return;
                    }
                    if (f.type == MsgType::RemoteCopyReq) {
                        handle_remote_copy(sess, std::move(f));
                        return;
                    }
                    if (f.type == MsgType::FileResumeReq) {
                        handle_file_resume_req(sess, std::move(f));
                        return;
                    }
                    if (f.type == MsgType::DownloadResumeReq) {
                        handle_download_resume_req(sess, std::move(f));
                        return;
                    }

                    if (f.type == MsgType::FileBegin ||
                        f.type == MsgType::FileChunk ||
                        f.type == MsgType::FileEnd ||
                        f.type == MsgType::FileAck)
                    {
                        if (f.payload.size() < 4) {
                            sess->close(make_error_code(ErrorCode::ProtocolError));
                            return;
                        }
                        const u32 tid = get_u32(f.payload.data());

                        if (f.type == MsgType::FileBegin) {
                            FileBeginData bd;
                            if (decode_file_begin(f.payload, bd) && bd.is_pull) {
                                handle_download_request(sess, std::move(f), tid);
                                return;
                            }
                            handle_file_begin(sess, std::move(f));
                            return;
                        }
                        if (f.type == MsgType::FileChunk) {
                            handle_file_chunk(sess, std::move(f));
                            return;
                        }
                        if (f.type == MsgType::FileEnd) {
                            handle_file_end(sess, std::move(f));
                            return;
                        }
                        if (f.type == MsgType::FileAck) {
                            handle_upload_ack(sess, std::move(f), tid);
                            return;
                        }
                    }

                    FX_DEBUG("unknown message type=0x",
                             static_cast<int>(f.type));
                });

            sess->start(std::move(socket));
            do_accept();
        });
}

// =====================================================================
// Generic dispatch
// =====================================================================

void Server::handle_generic(std::shared_ptr<detail::Session> sess,
                            detail::Frame f) {
    using namespace detail;

    if (f.payload.size() < 6) {
        sess->close(make_error_code(ErrorCode::ProtocolError));
        return;
    }
    const u32 rid = get_u32(f.payload.data());
    const u16 mlen = get_u16(f.payload.data() + 4);

    if (f.payload.size() < 6u + mlen ||
        mlen > cfg_.limits.maxMethodNameLength) {
        ByteBuffer buf(6);
        put_u32(buf.data(), rid);
        put_u16(buf.data() + 4, 0xFFFD);
        sess->send(MsgType::GenericResp, std::move(buf));
        return;
    }

    std::string method(reinterpret_cast<const char*>(f.payload.data() + 6),
                       mlen);
    ByteBuffer body(f.payload.begin() + 6 + mlen, f.payload.end());

    GenericHandler h;
    {
        std::lock_guard<std::mutex> lk(handlersMu_);
        auto it = handlers_.find(method);
        if (it != handlers_.end()) h = it->second;
    }

    if (!h) {
        ByteBuffer buf(6);
        put_u32(buf.data(), rid);
        put_u16(buf.data() + 4, 1);
        sess->send(MsgType::GenericResp, std::move(buf));
        return;
    }

    GenericContext ctx(sess, rid, std::move(body));
    try { h(ctx); }
    catch (...) {
        ByteBuffer buf(6);
        put_u32(buf.data(), rid);
        put_u16(buf.data() + 4, 3);
        sess->send(MsgType::GenericResp, std::move(buf));
    }
}

// =====================================================================
// Path resolution
// =====================================================================

fs::path Server::resolvePath(const std::string& remote) const {
    fs::path p(remote);
    fs::path root = fs::weakly_canonical(cfg_.rootDir);

    bool truly_absolute = p.has_root_name() && p.has_root_directory();

    fs::path full;
    if (truly_absolute) {
        full = fs::weakly_canonical(p);
    } else {
        std::string rel = remote;
        while (!rel.empty() && (rel[0] == '/' || rel[0] == '\\'))
            rel.erase(rel.begin());
        full = fs::weakly_canonical(root / fs::path(rel));
    }

    auto rootStr = root.generic_string();
    auto fullStr = full.generic_string();
    bool inside = false;
    if (fullStr.size() >= rootStr.size() &&
        fullStr.compare(0, rootStr.size(), rootStr) == 0)
    {
        if (fullStr.size() == rootStr.size() ||
            fullStr[rootStr.size()] == '/' ||
            fullStr[rootStr.size()] == '\\')
        {
            inside = true;
        }
    }
    if (!inside && !cfg_.allowAbsoluteOutsideRoot) {
        throw std::system_error(make_error_code(ErrorCode::PathOutsideRoot));
    }
    return full;
}

// =====================================================================
// Transfer registry
// =====================================================================

detail::TransferRegistry::Ptr
Server::get_registry(std::shared_ptr<detail::Session> sess) {
    std::lock_guard<std::mutex> lk(transferRegsMu_);
    auto it = transferRegs_.find(sess.get());
    if (it != transferRegs_.end()) return it->second;
    auto reg = std::make_shared<detail::TransferRegistry>();
    transferRegs_[sess.get()] = reg;
    return reg;
}

// =====================================================================
// Upload
// =====================================================================

void Server::handle_file_begin(std::shared_ptr<detail::Session> sess,
                               detail::Frame f) {
    using namespace detail;

    FileBeginData bd;
    if (!decode_file_begin(f.payload, bd)) {
        sess->close(make_error_code(ErrorCode::ProtocolError));
        return;
    }
    const u32 tid = get_u32(f.payload.data());

    fs::path finalPath;
    try { finalPath = resolvePath(bd.path); }
    catch (const std::system_error& e) {
        sess->send(MsgType::FileAck,
                   encode_file_ack(tid, false, 0, e.what()));
        return;
    }

    std::error_code ec;
    fs::create_directories(finalPath.parent_path(), ec);

    auto inc = std::make_shared<IncomingFile>();
    inc->owner = sess.get();
    inc->finalPath = finalPath;
    inc->tmpPath = finalPath;
    inc->tmpPath += ".fxfer.tmp";
    inc->expectedSize = bd.size;
    inc->mtimeNs = bd.mtimeNs;
    inc->expectedHash = bd.xxhash;
    inc->compressed = bd.compressed;   // ← flag از FileBegin

    u64 resumeOffset = 0;
    bool isResume = false;
    if (fs::exists(inc->tmpPath, ec) && fs::is_regular_file(inc->tmpPath, ec)) {
        auto tmpSz = fs::file_size(inc->tmpPath, ec);
        if (!ec && tmpSz > 0 && tmpSz < bd.size) {
            resumeOffset = tmpSz;
            isResume = true;
            FX_INFO("resuming upload at offset ", resumeOffset,
                    " (of ", bd.size, ")");
        } else if (!ec && tmpSz >= bd.size) {
            std::error_code rmEc;
            fs::remove(inc->tmpPath, rmEc);
            resumeOffset = 0;
            isResume = false;
        }
    }

    if (isResume) {
        inc->out.open(inc->tmpPath,
                      std::ios::binary | std::ios::in | std::ios::out);
        if (!inc->out) {
            FX_WARN("resume open failed, restarting from scratch");
            isResume = false;
            resumeOffset = 0;
            inc->out.open(inc->tmpPath,
                          std::ios::binary | std::ios::trunc);
        }
    } else {
        inc->out.open(inc->tmpPath,
                      std::ios::binary | std::ios::trunc);
    }

    if (!inc->out) {
        sess->send(MsgType::FileAck,
                   encode_file_ack(tid, false, 0, "cannot open tmp"));
        return;
    }

    inc->receivedBytes = resumeOffset;

    {
        std::lock_guard<std::mutex> lk(incomingMu_);
        incoming_[{sess.get(), tid}] = inc;
    }

    sess->send(MsgType::FileAck,
               encode_file_ack(tid, true, resumeOffset));
}

void Server::handle_file_chunk(std::shared_ptr<detail::Session> sess,
                               detail::Frame f) {
    using namespace detail;

    if (f.payload.size() < 4) {
        sess->close(make_error_code(ErrorCode::ProtocolError));
        return;
    }
    const u32 tid = get_u32(f.payload.data());

    std::shared_ptr<IncomingFile> inc;
    {
        std::lock_guard<std::mutex> lk(incomingMu_);
        auto it = incoming_.find({sess.get(), tid});
        if (it == incoming_.end()) {
            sess->close(make_error_code(ErrorCode::ProtocolError));
            return;
        }
        inc = it->second;
    }

    FileChunkData cd;
    if (!decode_file_chunk(f.payload, cd)) {
        sess->close(make_error_code(ErrorCode::ProtocolError));
        return;
    }

    // ---- Decompress if the transfer is compressed ----
    std::vector<char> decompressedBuf;
    const detail::byte* dataToWrite = cd.data;
    std::size_t lenToWrite = cd.len;

    if (inc->compressed) {
        if (cd.len < 4) {
            sess->send(MsgType::FileAck,
                encode_file_ack(tid, false, 0, "compressed chunk too short"));
            return;
        }
        const u32 origSize = get_u32(cd.data);
        auto decompressed = zstd_decompress(
            cd.data + 4, cd.len - 4, origSize);
        if (decompressed.empty() || decompressed.size() != origSize) {
            sess->send(MsgType::FileAck,
                encode_file_ack(tid, false, 0, "decompress failed"));
            return;
        }
        decompressedBuf.assign(
            reinterpret_cast<const char*>(decompressed.data()),
            reinterpret_cast<const char*>(decompressed.data())
                + decompressed.size());
        dataToWrite = reinterpret_cast<const detail::byte*>(
            decompressedBuf.data());
        lenToWrite = decompressedBuf.size();
    }

    auto write_chunk = [&](u64 offset, const byte* data, std::size_t len) {
        inc->out.seekp(static_cast<std::streamoff>(offset));
        inc->out.write(reinterpret_cast<const char*>(data),
                       static_cast<std::streamsize>(len));
    };

    if (cd.offset == inc->receivedBytes) {
        write_chunk(cd.offset, dataToWrite, lenToWrite);
        inc->receivedBytes += lenToWrite;

        // Flush any pending in-order chunks
        auto it = inc->pendingChunks.begin();
        while (it != inc->pendingChunks.end() &&
               it->first == inc->receivedBytes)
        {
            FileChunkData pcd;
            if (decode_file_chunk(it->second, pcd)) {
                // Pending chunks are already decoded from f.payload.
                // They need the same decompression treatment.
                std::vector<char> pDecomp;
                const detail::byte* pData = pcd.data;
                std::size_t pLen = pcd.len;

                if (inc->compressed) {
                    if (pcd.len < 4) {
                        it = inc->pendingChunks.erase(it);
                        continue;
                    }
                    const u32 pOrig = get_u32(pcd.data);
                    auto pd = zstd_decompress(
                        pcd.data + 4, pcd.len - 4, pOrig);
                    if (pd.empty() || pd.size() != pOrig) {
                        it = inc->pendingChunks.erase(it);
                        continue;
                    }
                    pDecomp.assign(
                        reinterpret_cast<const char*>(pd.data()),
                        reinterpret_cast<const char*>(pd.data()) + pd.size());
                    pData = reinterpret_cast<const detail::byte*>(
                        pDecomp.data());
                    pLen = pDecomp.size();
                }

                write_chunk(pcd.offset, pData, pLen);
                inc->receivedBytes += pLen;
            }
            it = inc->pendingChunks.erase(it);
        }
    } else if (cd.offset > inc->receivedBytes) {
        if (inc->pendingChunks.size() >= 16) {
            sess->send(MsgType::FileAck,
                encode_file_ack(tid, false, 0, "too many out-of-order"));
            return;
        }
        ByteBuffer copy = f.payload;
        inc->pendingChunks[cd.offset] = std::move(copy);
    }

    // ---- Apply rate limit to ACK (upload throttle) ----
    if (rateLimiter_ && rateLimiter_->isEnabled()) {
        auto weak = std::weak_ptr<detail::Session>(sess);
        const u64 bytes = static_cast<u64>(cd.len);
        rateLimiter_->acquire(bytes, [weak, tid]{
            auto s = weak.lock();
            if (!s) return;
            s->send(MsgType::FileAck, encode_file_ack(tid, true, 0));
        });
    } else {
        sess->send(MsgType::FileAck, encode_file_ack(tid, true, 0));
    }
}

void Server::handle_file_end(std::shared_ptr<detail::Session> sess,
                             detail::Frame f) {
    using namespace detail;

    if (f.payload.size() < 12) {
        sess->close(make_error_code(ErrorCode::ProtocolError));
        return;
    }
    const u32 tid = get_u32(f.payload.data());

    std::shared_ptr<IncomingFile> inc;
    {
        std::lock_guard<std::mutex> lk(incomingMu_);
        auto it = incoming_.find({sess.get(), tid});
        if (it == incoming_.end()) {
            sess->close(make_error_code(ErrorCode::ProtocolError));
            return;
        }
        inc = it->second;
        incoming_.erase(it);
    }

    u64 srcHash = 0;
    if (!decode_file_end(f.payload, srcHash)) {
        sess->close(make_error_code(ErrorCode::ProtocolError));
        return;
    }

    const fs::path tmpPath   = inc->tmpPath;
    const fs::path finalPath = inc->finalPath;
    const i64      mtimeNs   = inc->mtimeNs;
    const u64      expected  = inc->expectedSize;

    inc->out.flush();
    inc->out.close();
    inc->out = std::ofstream{};
    inc.reset();

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Finalize: rename or copy
    bool finalized = false;
    {
        std::error_code rmEc;
        fs::remove(finalPath, rmEc);
        std::error_code rnEc;
        fs::rename(tmpPath, finalPath, rnEc);
        if (!rnEc) finalized = true;
    }
    if (!finalized) {
        for (int i = 0; i < 5 && !finalized; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::error_code rmEc;
            fs::remove(finalPath, rmEc);
            std::error_code rnEc;
            fs::rename(tmpPath, finalPath, rnEc);
            if (!rnEc) finalized = true;
        }
    }
    if (!finalized) {
        FX_WARN("rename failed; falling back to copy");
        std::error_code rmEc;
        fs::remove(finalPath, rmEc);
        std::error_code cpEc;
        fs::copy_file(tmpPath, finalPath,
                      fs::copy_options::overwrite_existing, cpEc);
        if (!cpEc) finalized = true;
        else FX_ERROR("copy_file also failed: ", cpEc.message());
    }
    if (!finalized) {
        sess->send(MsgType::FileAck,
                   encode_file_ack(tid, false, 0, "finalize failed"));
        return;
    }

    // Hash the final file (uncompressed on disk)
    std::error_code hEc;
    u64 dstHash = xxhash_file(finalPath, hEc);
    if (hEc) dstHash = 0;

    const bool ok = (dstHash == srcHash) || (srcHash == 0);

    if (!ok) {
        std::error_code rmEc;
        fs::remove(finalPath, rmEc);
        sess->send(MsgType::FileAck, encode_file_ack(tid, false, dstHash,
                                                     "hash mismatch"));
        return;
    }

    if (mtimeNs != 0) {
        std::error_code timeEc;
        using file_dur = std::filesystem::file_time_type::duration;
        auto t = std::filesystem::file_time_type{
            std::chrono::duration_cast<file_dur>(
                std::chrono::nanoseconds{mtimeNs})};
        fs::last_write_time(finalPath, t, timeEc);
    }

    if (fs::exists(tmpPath)) {
        for (int i = 0; i < 5; ++i) {
            std::error_code rmEc;
            fs::remove(tmpPath, rmEc);
            if (!rmEc || !fs::exists(tmpPath)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
    }

    sess->send(MsgType::FileAck, encode_file_ack(tid, ok, dstHash));
}

void Server::handle_file_resume_req(std::shared_ptr<detail::Session> sess,
                                    detail::Frame f) {
    using namespace detail;

    if (f.payload.size() < 4) {
        sess->close(make_error_code(ErrorCode::ProtocolError));
        return;
    }
    const u32 tid = get_u32(f.payload.data());

    std::string path;
    if (!decode_resume_req(f.payload, path)) {
        sess->send(MsgType::FileResumeResp,
                   encode_resume_resp(tid, 0, false));
        return;
    }

    fs::path finalPath;
    try { finalPath = resolvePath(path); }
    catch (...) {
        sess->send(MsgType::FileResumeResp,
                   encode_resume_resp(tid, 0, false));
        return;
    }

    fs::path tmpPath = finalPath;
    tmpPath += ".fxfer.tmp";

    std::error_code ec;
    if (fs::exists(tmpPath, ec) && fs::is_regular_file(tmpPath, ec)) {
        auto sz = fs::file_size(tmpPath, ec);
        if (!ec) {
            sess->send(MsgType::FileResumeResp,
                       encode_resume_resp(tid, sz, true));
            return;
        }
    }

    sess->send(MsgType::FileResumeResp,
               encode_resume_resp(tid, 0, false));
}

// =====================================================================
// Download
// =====================================================================

void Server::handle_download_request(std::shared_ptr<detail::Session> sess,
                                     detail::Frame f, std::uint32_t tid) {
    using namespace detail;

    FileBeginData bd;
    if (!decode_file_begin(f.payload, bd)) {
        sess->send(MsgType::FileAck,
                   encode_file_ack(tid, false, 0, "bad begin"));
        return;
    }

    auto of = std::make_shared<OutgoingFile>();
    of->sess = sess;
    of->transferId = tid;
    of->compressed = bd.compressed;   // ← flag از pull request

    try { of->path = resolvePath(bd.path); }
    catch (const std::system_error& e) {
        sess->send(MsgType::FileAck,
                   encode_file_ack(tid, false, 0, e.what()));
        return;
    }

    std::error_code ec;
    of->size = fs::file_size(of->path, ec);
    if (ec) {
        sess->send(MsgType::FileAck,
                   encode_file_ack(tid, false, 0, "file not found"));
        return;
    }

    u64 startOffset = 0;
    {
        std::lock_guard<std::mutex> lk(pendingResumeMu_);
        auto it = pendingDownloadResumeOffset_.find(sess.get());
        if (it != pendingDownloadResumeOffset_.end()) {
            startOffset = it->second;
            pendingDownloadResumeOffset_.erase(it);
        }
    }
    if (startOffset > of->size) startOffset = 0;
    of->offset = startOffset;
    of->ackedOffset = startOffset;

    of->windowSize = cfg_.downloadWindowSize;
    if (of->windowSize == 0) of->windowSize = 1;

    // Adapt chunkSize to the rate limit
    if (cfg_.maxBytesPerSecond > 0) {
        u64 target = cfg_.maxBytesPerSecond / 5;
        if (target < 1024) target = 1024;
        if (target > 256 * 1024) target = 256 * 1024;
        of->chunkSize = static_cast<std::size_t>(target);
    }

    auto t = fs::last_write_time(of->path, ec);
    if (!ec) {
        of->mtimeNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            t.time_since_epoch()).count();
    }

    std::error_code hEc;
    of->hash = xxhash_file(of->path, hEc);
    if (hEc) of->hash = 0;

    of->file.open(of->path, std::ios::binary);
    if (!of->file) {
        sess->send(MsgType::FileAck,
                   encode_file_ack(tid, false, 0, "cannot open"));
        return;
    }
    if (startOffset > 0) {
        of->file.seekg(static_cast<std::streamoff>(startOffset));
    }
    of->buf.resize(of->chunkSize);

    {
        std::lock_guard<std::mutex> lk(outgoingMu_);
        outgoing_[tid] = of;
    }

    auto weak = std::weak_ptr<OutgoingFile>(of);
    get_registry(sess)->register_handler(
        tid,
        [this, weak](Frame fr) {
            auto s = weak.lock();
            if (!s) return;
            if (fr.type != MsgType::FileAck) return;

            FileAckData ack;
            if (!decode_file_ack(fr.payload, ack)) {
                std::lock_guard<std::mutex> lk(outgoingMu_);
                outgoing_.erase(s->transferId);
                return;
            }
            if (!ack.ok) {
                std::lock_guard<std::mutex> lk(outgoingMu_);
                outgoing_.erase(s->transferId);
                if (s->sess)
                    s->sess->transfers()->unregister_handler(s->transferId);
                return;
            }
            if (s->inflight > 0) s->inflight--;
            fill_window(s);
        });

    sess->send(MsgType::FileBegin,
        encode_file_begin(tid, bd.path, of->size, of->mtimeNs, of->hash,
                          of->compressed));
}

void Server::handle_download_resume_req(std::shared_ptr<detail::Session> sess,
                                        detail::Frame f) {
    using namespace detail;

    if (f.payload.size() < 4) {
        sess->close(make_error_code(ErrorCode::ProtocolError));
        return;
    }
    const u32 tid = get_u32(f.payload.data());

    DownloadResumeReqData rd;
    if (!decode_download_resume_req(f.payload, rd)) {
        sess->send(MsgType::DownloadResumeResp,
                   encode_download_resume_resp(tid, 0, 0, 0, 0, false, false));
        return;
    }

    fs::path finalPath;
    try { finalPath = resolvePath(rd.path); }
    catch (...) {
        sess->send(MsgType::DownloadResumeResp,
                   encode_download_resume_resp(tid, 0, 0, 0, 0, false, false));
        return;
    }

    std::error_code ec;
    if (!fs::exists(finalPath, ec) || !fs::is_regular_file(finalPath, ec)) {
        sess->send(MsgType::DownloadResumeResp,
                   encode_download_resume_resp(tid, 0, 0, 0, 0, false, false));
        return;
    }

    u64 totalSize = fs::file_size(finalPath, ec);
    if (ec) {
        sess->send(MsgType::DownloadResumeResp,
                   encode_download_resume_resp(tid, 0, 0, 0, 0, false, false));
        return;
    }

    i64 mtimeNs = 0;
    auto t = fs::last_write_time(finalPath, ec);
    if (!ec) {
        mtimeNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            t.time_since_epoch()).count();
    }

    std::error_code hEc;
    u64 hash = xxhash_file(finalPath, hEc);
    if (hEc) hash = 0;

    u64 serverOffset = rd.haveOffset;
    if (serverOffset > totalSize) serverOffset = 0;

    {
        std::lock_guard<std::mutex> lk(pendingResumeMu_);
        pendingDownloadResumeOffset_[sess.get()] = serverOffset;
    }

    // Note: `compressed` flag will be set from the FileBegin pull
    // request, not from this resume response. We send false here to
    // keep backward compatibility, and the FileBegin handler will
    // override the flag based on the actual pull request.
    sess->send(MsgType::DownloadResumeResp,
               encode_download_resume_resp(tid, serverOffset,
                                           totalSize, hash, mtimeNs, true,
                                           false));
}

// =====================================================================
// fill_window — with compression and rate limiter
// =====================================================================

void Server::fill_window(std::shared_ptr<OutgoingFile> of) {
    using namespace detail;
    if (!of || of->finished) return;

    // If the transfer is complete, send END.
    if (of->offset >= of->size && of->inflight == 0) {
        of->sess->send(MsgType::FileEnd,
                       encode_file_end(of->transferId, of->hash));
        of->finished = true;
        {
            std::lock_guard<std::mutex> lk(outgoingMu_);
            outgoing_.erase(of->transferId);
        }
        return;
    }

    // Without rate limiting, fill synchronously.
    if (!rateLimiter_ || !rateLimiter_->isEnabled()) {
        while (of->inflight < of->windowSize && of->offset < of->size) {
            of->file.read(of->buf.data(),
                          static_cast<std::streamsize>(of->buf.size()));
            const auto n = of->file.gcount();
            if (n <= 0) break;

            const auto offset = of->offset;
            const auto bytes  = static_cast<std::size_t>(n);

            if (of->compressed) {
                auto compressed = zstd_compress(
                    of->buf.data(), bytes, of->compressionLevel);
                if (!compressed.empty()) {
                    ByteBuffer payload(4 + compressed.size());
                    put_u32(payload.data(), static_cast<u32>(bytes));
                    std::memcpy(payload.data() + 4,
                                compressed.data(), compressed.size());

                    ByteBuffer frame(4 + 8 + payload.size());
                    put_u32(frame.data(), of->transferId);
                    put_u64(frame.data() + 4, offset);
                    std::memcpy(frame.data() + 12,
                                payload.data(), payload.size());
                    of->sess->send(MsgType::FileChunk, std::move(frame));
                } else {
                    of->sess->send(MsgType::FileChunk,
                        encode_file_chunk(of->transferId, offset,
                                          of->buf.data(), bytes));
                }
            } else {
                of->sess->send(MsgType::FileChunk,
                    encode_file_chunk(of->transferId, offset,
                                      of->buf.data(), bytes));
            }
            of->offset += static_cast<u64>(bytes);
            of->inflight++;
        }
        if (of->offset >= of->size && of->inflight == 0) {
            of->sess->send(MsgType::FileEnd,
                           encode_file_end(of->transferId, of->hash));
            of->finished = true;
            {
                std::lock_guard<std::mutex> lk(outgoingMu_);
                outgoing_.erase(of->transferId);
            }
        }
        return;
    }

    // ---- With rate limiting ----
    if (of->waitingForToken) return;
    if (of->inflight >= of->windowSize) return;
    if (of->offset >= of->size) return;

    of->file.read(of->buf.data(),
                  static_cast<std::streamsize>(of->buf.size()));
    const auto n = of->file.gcount();
    if (n <= 0) {
        if (of->inflight == 0) {
            of->sess->send(MsgType::FileEnd,
                           encode_file_end(of->transferId, of->hash));
            of->finished = true;
            {
                std::lock_guard<std::mutex> lk(outgoingMu_);
                outgoing_.erase(of->transferId);
            }
        }
        return;
    }

    const auto offset = of->offset;
    const auto bytes  = static_cast<std::size_t>(n);
    of->waitingForToken = true;

    auto weak = std::weak_ptr<OutgoingFile>(of);
    rateLimiter_->acquire(bytes, [this, weak, offset, bytes]{
        auto s = weak.lock();
        if (!s || s->finished) return;

        s->waitingForToken = false;

        if (s->compressed) {
            auto compressed = zstd_compress(
                s->buf.data(), bytes, s->compressionLevel);
            if (!compressed.empty()) {
                ByteBuffer payload(4 + compressed.size());
                put_u32(payload.data(), static_cast<u32>(bytes));
                std::memcpy(payload.data() + 4,
                            compressed.data(), compressed.size());

                ByteBuffer frame(4 + 8 + payload.size());
                put_u32(frame.data(), s->transferId);
                put_u64(frame.data() + 4, offset);
                std::memcpy(frame.data() + 12,
                            payload.data(), payload.size());
                s->sess->send(MsgType::FileChunk, std::move(frame));
            } else {
                s->sess->send(MsgType::FileChunk,
                    encode_file_chunk(s->transferId, offset,
                                      s->buf.data(), bytes));
            }
        } else {
            s->sess->send(MsgType::FileChunk,
                encode_file_chunk(s->transferId, offset,
                                  s->buf.data(), bytes));
        }
        s->offset += static_cast<u64>(bytes);
        s->inflight++;

        fill_window(s);
    });
}

void Server::send_next_chunk(std::shared_ptr<OutgoingFile> of) {
    fill_window(of);
}

void Server::handle_upload_ack(std::shared_ptr<detail::Session> sess,
                               detail::Frame f, std::uint32_t tid) {
    std::shared_ptr<OutgoingFile> of;
    {
        std::lock_guard<std::mutex> lk(outgoingMu_);
        auto it = outgoing_.find(tid);
        if (it != outgoing_.end()) of = it->second;
    }
    if (!of) return;

    get_registry(sess)->dispatch(std::move(f), tid);
}

// =====================================================================
// List
// =====================================================================

void Server::handle_list(std::shared_ptr<detail::Session> sess,
                         detail::Frame f) {
    using namespace detail;
    std::size_t used = 0;
    std::string remote = get_str(f.payload.data(), f.payload.size(), used);

    std::string root;
    try { root = resolvePath(remote).generic_string(); }
    catch (const std::system_error& e) {
        ByteBuffer resp;
        resp.push_back(std::byte(1));
        put_str(resp, e.what());
        sess->send(MsgType::ListResp, std::move(resp));
        return;
    }

    ByteBuffer resp;
    std::error_code existsEc;
    if (!fs::exists(root, existsEc)) {
        sess->send(MsgType::ListResp, std::move(resp));
        return;
    }

    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const auto& entry = *it;
        std::error_code relEc;
        const auto rel = fs::relative(entry.path(), root, relEc);
        if (relEc) continue;
        std::string relPosix = rel.generic_string();
        if (relPosix.empty()) continue;
        if (relPosix[0] != '/') relPosix = "/" + relPosix;

        FileMeta m;
        m.relativePath = relPosix;
        std::error_code dEc;
        m.isDirectory = entry.is_directory(dEc);
        if (!m.isDirectory) {
            std::error_code sEc;
            m.size = fs::file_size(entry.path(), sEc);
            if (sEc) continue;
            std::error_code tEc;
            auto t = fs::last_write_time(entry.path(), tEc);
            if (!tEc) m.mtimeNs = std::chrono::duration_cast<
                std::chrono::nanoseconds>(t.time_since_epoch()).count();
        }
        put_str(resp, m.relativePath);
        const auto base = resp.size();
        resp.resize(base + 25);
        put_u64(resp.data() + base, m.size);
        put_i64(resp.data() + base + 8, m.mtimeNs);
        put_u64(resp.data() + base + 16, m.xxhash);
        resp[base + 24] = std::byte(m.isDirectory ? 1 : 0);
    }
    sess->send(MsgType::ListResp, std::move(resp));
}

// =====================================================================
// Delete
// =====================================================================

void Server::handle_delete(std::shared_ptr<detail::Session> sess,
                           detail::Frame f) {
    using namespace detail;
    std::size_t used = 0;
    std::string remote = get_str(f.payload.data(), f.payload.size(), used);
    bool recursive = false;
    if (f.payload.size() > used)
        recursive = std::to_integer<u8>(f.payload[used]) != 0;

    bool ok = true;
    std::string msg;
    try {
        auto p = resolvePath(remote);
        std::error_code ec;
        if (recursive) fs::remove_all(p, ec);
        else           fs::remove(p, ec);
        if (ec) { ok = false; msg = ec.message(); }
    } catch (const std::exception& e) {
        ok = false; msg = e.what();
    }
    ByteBuffer ack;
    ack.push_back(std::byte(ok ? 1 : 0));
    put_str(ack, msg);
    sess->send(MsgType::DeleteAck, std::move(ack));
}

// =====================================================================
// Set mtime
// =====================================================================

void Server::handle_set_mtime(std::shared_ptr<detail::Session> sess,
                              detail::Frame f) {
    using namespace detail;
    std::size_t used = 0;
    std::string remote = get_str(f.payload.data(), f.payload.size(), used);
    if (f.payload.size() < used + 8) {
        sess->close(make_error_code(ErrorCode::ProtocolError));
        return;
    }
    const i64 ns = get_i64(f.payload.data() + used);

    bool ok = true; std::string msg;
    try {
        auto p = resolvePath(remote);
        std::error_code ec;
        using file_dur = std::filesystem::file_time_type::duration;
        auto t = std::filesystem::file_time_type{
            std::chrono::duration_cast<file_dur>(
                std::chrono::nanoseconds{ns})};
        fs::last_write_time(p, t, ec);
        if (ec) { ok = false; msg = ec.message(); }
    } catch (const std::exception& e) {
        ok = false; msg = e.what();
    }
    ByteBuffer ack;
    ack.push_back(std::byte(ok ? 1 : 0));
    put_str(ack, msg);
    sess->send(MsgType::SetMtimeAck, std::move(ack));
}

// =====================================================================
// Mkdir
// =====================================================================

void Server::handle_mkdir(std::shared_ptr<detail::Session> sess,
                          detail::Frame f) {
    using namespace detail;
    std::size_t used = 0;
    std::string remote = get_str(f.payload.data(), f.payload.size(), used);
    bool parents = true;
    if (f.payload.size() > used)
        parents = std::to_integer<u8>(f.payload[used]) != 0;

    bool ok = true; std::string msg;
    try {
        auto p = resolvePath(remote);
        std::error_code ec;
        if (parents) fs::create_directories(p, ec);
        else         fs::create_directory(p, ec);
        if (ec) { ok = false; msg = ec.message(); }
    } catch (const std::exception& e) {
        ok = false; msg = e.what();
    }
    ByteBuffer ack;
    ack.push_back(std::byte(ok ? 1 : 0));
    put_str(ack, msg);
    sess->send(MsgType::MkdirAck, std::move(ack));
}

// =====================================================================
// Remote-to-remote copy
// =====================================================================

void Server::handle_remote_copy(std::shared_ptr<detail::Session> sess,
                                detail::Frame f) {
    using namespace detail;
    std::size_t used1 = 0;
    std::string src = get_str(f.payload.data(), f.payload.size(), used1);
    if (used1 == 0) {
        sess->close(make_error_code(ErrorCode::ProtocolError));
        return;
    }
    std::size_t used2 = 0;
    std::string dst = get_str(f.payload.data() + used1,
                              f.payload.size() - used1, used2);
    if (used2 == 0) {
        sess->close(make_error_code(ErrorCode::ProtocolError));
        return;
    }

    bool ok = true; std::string msg; u64 copied = 0;
    try {
        auto sp = resolvePath(src);
        auto dp = resolvePath(dst);
        std::error_code ec;
        fs::create_directories(dp.parent_path(), ec);
        fs::copy_file(sp, dp, fs::copy_options::overwrite_existing, ec);
        if (ec) { ok = false; msg = ec.message(); }
        else {
            std::error_code sEc;
            copied = fs::file_size(dp, sEc);
            if (sEc) copied = 0;
        }
    } catch (const std::exception& e) {
        ok = false; msg = e.what();
    }
    ByteBuffer ack;
    ack.push_back(std::byte(ok ? 1 : 0));
    put_u64_into(ack, copied);
    put_str(ack, msg);
    sess->send(MsgType::RemoteCopyAck, std::move(ack));
}

// =====================================================================
// Handler registration
// =====================================================================

void Server::registerHandler(std::string method, GenericHandler handler) {
    std::lock_guard<std::mutex> lk(handlersMu_);
    handlers_[std::move(method)] = std::move(handler);
}

// =====================================================================
// Rename
// =====================================================================

void Server::handle_rename(std::shared_ptr<detail::Session> sess,
                           detail::Frame f) {
    using namespace detail;

    RenameReqData rq;
    if (!decode_rename_req(f.payload, rq)) {
        sess->close(make_error_code(ErrorCode::ProtocolError));
        return;
    }

    bool ok = true;
    std::string msg;
    try {
        auto oldP = resolvePath(rq.oldPath);
        auto newP = resolvePath(rq.newPath);

        std::error_code ec;
        fs::create_directories(newP.parent_path(), ec);

        fs::rename(oldP, newP, ec);
        if (ec) {
            ok = false;
            msg = ec.message();
        }
    } catch (const std::exception& e) {
        ok = false;
        msg = e.what();
    }

    ByteBuffer ack = encode_rename_ack(ok, msg);
    sess->send(MsgType::RenameAck, std::move(ack));
}

} // namespace filexferlib
