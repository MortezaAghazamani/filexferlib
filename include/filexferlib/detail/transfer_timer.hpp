#pragma once
#include "filexferlib/detail/common.hpp"
#include "filexferlib/error.hpp"

#include <asio.hpp>
#include <memory>
#include <chrono>
#include <functional>

namespace filexferlib::detail {

// Simple per-transfer watchdog timer.
// Call arm() to (re)start, disarm() to stop.
// on_timeout fires if the timer expires.
class TransferTimer : public std::enable_shared_from_this<TransferTimer> {
public:
    using Ptr = std::shared_ptr<TransferTimer>;

    TransferTimer(asio::io_context& io, std::chrono::milliseconds timeout)
        : timer_(io), timeout_(timeout) {}

    void set_on_timeout(std::function<void()> cb) { onTimeout_ = std::move(cb); }

    void arm() {
        if (disarmed_) return;
        timer_.expires_after(timeout_);
        auto self = shared_from_this();
        timer_.async_wait([self](std::error_code ec) {
            if (ec == asio::error::operation_aborted) return;
            if (!ec && self->onTimeout_) self->onTimeout_();
        });
    }

    void disarm() {
        disarmed_ = true;
        std::error_code ec;
        timer_.cancel(ec);
    }

private:
    asio::steady_timer timer_;
    std::chrono::milliseconds timeout_;
    std::function<void()> onTimeout_;
    bool disarmed_ = false;
};

} // namespace filexferlib::detail
