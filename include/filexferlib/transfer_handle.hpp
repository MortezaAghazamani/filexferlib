#pragma once
#include "filexferlib/detail/common.hpp"
#include "filexferlib/types.hpp"

#include <memory>
#include <atomic>
#include <functional>
#include <cstdint>

namespace filexferlib {

struct TransferControl {
    std::atomic<bool> paused{false};
    std::atomic<bool> cancelled{false};
    std::atomic<std::uint64_t> bytesTransferred{0};
    std::atomic<std::uint64_t> totalBytes{0};
    std::atomic<int> state{0};

    // Called on pause/resume/cancel to poke the state machine on io thread.
    std::function<void()> resumeCallback;
};

class TransferHandle {
public:
    enum class State : int {
        Running = 0,
        Paused  = 1,
        Cancelled = 2,
        Done    = 3,
        Failed  = 4,
    };

    TransferHandle() = default;
    explicit TransferHandle(std::shared_ptr<TransferControl> c)
        : ctrl_(std::move(c)) {}

    void pause() {
        if (!ctrl_) return;
        ctrl_->paused.store(true);
        ctrl_->state.store(static_cast<int>(State::Paused));
    }

    void resume() {
        if (!ctrl_) return;
        ctrl_->paused.store(false);
        ctrl_->state.store(static_cast<int>(State::Running));
        if (ctrl_->resumeCallback) ctrl_->resumeCallback();
    }

    void cancel() {
        if (!ctrl_) return;
        ctrl_->cancelled.store(true);
        if (ctrl_->resumeCallback) ctrl_->resumeCallback();
    }

    State state() const {
        if (!ctrl_) return State::Done;
        return static_cast<State>(ctrl_->state.load());
    }

    bool valid() const { return static_cast<bool>(ctrl_); }

    std::uint64_t bytesTransferred() const {
        return ctrl_ ? ctrl_->bytesTransferred.load() : 0;
    }
    std::uint64_t totalBytes() const {
        return ctrl_ ? ctrl_->totalBytes.load() : 0;
    }

private:
    std::shared_ptr<TransferControl> ctrl_;
};

} // namespace filexferlib
