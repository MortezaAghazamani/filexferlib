#pragma once
#include "filexferlib/detail/common.hpp"
#include "filexferlib/detail/protocol.hpp"

#include <string>
#include <cstring>

namespace filexferlib::detail {

// =====================================================================
// Hello
// =====================================================================
// Hello payload (client -> server):
//   version(1) | tokenLen(2) | token
//
// HelloAck payload (server -> client):
//   version(1) | serverVersion(str)
//            | maxBytesPerSecond(8)
//            | recommendedTimeoutMs(4)
//            | recommendedChunkSize(4)

inline ByteBuffer encode_hello(u8 version, const std::string& token) {
    ByteBuffer b;
    const auto base = b.size();
    b.resize(base + 1);
    b[base] = byte(version);
    put_u16_into(b, static_cast<u16>(token.size()));
    if (!token.empty()) {
        const auto off = b.size();
        b.resize(off + token.size());
        std::memcpy(b.data() + off, token.data(), token.size());
    }
    return b;
}

inline ByteBuffer encode_hello_ack(const std::string& serverVersion,
                                   u64 maxBytesPerSecond,
                                   u32 recommendedTimeoutMs,
                                   u32 recommendedChunkSize) {
    ByteBuffer b;
    const auto base = b.size();
    b.resize(base + 1);
    b[base] = byte(kProtocolVersion);
    put_str(b, serverVersion);
    put_u64_into(b, maxBytesPerSecond);
    const auto base2 = b.size();
    b.resize(base2 + 8);
    put_u32(b.data() + base2, recommendedTimeoutMs);
    put_u32(b.data() + base2 + 4, recommendedChunkSize);
    return b;
}

struct HelloAckData {
    u8 version = 0;
    std::string serverVersion;
    u64 maxBytesPerSecond = 0;
    u32 recommendedTimeoutMs = 30000;
    u32 recommendedChunkSize = 0;
};

inline bool decode_hello_ack(const ByteBuffer& b, HelloAckData& out) {
    if (b.size() < 1) return false;
    out.version = std::to_integer<u8>(b[0]);
    std::size_t used = 0;
    out.serverVersion = get_str(b.data() + 1, b.size() - 1, used);
    if (used == 0) return false;
    std::size_t off = 1 + used;
    if (b.size() < off + 8) return false;
    out.maxBytesPerSecond = get_u64(b.data() + off);
    off += 8;
    if (b.size() < off + 8) return false;
    out.recommendedTimeoutMs = get_u32(b.data() + off);
    out.recommendedChunkSize = get_u32(b.data() + off + 4);
    return true;
}

// =====================================================================
// FileBegin
// =====================================================================
// FileBegin payload: transferId(4) | path(str) | size(8) | mtimeNs(8) | xxhash(8)
//
// The high bit of `xxhash` is the compression flag:
//   0 = raw data
//   1 = zstd-compressed data
// The remaining 63 bits are the actual XXHash3 of the *uncompressed*
// data.

constexpr u64 kCompressionFlag = (1ull << 63);

inline ByteBuffer encode_file_begin(u32 tid,
                                    const std::string& path,
                                    u64 size, i64 mtime, u64 hash,
                                    bool compressed = false) {
    ByteBuffer b;
    const auto base0 = b.size();
    b.resize(base0 + 4);
    put_u32(b.data() + base0, tid);
    put_str(b, path);
    const auto base = b.size();
    b.resize(base + 24);
    put_u64(b.data() + base + 0, size);
    put_i64(b.data() + base + 8, mtime);
    const u64 hashField = (hash & ~kCompressionFlag) |
                          (compressed ? kCompressionFlag : 0);
    put_u64(b.data() + base + 16, hashField);
    return b;
}

inline ByteBuffer encode_download_pull(u32 tid, const std::string& path,
                                       bool compressed = false) {
    return encode_file_begin(tid, path, 0, -1, 0, compressed);
}

struct FileBeginData {
    std::string path;
    u64 size = 0;
    i64 mtimeNs = 0;
    u64 xxhash = 0;
    bool is_pull = false;
    bool compressed = false;
};

inline bool decode_file_begin(const ByteBuffer& b, FileBeginData& out) {
    if (b.size() < 4) return false;
    std::size_t used = 0;
    out.path = get_str(b.data() + 4, b.size() - 4, used);
    if (used == 0) return false;
    const std::size_t off = 4 + used;
    if (b.size() < off + 24) return false;

    out.size    = get_u64(b.data() + off + 0);
    out.mtimeNs = get_i64(b.data() + off + 8);

    const u64 raw = get_u64(b.data() + off + 16);
    out.compressed = (raw & kCompressionFlag) != 0;
    out.xxhash     = raw & ~kCompressionFlag;

    out.is_pull = (out.mtimeNs == -1);
    return true;
}

// =====================================================================
// FileChunk
// =====================================================================
// payload: transferId(4) | offset(8) | data

inline ByteBuffer encode_file_chunk(u32 tid, u64 offset,
                                    const void* data, std::size_t len) {
    ByteBuffer b(4 + 8 + len);
    put_u32(b.data(), tid);
    put_u64(b.data() + 4, offset);
    if (len) std::memcpy(b.data() + 12, data, len);
    return b;
}

struct FileChunkData {
    u64 offset = 0;
    const byte* data = nullptr;
    std::size_t len = 0;
};

inline bool decode_file_chunk(const ByteBuffer& b, FileChunkData& out) {
    if (b.size() < 12) return false;
    out.offset = get_u64(b.data() + 4);
    out.data = b.data() + 12;
    out.len = b.size() - 12;
    return true;
}

// =====================================================================
// FileEnd
// =====================================================================
// payload: transferId(4) | xxhash(8)

inline ByteBuffer encode_file_end(u32 tid, u64 hash) {
    ByteBuffer b(12);
    put_u32(b.data(), tid);
    put_u64(b.data() + 4, hash);
    return b;
}

inline bool decode_file_end(const ByteBuffer& b, u64& hashOut) {
    if (b.size() < 12) return false;
    hashOut = get_u64(b.data() + 4);
    return true;
}

// =====================================================================
// FileAck
// =====================================================================
// payload: transferId(4) | ok(1) | xxhash(8) | msg(str)

inline ByteBuffer encode_file_ack(u32 tid, bool ok, u64 hash,
                                  const std::string& msg = {}) {
    ByteBuffer b;
    const auto base0 = b.size();
    b.resize(base0 + 4 + 1 + 8);
    put_u32(b.data() + base0, tid);
    b[base0 + 4] = byte(ok ? 1 : 0);
    put_u64(b.data() + base0 + 5, hash);
    if (!msg.empty()) put_str(b, msg);
    else { b.resize(b.size() + 2); put_u16(b.data() + b.size() - 2, 0); }
    return b;
}

struct FileAckData {
    bool ok = false;
    u64 hash = 0;
    std::string msg;
};

inline bool decode_file_ack(const ByteBuffer& b, FileAckData& out) {
    if (b.size() < 13) return false;
    out.ok = std::to_integer<u8>(b[4]) != 0;
    out.hash = get_u64(b.data() + 5);
    if (b.size() > 13) {
        std::size_t used = 0;
        out.msg = get_str(b.data() + 13, b.size() - 13, used);
    }
    return true;
}

// =====================================================================
// Upload resume
// =====================================================================

inline ByteBuffer encode_resume_req(u32 tid, const std::string& path) {
    ByteBuffer b;
    const auto base = b.size();
    b.resize(base + 4);
    put_u32(b.data() + base, tid);
    put_str(b, path);
    return b;
}

inline bool decode_resume_req(const ByteBuffer& b, std::string& path) {
    if (b.size() < 4) return false;
    std::size_t used = 0;
    path = get_str(b.data() + 4, b.size() - 4, used);
    return used > 0;
}

inline ByteBuffer encode_resume_resp(u32 tid, u64 offset, bool ok) {
    ByteBuffer b(13);
    put_u32(b.data(), tid);
    put_u64(b.data() + 4, offset);
    b[12] = byte(ok ? 1 : 0);
    return b;
}

struct ResumeRespData {
    u64 offset = 0;
    bool ok = false;
};

inline bool decode_resume_resp(const ByteBuffer& b, ResumeRespData& out) {
    if (b.size() < 13) return false;
    out.offset = get_u64(b.data() + 4);
    out.ok = std::to_integer<u8>(b[12]) != 0;
    return true;
}

// =====================================================================
// Download resume
// =====================================================================

inline ByteBuffer encode_download_resume_req(u32 tid,
                                             const std::string& path,
                                             u64 haveOffset) {
    ByteBuffer b;
    const auto base = b.size();
    b.resize(base + 4);
    put_u32(b.data() + base, tid);
    put_str(b, path);
    put_u64_into(b, haveOffset);
    return b;
}

struct DownloadResumeReqData {
    std::string path;
    u64 haveOffset = 0;
};

inline bool decode_download_resume_req(const ByteBuffer& b,
                                       DownloadResumeReqData& out) {
    if (b.size() < 4) return false;
    std::size_t used = 0;
    out.path = get_str(b.data() + 4, b.size() - 4, used);
    if (used == 0) return false;
    const std::size_t off = 4 + used;
    if (b.size() < off + 8) return false;
    out.haveOffset = get_u64(b.data() + off);
    return true;
}

struct DownloadResumeRespData {
    u64 serverOffset = 0;
    u64 totalSize = 0;
    u64 xxhash = 0;
    i64 mtimeNs = 0;
    bool ok = false;
    bool compressed = false;
};

inline ByteBuffer encode_download_resume_resp(u32 tid,
                                              u64 serverOffset,
                                              u64 totalSize,
                                              u64 xxhash,
                                              i64 mtimeNs,
                                              bool ok,
                                              bool compressed = false) {
    ByteBuffer b(4 + 8 + 8 + 8 + 8 + 1 + 1);
    put_u32(b.data(), tid);
    put_u64(b.data() + 4, serverOffset);
    put_u64(b.data() + 12, totalSize);
    put_u64(b.data() + 20, xxhash);
    put_i64(b.data() + 28, mtimeNs);
    b[36] = byte(ok ? 1 : 0);
    b[37] = byte(compressed ? 1 : 0);
    return b;
}

inline bool decode_download_resume_resp(const ByteBuffer& b,
                                        DownloadResumeRespData& out) {
    if (b.size() < 37) return false;
    out.serverOffset = get_u64(b.data() + 4);
    out.totalSize    = get_u64(b.data() + 12);
    out.xxhash       = get_u64(b.data() + 20);
    out.mtimeNs      = get_i64(b.data() + 28);
    out.ok           = std::to_integer<u8>(b[36]) != 0;
    out.compressed   = (b.size() > 37)
                        ? (std::to_integer<u8>(b[37]) != 0)
                        : false;
    return true;
}

// ---------- Rename ----------
// RenameReq payload:  oldPath(str) | newPath(str)
// RenameAck payload:  ok(1) | msg(str)

inline ByteBuffer encode_rename_req(const std::string& oldPath,
                                    const std::string& newPath) {
    ByteBuffer b;
    put_str(b, oldPath);
    put_str(b, newPath);
    return b;
}

struct RenameReqData {
    std::string oldPath;
    std::string newPath;
};

inline bool decode_rename_req(const ByteBuffer& b, RenameReqData& out) {
    std::size_t used1 = 0;
    out.oldPath = get_str(b.data(), b.size(), used1);
    if (used1 == 0) return false;
    std::size_t used2 = 0;
    out.newPath = get_str(b.data() + used1, b.size() - used1, used2);
    return used2 > 0;
}

inline ByteBuffer encode_rename_ack(bool ok, const std::string& msg) {
    ByteBuffer b;
    b.push_back(byte(ok ? 1 : 0));
    put_str(b, msg);
    return b;
}

struct RenameAckData {
    bool ok = false;
    std::string msg;
};

inline bool decode_rename_ack(const ByteBuffer& b, RenameAckData& out) {
    if (b.size() < 1) return false;
    out.ok = std::to_integer<u8>(b[0]) != 0;
    std::size_t used = 0;
    out.msg = get_str(b.data() + 1, b.size() - 1, used);
    return true;
}

} // namespace filexferlib::detail
