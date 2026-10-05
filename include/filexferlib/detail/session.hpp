#pragma once
#include "filexferlib/detail/common.hpp"
#include "filexferlib/detail/frame.hpp"
#include "filexferlib/detail/transfer_registry.hpp"
#include "filexferlib/types.hpp"
#include "filexferlib/error.hpp"
#include "filexferlib/logger.hpp"

#include <asio.hpp>
#include <memory>
#include <functional>
#include <deque>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <future>
#include <array>

namespace filexferlib::detail {

class Session : public std::enable_shared_from_this<Session> {
public:
    using Ptr = std::shared_ptr<Session>;
    using MessageHandler = std::function<void(Frame)>;
    using CloseHandler   = std::function<void(std::error_code)>;
    using WriteDone      = std::function<void(std::error_code, std::size_t)>;

    Session(asio::io_context& io, Limits limits);

    void start(asio::ip::tcp::socket socket);

    void set_message_handler(MessageHandler h) { onMessage_ = std::move(h); }
    void set_close_handler(CloseHandler h)     { onClose_   = std::move(h); }

    // Multiple close callbacks (add-only). Called in registration order
    // when the session closes, before onClose_.
    void add_close_handler(CloseHandler h) {
        if (h) closeHandlers_.push_back(std::move(h));
    }

    void send(MsgType type, ByteBuffer payload);
    void send_with_callback(MsgType type, ByteBuffer payload, WriteDone done);
    void close(std::error_code ec = {});

    bool is_open() const { return open_.load(); }
    asio::ip::tcp::socket& socket() { return socket_; }

    TransferRegistry::Ptr transfers() { return transfers_; }

    std::future<GenericResponse> call_generic(std::string method,
                                              ByteBuffer payload,
                                              CallOptions opts);

private:
    void do_read_header();
    void do_read_body(std::uint32_t length, MsgType type);
    void do_write();
    void fail(std::error_code ec);
    void dispatch(Frame frame);

    asio::ip::tcp::socket socket_;
    Limits limits_;

    std::array<std::byte, kHeaderSize> headerBuf_{};
    ByteBuffer bodyBuf_;
    MsgType    pendingType_{};
    std::uint32_t pendingLen_{};

    std::deque<ByteBuffer> writeQueue_;
    std::deque<WriteDone>  callbacks_;

    MessageHandler onMessage_;
    CloseHandler   onClose_;
    std::vector<CloseHandler> closeHandlers_;
    std::atomic<bool> open_{false};

    TransferRegistry::Ptr transfers_ = std::make_shared<TransferRegistry>();

    std::mutex mu_;
    std::uint32_t nextRequestId_ = 1;
    std::unordered_map<std::uint32_t,
        std::shared_ptr<std::promise<GenericResponse>>> pending_;
};

} // namespace filexferlib::detail
