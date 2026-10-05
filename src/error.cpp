#include "filexferlib/error.hpp"

namespace filexferlib {

namespace {
class FilexferCategory : public std::error_category {
public:
    const char* name() const noexcept override { return "filexferlib"; }
    std::string message(int ev) const override {
        switch (static_cast<ErrorCode>(ev)) {
        case ErrorCode::Ok:               return "ok";
        case ErrorCode::NetworkError:     return "network error";
        case ErrorCode::ProtocolError:    return "protocol error";
        case ErrorCode::VersionMismatch:  return "protocol version mismatch";
        case ErrorCode::PayloadTooLarge:  return "payload too large";
        case ErrorCode::MethodNotFound:   return "method not found";
        case ErrorCode::PathOutsideRoot:  return "path outside root";
        case ErrorCode::PathNotFound:     return "path not found";
        case ErrorCode::PermissionDenied: return "permission denied";
        case ErrorCode::Cancelled:        return "cancelled";
        case ErrorCode::Timeout:          return "timeout";
        case ErrorCode::InternalError:    return "internal error";
        case ErrorCode::HashMismatch:     return "hash mismatch";
        case ErrorCode::AlreadyExists:    return "already exists";
        }
        return "unknown";
    }
};
} // namespace

const std::error_category& filexferlib_category() {
    static FilexferCategory cat;
    return cat;
}

} // namespace filexferlib
