#include "filexferlib/detail/hash.hpp"
#include <xxhash.h>
#include <array>
#include <sstream>
#include <iomanip>
#include <fstream>

namespace filexferlib::detail {

struct FileHasher::Impl {
    XXH3_state_t* state = nullptr;
    Impl() {
        state = XXH3_createState();
        XXH3_64bits_reset(state);
    }
    ~Impl() { if (state) XXH3_freeState(state); }
};

FileHasher::FileHasher() : impl_(new Impl) {}
FileHasher::~FileHasher() { delete impl_; }
void FileHasher::reset() { XXH3_64bits_reset(impl_->state); }
void FileHasher::update(const void* data, std::size_t len) {
    XXH3_64bits_update(impl_->state, data, len);
}
std::uint64_t FileHasher::digest() const {
    return XXH3_64bits_digest(impl_->state);
}

std::uint64_t xxhash_file(const std::filesystem::path& path,
                          std::error_code& ec) {
    ec.clear();
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        ec = std::make_error_code(std::errc::no_such_file_or_directory);
        return 0;
    }
    FileHasher h;
    std::array<char, 256 * 1024> buf{};
    while (f) {
        f.read(buf.data(), buf.size());
        const auto n = f.gcount();
        if (n > 0) h.update(buf.data(), static_cast<std::size_t>(n));
    }
    if (!f.eof()) {
        ec = std::make_error_code(std::errc::io_error);
        return 0;
    }
    return h.digest();
}

std::string hash_to_hex(std::uint64_t h) {
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << h;
    return oss.str();
}

} // namespace filexferlib::detail