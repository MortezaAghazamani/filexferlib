#include <filexferlib/detail/frame.hpp>
#include <iostream>

using namespace filexferlib;
using namespace filexferlib::detail;

static int failures = 0;
#define CHECK(x) do { if(!(x)) { std::cerr << "FAIL: " #x " at line " << __LINE__ << "\n"; ++failures; } } while(0)

int main() {
    // roundtrip
    {
        ByteBuffer payload = {std::byte{1}, std::byte{2}, std::byte{3}};
        ByteBuffer out;
        encode_frame(MsgType::GenericReq, payload, out);
        CHECK(out.size() == kHeaderSize + 3);

        Frame f;
        std::size_t consumed = 0;
        std::error_code ec;
        auto r = try_decode_frame(out, 0, f, consumed, ec);
        CHECK(r == DecodeResult::Ok);
        CHECK(consumed == out.size());
        CHECK(f.type == MsgType::GenericReq);
        CHECK(f.payload == payload);
    }

    // empty payload
    {
        ByteBuffer out;
        encode_frame(MsgType::Hello, {}, out);
        Frame f; std::size_t consumed = 0; std::error_code ec;
        auto r = try_decode_frame(out, 0, f, consumed, ec);
        CHECK(r == DecodeResult::Ok);
    }

    // need more
    {
        ByteBuffer tiny = {std::byte{0xF1}, std::byte{0xEE}};
        Frame f; std::size_t consumed = 0; std::error_code ec;
        auto r = try_decode_frame(tiny, 0, f, consumed, ec);
        CHECK(r == DecodeResult::NeedMore);
    }

    // bad magic
    {
        ByteBuffer bad = {std::byte{0x00}, std::byte{0x00},
                          std::byte{0x01}, std::byte{0x01},
                          std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
        Frame f; std::size_t consumed = 0; std::error_code ec;
        auto r = try_decode_frame(bad, 0, f, consumed, ec);
        CHECK(r == DecodeResult::Error);
    }

    if (failures == 0) std::cout << "all tests passed\n";
    return failures ? 1 : 0;
}
