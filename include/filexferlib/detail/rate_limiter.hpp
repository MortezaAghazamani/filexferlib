#pragma once
#include "filexferlib/detail/common.hpp"

#include <asio.hpp>
#include <chrono>
#include <memory>
#include <functional>
#include <mutex>
#include <vector>

namespace filexferlib::detail {

// Token bucket rate limiter.
//
// The bucket starts empty so the very first transfer does not burst.
// Tokens accumulate at `rate_` per second, up to a maximum of one
// second's worth (to allow small bursts without going over budget).
class RateLimiter : public std::enable_shared_from_this<RateLimiter> {
public:
    using Ptr = std::shared_ptr<RateLimiter>;

    // rateBytesPerSec == 0 means unlimited.
    RateLimiter(asio::io_context& io, u64 rateBytesPerSec)
        : timer_(io)
        , rate_(rateBytesPerSec)
        , tokens_(0)   // start empty (no initial burst)
    {
        lastRefill_ = std::chrono::steady_clock::now();
    }

    bool isEnabled() const { return rate_ > 0; }
    u64 rate() const { return rate_; }

    void acquire(u64 bytes, std::function<void()> cb) {
        if (rate_ == 0) { cb(); return; }

        std::lock_guard<std::mutex> lk(mu_);
        refill_locked();
        waiters_.push_back({bytes, std::move(cb)});
        drain_locked();
    }

    void cancel_all() {
        std::vector<Waiter> ws;
        {
            std::lock_guard<std::mutex> lk(mu_);
            ws.swap(waiters_);
            std::error_code ec;
            timer_.cancel(ec);
        }
    }

private:
    struct Waiter {
        u64 bytes;
        std::function<void()> cb;
    };

    void refill_locked() {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            now - lastRefill_).count();
        if (elapsed <= 0) return;

        const u64 add = static_cast<u64>(
            static_cast<double>(rate_) * static_cast<double>(elapsed) / 1'000'000.0);
        if (add > 0) {
            tokens_ += add;
            // cap at 1 second worth of tokens
            if (tokens_ > rate_) tokens_ = rate_;
            lastRefill_ = now;
        }
    }

    void drain_locked() {
        while (!waiters_.empty()) {
            auto& w = waiters_.front();
            if (w.bytes <= tokens_) {
                tokens_ -= w.bytes;
                auto cb = std::move(w.cb);
                waiters_.erase(waiters_.begin());
                asio::post(timer_.get_executor(), [cb]{ cb(); });
            } else {
                schedule_refill_locked();
                return;
            }
        }
    }

    void schedule_refill_locked() {
        if (waiters_.empty()) return;
        timer_.expires_after(std::chrono::milliseconds(10));
        auto self = shared_from_this();
        timer_.async_wait([self](std::error_code ec) {
            if (ec) return;
            std::lock_guard<std::mutex> lk(self->mu_);
            self->refill_locked();
            self->drain_locked();
        });
    }

    asio::steady_timer timer_;
    u64 rate_ = 0;
    u64 tokens_ = 0;
    std::chrono::steady_clock::time_point lastRefill_;
    std::mutex mu_;
    std::vector<Waiter> waiters_;
};

} // namespace filexferlib::detail
