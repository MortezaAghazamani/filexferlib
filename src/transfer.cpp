#include "filexferlib/client.hpp"
#include "filexferlib/detail/session.hpp"
#include "filexferlib/detail/protocol.hpp"
#include "filexferlib/detail/hash.hpp"
#include "filexferlib/detail/compression.hpp"
#include "filexferlib/detail/transfer_registry.hpp"
#include "filexferlib/detail/transfer_util.hpp"
#include "filexferlib/detail/transfer_timer.hpp"
#include "filexferlib/error.hpp"
#include "filexferlib/logger.hpp"

#include <fstream>
#include <chrono>
#include <cstring>
#include <array>
#include <vector>
#include <thread>
#include <memory>

namespace filexferlib {

namespace {

using namespace std::chrono;
using namespace detail;

// =====================================================================
// Utilities
// =====================================================================

std::int64_t file_mtime_ns(const std::filesystem::path& p) {
    std::error_code ec;
    auto t = std::filesystem::last_write_time(p, ec);
    if (ec) return 0;
    return duration_cast<nanoseconds>(t.time_since_epoch()).count();
}

// =====================================================================
// Upload state machine
// =====================================================================

struct UploadState {
    std::shared_ptr<Session> sess;
    std::filesystem::path local;
    std::string remotePath;
    CopyOptions opts;
    ProgressCallback onProgress;
    CancellationToken cancel;
    std::shared_ptr<std::promise<TransferResult>> promise;
    std::shared_ptr<TransferControl> control;

    u32 transferId = 0;
    std::ifstream file;
    std::vector<char> buf;
    u64 offset = 0;
    u64 totalSize = 0;
    u64 srcHash = 0;
    i64 mtime = 0;
    steady_clock::time_point start;
    steady_clock::time_point lastProgress;
    std::chrono::milliseconds timeoutMs{30000};

    enum class Phase { Begin, Chunk, End, Done } phase = Phase::Begin;
    bool finished = false;
    u32 inflight = 0;
    bool waitingResume = false;

    u64 resumeOffset = 0;
    bool isResume = false;

    TransferTimer::Ptr timer;
};

void upload_send_chunk(std::shared_ptr<UploadState> st);

void finish_upload(std::shared_ptr<UploadState> st, std::exception_ptr e) {
    if (st->finished) return;
    st->finished = true;
    if (st->timer) st->timer->disarm();
    st->sess->transfers()->unregister_handler(st->transferId);
    if (st->control) {
        st->control->state.store(static_cast<int>(
            e ? TransferHandle::State::Failed : TransferHandle::State::Done));
    }
    if (e) st->promise->set_exception(e);
}

void fail_upload(std::shared_ptr<UploadState> st, std::error_code ec,
                 const std::string& what) {
    finish_upload(st, std::make_exception_ptr(std::system_error(ec, what)));
}

void on_upload_ack(std::shared_ptr<UploadState> st, Frame f) {
    if (st->finished) return;

    if (st->cancel && st->cancel->load()) {
        fail_upload(st, make_error_code(ErrorCode::Cancelled), "cancelled");
        return;
    }
    if (st->control && st->control->cancelled.load()) {
        fail_upload(st, make_error_code(ErrorCode::Cancelled), "cancelled");
        return;
    }

    if (st->timer) st->timer->arm();

    FileAckData ack;
    bool decoded = decode_file_ack(f.payload, ack);
    if (!decoded) {
        fail_upload(st, make_error_code(ErrorCode::ProtocolError), "bad file_ack");
        return;
    }
    if (!ack.ok) {
        finish_upload(st, std::make_exception_ptr(
            std::runtime_error("server rejected: " + ack.msg)));
        return;
    }

    switch (st->phase) {
    case UploadState::Phase::Begin: {
        st->resumeOffset = ack.hash;
        if (st->resumeOffset > 0 && st->resumeOffset < st->totalSize) {
            st->isResume = true;
            st->offset = st->resumeOffset;
            st->file.seekg(static_cast<std::streamoff>(st->resumeOffset));
            FX_INFO("upload resuming from offset ", st->resumeOffset);
        } else {
            st->offset = 0;
        }
        st->phase = UploadState::Phase::Chunk;
        upload_send_chunk(st);
        break;
    }

    case UploadState::Phase::Chunk: {
        if (st->inflight > 0) st->inflight--;

        if (st->control) {
            st->control->bytesTransferred.store(st->offset);
        }

        if (st->onProgress) {
            const auto now = steady_clock::now();
            const auto elapsed = duration_cast<milliseconds>(
                now - st->lastProgress).count();
            const auto interval = st->opts.progressInterval.count();
            if (interval == 0 || elapsed >= interval) {
                st->lastProgress = now;
                TransferProgress tp;
                tp.bytesTransferred = st->offset;
                tp.totalBytes = st->totalSize;
                tp.currentPath = st->remotePath;
                const auto ms = duration_cast<milliseconds>(
                    now - st->start).count();
                tp.bytesPerSecond = ms > 0
                    ? (st->offset * 1000ull / static_cast<u64>(ms)) : 0;
                st->onProgress(tp);
            }
        }
        upload_send_chunk(st);
        break;
    }

    case UploadState::Phase::End: {
        TransferResult result;
        result.bytesTransferred = st->offset;
        result.xxhashSource = hash_to_hex(st->srcHash);
        if (st->opts.verifyHash && ack.hash != 0) {
            result.xxhashDest = hash_to_hex(ack.hash);
            result.verified = (ack.hash == st->srcHash);
        }
        const auto ms = duration_cast<milliseconds>(
            steady_clock::now() - st->start).count();
        result.elapsedMs = static_cast<u64>(ms);

        st->finished = true;
        if (st->timer) st->timer->disarm();
        st->sess->transfers()->unregister_handler(st->transferId);
        if (st->control) {
            st->control->state.store(
                static_cast<int>(TransferHandle::State::Done));
        }
        st->promise->set_value(std::move(result));
        break;
    }
    default: break;
    }
}

void upload_send_chunk(std::shared_ptr<UploadState> st) {
    if (st->finished) return;

    if (st->cancel && st->cancel->load()) {
        fail_upload(st, make_error_code(ErrorCode::Cancelled), "cancelled");
        return;
    }

    if (st->control) {
        if (st->control->cancelled.load()) {
            fail_upload(st, make_error_code(ErrorCode::Cancelled), "cancelled");
            return;
        }
        if (st->control->paused.load()) {
            st->waitingResume = true;
            return;
        }
    }
    st->waitingResume = false;

    while (st->inflight < st->opts.maxInflightChunks &&
           st->offset < st->totalSize)
    {
        if (st->cancel && st->cancel->load()) {
            fail_upload(st, make_error_code(ErrorCode::Cancelled), "cancelled");
            return;
        }

        st->file.read(st->buf.data(),
                      static_cast<std::streamsize>(st->buf.size()));
        const auto n = st->file.gcount();
        if (n <= 0) break;

        const auto bytes = static_cast<std::size_t>(n);

        // ---- Apply compression if requested ----
        if (st->opts.compression == Compression::Zstd) {
            auto compressed = detail::zstd_compress(
                st->buf.data(), bytes, st->opts.compressionLevel);

            if (!compressed.empty()) {
                // payload format: origSize(4) | compressed-data
                ByteBuffer payload(4 + compressed.size());
                detail::put_u32(payload.data(),
                                static_cast<detail::u32>(bytes));
                std::memcpy(payload.data() + 4,
                            compressed.data(),
                            compressed.size());

                // frame format: transferId(4) | offset(8) | payload
                ByteBuffer frame(4 + 8 + payload.size());
                detail::put_u32(frame.data(), st->transferId);
                detail::put_u64(frame.data() + 4, st->offset);
                std::memcpy(frame.data() + 12,
                            payload.data(),
                            payload.size());

                st->sess->send(MsgType::FileChunk, std::move(frame));
            } else {
                // Compression failed — send raw
                st->sess->send(MsgType::FileChunk,
                               encode_file_chunk(st->transferId, st->offset,
                                                 st->buf.data(), bytes));
            }
        } else {
            st->sess->send(MsgType::FileChunk,
                           encode_file_chunk(st->transferId, st->offset,
                                             st->buf.data(), bytes));
        }

        st->offset += static_cast<u64>(bytes);
        st->inflight++;
    }

    if (st->offset >= st->totalSize && st->inflight == 0) {
        st->phase = UploadState::Phase::End;
        st->sess->send(MsgType::FileEnd,
                       encode_file_end(st->transferId, st->srcHash));
    }
}

void start_upload(std::shared_ptr<UploadState> st) {
    if (st->finished) return;

    // For empty files, we don't need to open the file.
    if (st->totalSize == 0) {
        // Just send FileBegin (with size=0) and FileEnd.
        // No chunks.
        st->start = steady_clock::now();
        st->lastProgress = st->start;
        st->buf.clear();

        auto weak = std::weak_ptr<UploadState>(st);

        st->sess->transfers()->register_handler(
            st->transferId,
            [st](Frame f) { on_upload_ack(st, std::move(f)); }
        );

        st->sess->add_close_handler([weak](std::error_code ec){
            auto s = weak.lock();
            if (!s) return;
            if (s->finished) return;
            finish_upload(s, std::make_exception_ptr(
                std::system_error(
                    ec ? ec : make_error_code(ErrorCode::NetworkError),
                    "session closed")));
        });

        auto& ioc = static_cast<asio::io_context&>(
            st->sess->socket().get_executor().context());
        st->timer = std::make_shared<TransferTimer>(ioc, st->timeoutMs);
        st->timer->set_on_timeout([weak]{
            auto s = weak.lock();
            if (!s) return;
            if (s->finished) return;
            fail_upload(s, make_error_code(ErrorCode::Timeout), "ack timeout");
        });
        st->timer->arm();

        st->sess->send(MsgType::FileBegin,
            encode_file_begin(st->transferId, st->remotePath,
                              0, st->mtime, 0,
                              st->opts.compression == Compression::Zstd));
        return;
    }

    st->file.open(st->local, std::ios::binary);
    if (!st->file) {
        finish_upload(st, std::make_exception_ptr(
            std::runtime_error("cannot open local file")));
        return;
    }

    if (st->opts.verifyHash) {
        std::error_code ec;
        st->srcHash = xxhash_file(st->local, ec);
        if (ec) {
            finish_upload(st, std::make_exception_ptr(
                std::system_error(ec, "hash")));
            return;
        }
    }

    st->start = steady_clock::now();
    st->lastProgress = st->start;
    st->buf.resize(st->opts.chunkSize ? st->opts.chunkSize : 256 * 1024);

    auto weak = std::weak_ptr<UploadState>(st);

    st->sess->transfers()->register_handler(
        st->transferId,
        [st](Frame f) { on_upload_ack(st, std::move(f)); }
    );

    st->sess->add_close_handler([weak](std::error_code ec){
        auto s = weak.lock();
        if (!s) return;
        if (s->finished) return;
        finish_upload(s, std::make_exception_ptr(
            std::system_error(
                ec ? ec : make_error_code(ErrorCode::NetworkError),
                "session closed")));
    });

    auto& ioc = static_cast<asio::io_context&>(
        st->sess->socket().get_executor().context());
    st->timer = std::make_shared<TransferTimer>(ioc, st->timeoutMs);
    st->timer->set_on_timeout([weak]{
        auto s = weak.lock();
        if (!s) return;
        if (s->finished) return;
        fail_upload(s, make_error_code(ErrorCode::Timeout), "ack timeout");
    });
    st->timer->arm();

    st->sess->send(MsgType::FileBegin,
        encode_file_begin(st->transferId, st->remotePath,
                          st->totalSize, st->mtime, st->srcHash,
                          st->opts.compression == Compression::Zstd));
}

// =====================================================================
// Download state machine
// =====================================================================

struct DownloadState {
    std::shared_ptr<Session> sess;
    std::string remotePath;
    std::filesystem::path local;
    CopyOptions opts;
    ProgressCallback onProgress;
    CancellationToken cancel;
    std::shared_ptr<std::promise<TransferResult>> promise;
    std::shared_ptr<TransferControl> control;

    u32 transferId = 0;
    std::ofstream file;
    std::unique_ptr<FileHasher> hasher;
    u64 totalSize = 0;
    u64 received = 0;
    u64 expectedHash = 0;
    i64 expectedMtimeNs = 0;
    steady_clock::time_point start;
    steady_clock::time_point lastProgress;
    std::chrono::milliseconds timeoutMs{30000};
    bool finished = false;

    bool resumeProbed = false;
    bool resumeAttempted = false;
    u64 resumeFrom = 0;
    bool isCompressed = false;

    TransferTimer::Ptr timer;
};

void finish_download(std::shared_ptr<DownloadState> st, std::exception_ptr e) {
    if (st->finished) return;
    st->finished = true;
    if (st->timer) st->timer->disarm();
    st->sess->transfers()->unregister_handler(st->transferId);
    if (st->file.is_open()) {
        st->file.flush();
        st->file.close();
    }
    if (st->control) {
        st->control->state.store(static_cast<int>(
            e ? TransferHandle::State::Failed : TransferHandle::State::Done));
    }
    if (e) st->promise->set_exception(e);
}

void fail_download(std::shared_ptr<DownloadState> st, std::error_code ec,
                   const std::string& what) {
    finish_download(st, std::make_exception_ptr(std::system_error(ec, what)));
}

static void send_download_ack(std::shared_ptr<DownloadState> st, bool ok,
                              u64 hash = 0, const std::string& msg = {}) {
    st->sess->send(MsgType::FileAck,
        encode_file_ack(st->transferId, ok, hash, msg));
}

void send_download_resume_req(std::shared_ptr<DownloadState> st) {
    std::filesystem::path partPath = st->local;
    partPath += ".fxfer.part";

    u64 have = 0;
    std::error_code ec;
    if (std::filesystem::exists(partPath, ec) &&
        std::filesystem::is_regular_file(partPath, ec))
    {
        have = std::filesystem::file_size(partPath, ec);
        if (ec) have = 0;
    }

    st->resumeFrom = have;
    st->resumeProbed = true;

    if (have > 0) {
        FX_INFO("download probe: local partial size=", have);
    }

    st->sess->send(MsgType::DownloadResumeReq,
                   encode_download_resume_req(st->transferId,
                                              st->remotePath,
                                              have));
}

void on_download_frame(std::shared_ptr<DownloadState> st, Frame f) {
    if (st->finished) return;

    if (st->cancel && st->cancel->load()) {
        fail_download(st, make_error_code(ErrorCode::Cancelled), "cancelled");
        return;
    }
    if (st->control && st->control->cancelled.load()) {
        fail_download(st, make_error_code(ErrorCode::Cancelled), "cancelled");
        return;
    }

    if (st->timer) st->timer->arm();

    switch (f.type) {

    case MsgType::DownloadResumeResp: {
        DownloadResumeRespData rd;
        if (!decode_download_resume_resp(f.payload, rd) || !rd.ok) {
            fail_download(st, make_error_code(ErrorCode::PathNotFound),
                          "server refused resume");
            return;
        }

        st->totalSize = rd.totalSize;
        st->expectedHash = rd.xxhash;
        st->expectedMtimeNs = rd.mtimeNs;
        st->resumeFrom = rd.serverOffset;
        st->isCompressed = rd.compressed;

        std::filesystem::path partPath = st->local;
        partPath += ".fxfer.part";

        std::error_code ec;
        std::filesystem::create_directories(st->local.parent_path(), ec);

        if (st->resumeFrom > 0 &&
            std::filesystem::exists(partPath, ec))
        {
            st->file.open(partPath, std::ios::binary | std::ios::in | std::ios::out);
            if (!st->file) {
                std::filesystem::remove(partPath, ec);
                st->file.open(partPath, std::ios::binary | std::ios::trunc);
                st->resumeFrom = 0;
            } else {
                st->file.seekp(static_cast<std::streamoff>(st->resumeFrom));
                FX_INFO("download resuming from offset ", st->resumeFrom);
            }
        } else {
            st->file.open(partPath, std::ios::binary | std::ios::trunc);
            st->resumeFrom = 0;
        }

        if (!st->file) {
            fail_download(st, make_error_code(ErrorCode::InternalError),
                          "cannot open local part file");
            return;
        }

        st->received = st->resumeFrom;
        if (st->opts.verifyHash) st->hasher = std::make_unique<FileHasher>();

        st->start = steady_clock::now();
        st->lastProgress = st->start;

        // Send pull request: FileBegin with mtime=-1 (is_pull).
        st->sess->send(MsgType::FileBegin,
            encode_download_pull(st->transferId, st->remotePath));
        break;
    }

    case MsgType::FileBegin: {
        FileBeginData bd;
        if (!decode_file_begin(f.payload, bd)) {
            fail_download(st, make_error_code(ErrorCode::ProtocolError),
                          "bad file_begin");
            return;
        }
        st->remotePath = bd.path;
        st->totalSize = bd.size;
        st->expectedHash = bd.xxhash;
        st->expectedMtimeNs = bd.mtimeNs;
        st->isCompressed = bd.compressed;

        send_download_ack(st, true);
        break;
    }

    case MsgType::FileChunk: {
        FileChunkData cd;
        if (!decode_file_chunk(f.payload, cd)) {
            fail_download(st, make_error_code(ErrorCode::ProtocolError),
                          "bad file_chunk");
            return;
        }
        if (cd.offset != st->received) {
            fail_download(st, make_error_code(ErrorCode::ProtocolError),
                          "chunk out of order");
            return;
        }

        // ---- Decompress if needed ----
        std::vector<char> decompressedBuf;
        const char* dataToWrite = reinterpret_cast<const char*>(cd.data);
        std::size_t lenToWrite = cd.len;

        if (st->isCompressed) {
            if (cd.len < 4) {
                fail_download(st, make_error_code(ErrorCode::ProtocolError),
                              "compressed chunk too short");
                return;
            }
            u32 origSize = detail::get_u32(cd.data);
            auto decompressed = detail::zstd_decompress(
                cd.data + 4, cd.len - 4, origSize);
            if (decompressed.empty()) {
                fail_download(st, make_error_code(ErrorCode::ProtocolError),
                              "decompression failed");
                return;
            }
            if (decompressed.size() != origSize) {
                fail_download(st, make_error_code(ErrorCode::ProtocolError),
                              "decompressed size mismatch");
                return;
            }
            decompressedBuf.assign(
                reinterpret_cast<const char*>(decompressed.data()),
                reinterpret_cast<const char*>(decompressed.data())
                    + decompressed.size());
            dataToWrite = decompressedBuf.data();
            lenToWrite = decompressedBuf.size();
        }

        st->file.write(dataToWrite, static_cast<std::streamsize>(lenToWrite));
        if (st->hasher) st->hasher->update(dataToWrite, lenToWrite);
        st->received += lenToWrite;

        if (st->control) {
            st->control->bytesTransferred.store(st->received);
        }

        if (st->onProgress) {
            const auto now = steady_clock::now();
            const auto elapsed = duration_cast<milliseconds>(
                now - st->lastProgress).count();
            const auto interval = st->opts.progressInterval.count();
            if (interval == 0 || elapsed >= interval) {
                st->lastProgress = now;
                TransferProgress tp;
                tp.bytesTransferred = st->received;
                tp.totalBytes = st->totalSize;
                tp.currentPath = st->remotePath;
                const auto ms = duration_cast<milliseconds>(
                    now - st->start).count();
                tp.bytesPerSecond = ms > 0
                    ? (st->received * 1000ull / static_cast<u64>(ms)) : 0;
                st->onProgress(tp);
            }
        }

        send_download_ack(st, true);
        break;
    }

    case MsgType::FileEnd: {
        u64 srcHash = 0;
        if (!decode_file_end(f.payload, srcHash)) {
            fail_download(st, make_error_code(ErrorCode::ProtocolError),
                          "bad file_end");
            return;
        }
        st->file.flush();
        st->file.close();

        std::filesystem::path partPath = st->local;
        partPath += ".fxfer.part";

        u64 dstHash = 0;
        if (st->resumeFrom > 0) {
            std::error_code hEc;
            dstHash = xxhash_file(partPath, hEc);
            if (hEc) dstHash = 0;
        } else if (st->hasher) {
            dstHash = st->hasher->digest();
        }
        const bool ok = (srcHash == 0) || (dstHash == srcHash);

        send_download_ack(st, ok, dstHash,
                          ok ? "" : "hash mismatch");

        if (!ok) {
            std::error_code rmEc;
            std::filesystem::remove(partPath, rmEc);
            fail_download(st, make_error_code(ErrorCode::HashMismatch),
                          "hash mismatch after download");
            return;
        }

        if (st->opts.preserveTimes && st->expectedMtimeNs != 0) {
            std::error_code timeEc;
            using file_dur = std::filesystem::file_time_type::duration;
            auto t = std::filesystem::file_time_type{
                std::chrono::duration_cast<file_dur>(
                    std::chrono::nanoseconds{st->expectedMtimeNs})};
            std::filesystem::last_write_time(partPath, t, timeEc);
        }

        std::filesystem::path finalPath = st->local;
        bool finalized = false;
        {
            std::error_code rmEc;
            std::filesystem::remove(finalPath, rmEc);
            std::error_code rnEc;
            std::filesystem::rename(partPath, finalPath, rnEc);
            if (!rnEc) finalized = true;
        }
        if (!finalized) {
            for (int i = 0; i < 5 && !finalized; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                std::error_code rmEc;
                std::filesystem::remove(finalPath, rmEc);
                std::error_code rnEc;
                std::filesystem::rename(partPath, finalPath, rnEc);
                if (!rnEc) finalized = true;
            }
        }
        if (!finalized) {
            std::error_code cpEc;
            std::filesystem::copy_file(partPath, finalPath,
                std::filesystem::copy_options::overwrite_existing, cpEc);
            if (!cpEc) {
                finalized = true;
                for (int i = 0; i < 10; ++i) {
                    std::error_code rmEc;
                    std::filesystem::remove(partPath, rmEc);
                    if (!rmEc) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            }
        }

        TransferResult result;
        result.bytesTransferred = st->received;
        result.xxhashSource = hash_to_hex(srcHash);
        result.xxhashDest = hash_to_hex(dstHash);
        result.verified = (srcHash == dstHash);
        const auto ms = duration_cast<milliseconds>(
            steady_clock::now() - st->start).count();
        result.elapsedMs = static_cast<u64>(ms);

        st->finished = true;
        if (st->timer) st->timer->disarm();
        st->sess->transfers()->unregister_handler(st->transferId);
        if (st->control) {
            st->control->state.store(
                static_cast<int>(TransferHandle::State::Done));
        }
        st->promise->set_value(std::move(result));
        break;
    }

    default:
        FX_WARN("download: unexpected frame type=0x",
                static_cast<int>(f.type));
        break;
    }
}

} // namespace

// =====================================================================
// Client::upload
// =====================================================================

Client::UploadResult Client::uploadInternal(
    const std::filesystem::path& local,
    const std::string& remotePath,
    CopyOptions opts,
    ProgressCallback onProgress,
    CancellationToken cancel)
{
    auto pr = std::make_shared<std::promise<TransferResult>>();
    auto fut = pr->get_future();

    Client::UploadResult res;
    res.future = std::move(fut);

    if (!session_) {
        pr->set_exception(std::make_exception_ptr(
            std::runtime_error("not connected")));
        return res;
    }
    if (!cancel) cancel = make_cancellation_token();

    // Adapt chunkSize to the server's advertised limit
    if (connectOpts_.recommendedChunkSize > 0 &&
        opts.chunkSize > connectOpts_.recommendedChunkSize)
    {
        opts.chunkSize = connectOpts_.recommendedChunkSize;
    }

    std::error_code fsEc;
    const auto size = std::filesystem::file_size(local, fsEc);
    if (fsEc) {
        pr->set_exception(std::make_exception_ptr(
            std::system_error(fsEc, "file_size")));
        return res;
    }

    auto st = std::make_shared<UploadState>();
    st->sess = session_;
    st->local = local;
    st->remotePath = remotePath;
    st->opts = opts;
    st->onProgress = std::move(onProgress);
    st->cancel = std::move(cancel);
    st->promise = pr;
    st->totalSize = static_cast<u64>(size);
    st->mtime = file_mtime_ns(local);
    st->transferId = session_->transfers()->alloc_id();
    st->timeoutMs = connectOpts_.readTimeout;

    auto ctrl = std::make_shared<TransferControl>();
    ctrl->totalBytes.store(st->totalSize);
    st->control = ctrl;
    res.handle = TransferHandle(ctrl);

    auto weak = std::weak_ptr<UploadState>(st);
    ctrl->resumeCallback = [this, weak]() {
        asio::post(io_, [weak]{
            auto s = weak.lock();
            if (!s || s->finished) return;
            if (s->control->cancelled.load()) {
                fail_upload(s, make_error_code(ErrorCode::Cancelled),
                            "cancelled");
                return;
            }
            if (s->control->paused.load()) return;
            if (s->waitingResume) {
                s->waitingResume = false;
                upload_send_chunk(s);
            }
        });
    };

    asio::post(io_, [st]{ start_upload(st); });
    return res;
}

Client::UploadResult Client::uploadWithHandle(
    const std::filesystem::path& local,
    const std::string& remotePath,
    CopyOptions opts,
    ProgressCallback onProgress)
{
    return uploadInternal(local, remotePath,
                          std::move(opts),
                          std::move(onProgress),
                          {});
}

std::future<TransferResult> Client::upload(
    const std::filesystem::path& local,
    const std::string& remotePath,
    CopyOptions opts,
    ProgressCallback onProgress,
    CancellationToken cancel)
{
    return uploadInternal(local, remotePath,
                          std::move(opts),
                          std::move(onProgress),
                          std::move(cancel)).future;
}

// =====================================================================
// Client::download
// =====================================================================

Client::DownloadResult Client::downloadInternal(
    const std::string& remotePath,
    const std::filesystem::path& local,
    CopyOptions opts,
    ProgressCallback onProgress,
    CancellationToken cancel)
{
    auto pr = std::make_shared<std::promise<TransferResult>>();
    auto fut = pr->get_future();

    Client::DownloadResult res;
    res.future = std::move(fut);

    if (!session_) {
        pr->set_exception(std::make_exception_ptr(
            std::runtime_error("not connected")));
        return res;
    }
    if (!cancel) cancel = make_cancellation_token();

    if (connectOpts_.recommendedChunkSize > 0 &&
        opts.chunkSize > connectOpts_.recommendedChunkSize)
    {
        opts.chunkSize = connectOpts_.recommendedChunkSize;
    }

    auto st = std::make_shared<DownloadState>();
    st->sess = session_;
    st->remotePath = remotePath;
    st->local = local;
    st->opts = opts;
    st->onProgress = std::move(onProgress);
    st->cancel = std::move(cancel);
    st->promise = pr;
    st->transferId = session_->transfers()->alloc_id();
    st->timeoutMs = connectOpts_.readTimeout;

    auto ctrl = std::make_shared<TransferControl>();
    st->control = ctrl;
    res.handle = TransferHandle(ctrl);

    auto weak = std::weak_ptr<DownloadState>(st);
    ctrl->resumeCallback = [this, weak]() {
        asio::post(io_, [weak]{
            auto s = weak.lock();
            if (!s || s->finished) return;
            if (s->control->cancelled.load()) {
                fail_download(s, make_error_code(ErrorCode::Cancelled),
                              "cancelled");
                return;
            }
        });
    };

    asio::post(io_, [st]() mutable {
        using namespace detail;

        auto w = std::weak_ptr<DownloadState>(st);

        st->sess->transfers()->register_handler(
            st->transferId,
            [st](Frame f) { on_download_frame(st, std::move(f)); }
        );

        st->sess->add_close_handler([w](std::error_code ec){
            auto s = w.lock();
            if (!s) return;
            if (s->finished) return;
            finish_download(s, std::make_exception_ptr(
                std::system_error(
                    ec ? ec : make_error_code(ErrorCode::NetworkError),
                    "session closed")));
        });

        auto& ioc = static_cast<asio::io_context&>(
            st->sess->socket().get_executor().context());
        st->timer = std::make_shared<TransferTimer>(ioc, st->timeoutMs);
        st->timer->set_on_timeout([w]{
            auto s = w.lock();
            if (!s) return;
            if (s->finished) return;
            fail_download(s, make_error_code(ErrorCode::Timeout),
                          "download timeout");
        });
        st->timer->arm();

        send_download_resume_req(st);
    });

    return res;
}

Client::DownloadResult Client::downloadWithHandle(
    const std::string& remotePath,
    const std::filesystem::path& local,
    CopyOptions opts,
    ProgressCallback onProgress)
{
    return downloadInternal(remotePath, local,
                            std::move(opts),
                            std::move(onProgress),
                            {});
}

std::future<TransferResult> Client::download(
    const std::string& remotePath,
    const std::filesystem::path& local,
    CopyOptions opts,
    ProgressCallback onProgress,
    CancellationToken cancel)
{
    return downloadInternal(remotePath, local,
                            std::move(opts),
                            std::move(onProgress),
                            std::move(cancel)).future;
}

} // namespace filexferlib
