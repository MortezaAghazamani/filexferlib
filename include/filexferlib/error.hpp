#pragma once
#include <system_error>
#include <string>

namespace filexferlib {

enum class ErrorCode {
    Ok = 0,
    NetworkError,
    ProtocolError,
    VersionMismatch,
    PayloadTooLarge,
    MethodNotFound,
    PathOutsideRoot,
    PathNotFound,
    PermissionDenied,
    Cancelled,
    Timeout,
    InternalError,
    HashMismatch,
    AlreadyExists,
};

const std::error_category& filexferlib_category();

inline std::error_code make_error_code(ErrorCode e) {
    return {static_cast<int>(e), filexferlib_category()};
}

} // namespace filexferlib

namespace std {
template <> struct is_error_code_enum<filexferlib::ErrorCode> : true_type {};
}
