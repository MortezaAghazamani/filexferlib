#pragma once
#include "filexferlib/detail/common.hpp"
#include "filexferlib/types.hpp"
#include <cstring>
#include <string>

namespace filexferlib::detail {

constexpr u16   kMagic           = 0xF1EE;
constexpr u8    kProtocolVersion = 1;
constexpr std::size_t kHeaderSize = 8;

enum class MsgType : u8 {
    Hello          = 0x01,
    HelloAck       = 0x02,
    Error          = 0x50,
    Cancel         = 0xF0,

    FileBegin      = 0x10,
    FileChunk      = 0x11,
    FileEnd        = 0x12,
    FileAck        = 0x13,
    FileCancelReq  = 0x14,
    FilePause      = 0x15,
    FileResume     = 0x16,

    // resume probes
    FileResumeReq  = 0x17,
    FileResumeResp = 0x18,

    // download resume: client tells server "I already have N bytes"
    DownloadResumeReq  = 0x19,
    DownloadResumeResp = 0x1A,
    DownloadConfigReq  = 0x1B,

    ListReq        = 0x20,
    ListResp       = 0x21,
    StatReq        = 0x22,
    StatResp       = 0x23,
    MkdirReq       = 0x24,
    MkdirAck       = 0x25,

    DeleteReq      = 0x30,
    DeleteAck      = 0x31,

    RenameReq      = 0x32,
    RenameAck      = 0x33,

    SetMtimeReq    = 0x40,
    SetMtimeAck    = 0x41,

    RemoteCopyReq  = 0x60,
    RemoteCopyAck  = 0x61,
    RemoteSyncReq  = 0x64,
    RemoteSyncAck  = 0x65,

    GenericReq     = 0xE0,
    GenericResp    = 0xE1,
};

inline void put_u16(byte* p, u16 v) noexcept {
    p[0] = byte(v & 0xFF);
    p[1] = byte((v >> 8) & 0xFF);
}
inline void put_u32(byte* p, u32 v) noexcept {
    p[0] = byte(v & 0xFF);
    p[1] = byte((v >> 8) & 0xFF);
    p[2] = byte((v >> 16) & 0xFF);
    p[3] = byte((v >> 24) & 0xFF);
}
inline void put_u64(byte* p, u64 v) noexcept {
    for (int i = 0; i < 8; ++i) p[i] = byte((v >> (8*i)) & 0xFF);
}
inline void put_i64(byte* p, i64 v) noexcept { put_u64(p, static_cast<u64>(v)); }

inline u16 get_u16(const byte* p) noexcept {
    return u16(std::to_integer<u8>(p[0]))
         | (u16(std::to_integer<u8>(p[1])) << 8);
}
inline u32 get_u32(const byte* p) noexcept {
    return u32(std::to_integer<u8>(p[0]))
         | (u32(std::to_integer<u8>(p[1])) << 8)
         | (u32(std::to_integer<u8>(p[2])) << 16)
         | (u32(std::to_integer<u8>(p[3])) << 24);
}
inline u64 get_u64(const byte* p) noexcept {
    u64 v = 0;
    for (int i = 0; i < 8; ++i)
        v |= (u64(std::to_integer<u8>(p[i])) << (8*i));
    return v;
}
inline i64 get_i64(const byte* p) noexcept { return static_cast<i64>(get_u64(p)); }

inline void put_str(ByteBuffer& b, const std::string& s) {
    const auto base = b.size();
    b.resize(base + 2 + s.size());
    put_u16(b.data() + base, static_cast<u16>(s.size()));
    if (!s.empty()) std::memcpy(b.data() + base + 2, s.data(), s.size());
}

inline std::string get_str(const byte* p, std::size_t avail, std::size_t& used) {
    if (avail < 2) { used = 0; return {}; }
    const u16 n = get_u16(p);
    if (avail < 2 + n) { used = 0; return {}; }
    used = 2 + n;
    return std::string(reinterpret_cast<const char*>(p + 2), n);
}

inline void put_u16_into(ByteBuffer& b, u16 v) {
    const auto base = b.size();
    b.resize(base + 2);
    put_u16(b.data() + base, v);
}

inline void put_u64_into(ByteBuffer& b, u64 v) {
    const auto base = b.size();
    b.resize(base + 8);
    put_u64(b.data() + base, v);
}

} // namespace filexferlib::detail
