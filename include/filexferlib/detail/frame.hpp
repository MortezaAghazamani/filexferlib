#pragma once
#include "filexferlib/detail/common.hpp"
#include "filexferlib/detail/protocol.hpp"
#include "filexferlib/types.hpp"

#include <system_error>
#include <vector>

namespace filexferlib::detail {

struct Frame {
    MsgType type = MsgType::Error;
    ByteBuffer payload;
};

void encode_frame(MsgType type, const ByteBuffer& payload, ByteBuffer& out);

enum class DecodeResult { Ok, NeedMore, Error };

DecodeResult try_decode_frame(const ByteBuffer& buf,
                              std::size_t offset,
                              Frame& out,
                              std::size_t& consumed,
                              std::error_code& ec);

} // namespace filexferlib::detail