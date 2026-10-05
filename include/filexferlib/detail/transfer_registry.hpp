#pragma once
#include "filexferlib/detail/common.hpp"
#include "filexferlib/detail/frame.hpp"

#include <functional>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <atomic>

namespace filexferlib::detail {

// Per-session registry of active transfers.
// Each transfer has an id and a handler that receives File* frames.
class TransferRegistry {
public:
    using Handler = std::function<void(Frame)>;
    using Ptr = std::shared_ptr<TransferRegistry>;

    u32 alloc_id() { return next_id_.fetch_add(1); }

    void register_handler(u32 id, Handler h) {
        std::lock_guard<std::mutex> lk(mu_);
        handlers_[id] = std::move(h);
    }

    void unregister_handler(u32 id) {
        std::lock_guard<std::mutex> lk(mu_);
        handlers_.erase(id);
    }

    // Returns true if a handler was found and invoked.
    bool dispatch(Frame f, u32 id) {
        Handler h;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = handlers_.find(id);
            if (it != handlers_.end()) h = it->second;
        }
        if (!h) return false;
        h(std::move(f));
        return true;
    }

private:
    std::mutex mu_;
    std::unordered_map<u32, Handler> handlers_;
    std::atomic<u32> next_id_{1};
};

} // namespace filexferlib::detail
