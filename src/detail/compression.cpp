#include "filexferlib/detail/compression.hpp"
#include "filexferlib/logger.hpp"

#include <zstd.h>
#include <cstring>

namespace filexferlib::detail {

ByteBuffer zstd_compress(const void* data, std::size_t len, int level) {
    if (len == 0) {
        return ByteBuffer{};
    }

    const std::size_t bound = ZSTD_compressBound(len);
    ByteBuffer out(bound);

    const std::size_t written = ZSTD_compress(
        out.data(), out.size(),
        data, len,
        level);

    if (ZSTD_isError(written)) {
        FX_ERROR("zstd_compress failed: ", ZSTD_getErrorName(written));
        return ByteBuffer{};
    }

    out.resize(written);
    return out;
}

ByteBuffer zstd_decompress(const void* data, std::size_t len,
                           std::size_t maxLen) {
    if (len == 0) return ByteBuffer{};

    const unsigned long long origSize =
        ZSTD_getFrameContentSize(data, len);

    std::size_t targetSize;
    if (origSize == ZSTD_CONTENTSIZE_ERROR ||
        origSize == ZSTD_CONTENTSIZE_UNKNOWN)
    {
        targetSize = maxLen;
    } else {
        targetSize = static_cast<std::size_t>(origSize);
        if (targetSize > maxLen) {
            FX_ERROR("zstd_decompress: frame size ", targetSize,
                     " exceeds max ", maxLen);
            return ByteBuffer{};
        }
    }

    ByteBuffer out(targetSize);
    const std::size_t written = ZSTD_decompress(
        out.data(), out.size(),
        data, len);

    if (ZSTD_isError(written)) {
        FX_ERROR("zstd_decompress failed: ", ZSTD_getErrorName(written));
        return ByteBuffer{};
    }

    out.resize(written);
    return out;
}

bool zstd_is_frame(const void* data, std::size_t len) {
    if (len < 4) return false;
    const auto* p = static_cast<const unsigned char*>(data);
    return p[0] == 0x28 && p[1] == 0xB5 && p[2] == 0x2F && p[3] == 0xFD;
}

} // namespace filexferlib::detail
