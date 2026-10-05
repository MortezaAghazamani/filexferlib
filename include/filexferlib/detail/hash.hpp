#pragma once
#include "filexferlib/detail/common.hpp"
#include <string>
#include <filesystem>
#include <system_error>

namespace filexferlib::detail {

std::uint64_t xxhash_file(const std::filesystem::path& path,
                          std::error_code& ec);

class FileHasher {
public:
    FileHasher();
    ~FileHasher();
    FileHasher(const FileHasher&) = delete;
    FileHasher& operator=(const FileHasher&) = delete;

    void reset();
    void update(const void* data, std::size_t len);
    std::uint64_t digest() const;

private:
    struct Impl;
    Impl* impl_;
};

std::string hash_to_hex(std::uint64_t h);

} // namespace filexferlib::detail