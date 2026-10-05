#include "filexferlib/detail/frame.hpp"
#include "filexferlib/error.hpp"
#include <cstring>

namespace filexferlib::detail {

void encode_frame(MsgType type, const ByteBuffer& payload, ByteBuffer& out) {
    const std::size_t base = out.size();
    out.resize(base + kHeaderSize + payload.size());

    byte* p = out.data() + base;
    put_u16(p + 0, kMagic);
    p[2] = byte(kProtocolVersion);
    p[3] = byte(static_cast<u8>(type));
    put_u32(p + 4, static_cast<u32>(payload.size()));

    if (!payload.empty())
        std::memcpy(p + kHeaderSize, payload.data(), payload.size());
}

DecodeResult try_decode_frame(const ByteBuffer& buf,
                              std::size_t offset,
                              Frame& out,
                              std::size_t& consumed,
                              std::error_code& ec) {
    consumed = 0;
    ec.clear();

    if (buf.size() < offset) return DecodeResult::Error;
    if (buf.size() - offset < kHeaderSize) return DecodeResult::NeedMore;

    const byte* p = buf.data() + offset;

    if (get_u16(p) != kMagic) {
        ec = make_error_code(ErrorCode::ProtocolError);
        return DecodeResult::Error;
    }

    const u8 ver = std::to_integer<u8>(p[2]);
    if (ver != kProtocolVersion) {
        ec = make_error_code(ErrorCode::VersionMismatch);
        return DecodeResult::Error;
    }

    const auto type = static_cast<MsgType>(std::to_integer<u8>(p[3]));
    const u32 len = get_u32(p + 4);

    if (buf.size() - offset - kHeaderSize < len)
        return DecodeResult::NeedMore;

    out.type = type;
    out.payload.assign(buf.begin() + offset + kHeaderSize,
                       buf.begin() + offset + kHeaderSize + len);
    consumed = kHeaderSize + len;
    return DecodeResult::Ok;
}

} // namespace filexferlib::detail