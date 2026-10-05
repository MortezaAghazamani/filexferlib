#pragma once
#include "filexferlib/detail/common.hpp"
#include "filexferlib/types.hpp"

#include <vector>
#include <cstddef>

namespace filexferlib::detail {

// Compress `data` with zstd at `level`.
// Returns a ByteBuffer containing the compressed bytes.
// On error, returns an empty buffer.
ByteBuffer zstd_compress(const void* data, std::size_t len, int level);

// Decompress `data` into a buffer of at most `maxLen` bytes.
// On error or if the decompressed size exceeds `maxLen`, returns
// an empty buffer.
ByteBuffer zstd_decompress(const void* data, std::size_t len,
                           std::size_t maxLen);

// Check whether the given bytes look like a zstd frame.
bool zstd_is_frame(const void* data, std::size_t len);

} // namespace filexferlib::detail
