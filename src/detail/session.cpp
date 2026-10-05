#include "filexferlib/detail/session.hpp"
#include "filexferlib/detail/protocol.hpp"
#include <cstring>

namespace filexferlib::detail {

Session::Session(asio::io_context& io, Limits limits)
    : socket_(io), limits_(limits) {}

void Session::start(asio::ip::tcp::socket socket) {
    socket_ = std::move(socket);
    open_.store(true);
    do_read_header();
}

void Session::send(MsgType type, ByteBuffer payload) {
    send_with_callback(type, std::move(payload), {});
}

void Session::send_with_callback(MsgType type, ByteBuffer payload, WriteDone done) {
    if (!open_.load()) {
        if (done) done(make_error_code(ErrorCode::NetworkError), 0);
        return;
    }
    ByteBuffer out;
    encode_frame(type, payload, out);
    const bool idle = writeQueue_.empty();
    callbacks_.push_back(std::move(done));
    writeQueue_.push_back(std::move(out));
    if (idle) do_write();
}

void Session::do_write() {
    if (writeQueue_.empty()) return;
    auto self = shared_from_this();
    asio::async_write(socket_, asio::buffer(writeQueue_.front()),
        [self](std::error_code ec, std::size_t written) {
            WriteDone cb;
            if (!self->callbacks_.empty()) {
                cb = std::move(self->callbacks_.front());
                self->callbacks_.pop_front();
            }
            if (cb) cb(ec, written);
            if (ec) { self->fail(ec); return; }
            self->writeQueue_.pop_front();
            self->do_write();
        });
}

void Session::do_read_header() {
    if (!open_.load()) return;
    auto self = shared_from_this();
    asio::async_read(socket_, asio::buffer(headerBuf_),
        [self](std::error_code ec, std::size_t) {
            if (ec) { self->fail(ec); return; }

            const byte* p = self->headerBuf_.data();
            if (get_u16(p) != kMagic) {
                self->fail(make_error_code(ErrorCode::ProtocolError));
                return;
            }
            if (std::to_integer<u8>(p[2]) != kProtocolVersion) {
                self->fail(make_error_code(ErrorCode::VersionMismatch));
                return;
            }

            self->pendingType_ =
                static_cast<MsgType>(std::to_integer<u8>(p[3]));
            self->pendingLen_ = get_u32(p + 4);

            if (self->pendingLen_ > self->limits_.maxPayloadSize) {
                self->fail(make_error_code(ErrorCode::PayloadTooLarge));
                return;
            }
            self->do_read_body(self->pendingLen_, self->pendingType_);
        });
}

void Session::do_read_body(std::uint32_t length, MsgType type) {
    bodyBuf_.resize(length);
    auto self = shared_from_this();
    asio::async_read(socket_, asio::buffer(bodyBuf_),
        [self, type](std::error_code ec, std::size_t) {
            if (ec) { self->fail(ec); return; }
            Frame f;
            f.type = type;
            f.payload = std::move(self->bodyBuf_);
            self->dispatch(std::move(f));
            self->do_read_header();
        });
}

void Session::dispatch(Frame f) {
    // ---- Generic response ----
    if (f.type == MsgType::GenericResp) {
        if (f.payload.size() < 6) {
            fail(make_error_code(ErrorCode::ProtocolError));
            return;
        }
        const u32 rid = get_u32(f.payload.data());
        const u16 status = get_u16(f.payload.data() + 4);
        GenericResponse resp;
        resp.statusCode = status;
        resp.payload.assign(f.payload.begin() + 6, f.payload.end());

        std::shared_ptr<std::promise<GenericResponse>> pr;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = pending_.find(rid);
            if (it != pending_.end()) { pr = it->second; pending_.erase(it); }
        }
        if (pr) pr->set_value(std::move(resp));
        return;
    }

    // ---- Any frame whose payload starts with transferId(u32) goes
    //      through the transfer registry first. If no handler matches,
    //      fall through to onMessage_ (server-side dispatch). ----
    const bool is_transfer_frame =
        f.type == MsgType::FileBegin        ||
        f.type == MsgType::FileChunk        ||
        f.type == MsgType::FileEnd          ||
        f.type == MsgType::FileAck          ||
        f.type == MsgType::FileResumeReq    ||
        f.type == MsgType::FileResumeResp   ||
        f.type == MsgType::DownloadResumeReq  ||
        f.type == MsgType::DownloadResumeResp ||
        f.type == MsgType::FileCancelReq    ||
        f.type == MsgType::FilePause        ||
        f.type == MsgType::FileResume;

    if (is_transfer_frame) {
        if (f.payload.size() < 4) {
            fail(make_error_code(ErrorCode::ProtocolError));
            return;
        }
        const u32 tid = get_u32(f.payload.data());
        Frame copy = f;
        if (transfers_->dispatch(std::move(copy), tid)) return;
        // fall through to onMessage_
    }

    if (onMessage_) onMessage_(std::move(f));
}

void Session::close(std::error_code ec) {
    bool expected = true;
    if (!open_.compare_exchange_strong(expected, false)) return;

    std::error_code ignored;
    socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);

    for (auto& h : closeHandlers_) {
        if (h) { try { h(ec); } catch (...) {} }
    }
    closeHandlers_.clear();

    if (onClose_) onClose_(ec);
}

void Session::fail(std::error_code ec) {
    if (ec == asio::error::eof || ec == asio::error::operation_aborted)
        ec = make_error_code(ErrorCode::NetworkError);

    std::unordered_map<std::uint32_t,
        std::shared_ptr<std::promise<GenericResponse>>> toComplete;
    {
        std::lock_guard<std::mutex> lk(mu_);
        toComplete.swap(pending_);
    }
    for (auto& kv : toComplete) {
        GenericResponse r;
        r.statusCode = 0xFFFF;
        kv.second->set_value(std::move(r));
    }

    close(ec);
}

std::future<GenericResponse>
Session::call_generic(std::string method, ByteBuffer payload, CallOptions) {
    auto pr = std::make_shared<std::promise<GenericResponse>>();
    auto fut = pr->get_future();

    if (method.size() > limits_.maxMethodNameLength) {
        GenericResponse r; r.statusCode = 0xFFFE;
        pr->set_value(std::move(r));
        return fut;
    }

    u32 rid;
    {
        std::lock_guard<std::mutex> lk(mu_);
        rid = nextRequestId_++;
        pending_[rid] = pr;
    }

    ByteBuffer buf;
    buf.resize(6 + method.size() + payload.size());
    put_u32(buf.data(), rid);
    put_u16(buf.data() + 4, static_cast<u16>(method.size()));
    std::memcpy(buf.data() + 6, method.data(), method.size());
    if (!payload.empty())
        std::memcpy(buf.data() + 6 + method.size(),
                    payload.data(), payload.size());

    send(MsgType::GenericReq, std::move(buf));
    return fut;
}

} // namespace filexferlib::detail
