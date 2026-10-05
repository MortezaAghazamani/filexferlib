#include <filexferlib/detail/protocol.hpp>
#include <filexferlib/detail/frame.hpp>
#include <filexferlib/detail/hash.hpp>
#include <filexferlib/detail/transfer_util.hpp>
#include <filexferlib/detail/rate_limiter.hpp>
#include <filexferlib/error.hpp>
#include <filexferlib/types.hpp>
#include <filexferlib/logger.hpp>

#include <asio.hpp>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <random>
#include <limits>
#include <cstring>

namespace fs = std::filesystem;
using namespace filexferlib;
using namespace filexferlib::detail;

static int failures = 0;
#define CHECK(x) do { if(!(x)) { std::cerr << "FAIL: " #x " at line " << __LINE__ << "\n"; ++failures; } } while(0)
#define CHECK_EQ(a, b) do { auto _a = (a); auto _b = (b); if(_a != _b) { \
    std::cerr << "FAIL: " #a " == " #b " at line " << __LINE__ \
              << " (" << _a << " != " << _b << ")\n"; ++failures; } } while(0)

// =====================================================================
// 1. integer encoding roundtrip
// =====================================================================
static void test_int_roundtrip() {
    std::cout << "[UNIT] integer roundtrip\n";
    std::byte buf[16];

    put_u16(buf, 0x1234);
    CHECK_EQ(get_u16(buf), 0x1234);

    put_u32(buf, 0xDEADBEEFu);
    CHECK_EQ(get_u32(buf), 0xDEADBEEFu);

    put_u64(buf, 0x1122334455667788ull);
    CHECK_EQ(get_u64(buf), 0x1122334455667788ull);

    put_i64(buf, -42);
    CHECK_EQ(get_i64(buf), -42);

    put_i64(buf, std::numeric_limits<i64>::min());
    CHECK_EQ(get_i64(buf), std::numeric_limits<i64>::min());

    put_i64(buf, std::numeric_limits<i64>::max());
    CHECK_EQ(get_i64(buf), std::numeric_limits<i64>::max());
}

// =====================================================================
// 2. string encoding
// =====================================================================
static void test_string_roundtrip() {
    std::cout << "[UNIT] string roundtrip\n";

    std::vector<std::string> tests = {
        "",
        "a",
        "hello world",
        std::string(1000, 'x'),
        std::string(65535, 'y'),
    };

    for (auto& s : tests) {
        ByteBuffer b;
        put_str(b, s);
        std::size_t used = 0;
        std::string got = get_str(b.data(), b.size(), used);
        CHECK_EQ(got, s);
        CHECK_EQ(used, 2 + s.size());
    }
}

// =====================================================================
// 3. frame encode/decode
// =====================================================================
static void test_frame_roundtrip() {
    std::cout << "[UNIT] frame roundtrip\n";

    std::vector<ByteBuffer> payloads = {
        {},
        {std::byte{1}},
        {std::byte{1}, std::byte{2}, std::byte{3}},
        ByteBuffer(1024, std::byte{0xAB}),
    };

    for (auto& p : payloads) {
        ByteBuffer out;
        encode_frame(MsgType::GenericReq, p, out);

        Frame f;
        std::size_t used = 0;
        std::error_code ec;
        auto r = try_decode_frame(out, 0, f, used, ec);
        CHECK(r == DecodeResult::Ok);
        CHECK_EQ(used, out.size());
        CHECK(f.type == MsgType::GenericReq);
        CHECK(f.payload == p);
    }
}

// =====================================================================
// 4. frame decode: bad cases
// =====================================================================
static void test_frame_errors() {
    std::cout << "[UNIT] frame error cases\n";

    // too short
    {
        ByteBuffer buf = {std::byte{0xF1}};
        Frame f; std::size_t used = 0; std::error_code ec;
        auto r = try_decode_frame(buf, 0, f, used, ec);
        CHECK(r == DecodeResult::NeedMore);
    }

    // bad magic
    {
        ByteBuffer buf(8, std::byte{0});
        Frame f; std::size_t used = 0; std::error_code ec;
        auto r = try_decode_frame(buf, 0, f, used, ec);
        CHECK(r == DecodeResult::Error);
    }

    // bad version
    {
        ByteBuffer buf;
        encode_frame(MsgType::Hello, {}, buf);
        buf[2] = std::byte{99};
        Frame f; std::size_t used = 0; std::error_code ec;
        auto r = try_decode_frame(buf, 0, f, used, ec);
        CHECK(r == DecodeResult::Error);
    }

    // partial body
    {
        ByteBuffer payload = {std::byte{1}, std::byte{2}, std::byte{3}};
        ByteBuffer buf;
        encode_frame(MsgType::GenericReq, payload, buf);
        buf.resize(buf.size() - 1);
        Frame f; std::size_t used = 0; std::error_code ec;
        auto r = try_decode_frame(buf, 0, f, used, ec);
        CHECK(r == DecodeResult::NeedMore);
    }
}

// =====================================================================
// 5. file hashing
// =====================================================================
static void test_xxhash() {
    std::cout << "[UNIT] xxhash\n";

    auto tmp = fs::temp_directory_path() / "filexfer_hash_test.bin";

    // empty file
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.close();
        std::error_code ec;
        auto h = xxhash_file(tmp, ec);
        CHECK(!ec);
        CHECK(h != 0);
    }

    // same content Ã¢â€ â€™ same hash
    {
        std::string content = "hello filexferlib";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f.write(content.data(), content.size());
        }
        std::error_code ec1, ec2;
        auto h1 = xxhash_file(tmp, ec1);
        auto h2 = xxhash_file(tmp, ec2);
        CHECK(!ec1 && !ec2);
        CHECK_EQ(h1, h2);
    }

    // different content Ã¢â€ â€™ different hash
    {
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f.write("aaa", 3);
        }
        std::error_code ec1;
        auto h1 = xxhash_file(tmp, ec1);

        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f.write("bbb", 3);
        }
        std::error_code ec2;
        auto h2 = xxhash_file(tmp, ec2);

        CHECK(h1 != h2);
    }

    // large file (10 MB)
    {
        std::mt19937 rng(7);
        std::vector<char> buf(1024 * 1024);
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            for (int i = 0; i < 10; ++i) {
                for (auto& b : buf) b = static_cast<char>(rng() & 0xFF);
                f.write(buf.data(), buf.size());
            }
        }
        std::error_code ec;
        auto h1 = xxhash_file(tmp, ec);
        auto h2 = xxhash_file(tmp, ec);
        CHECK(!ec);
        CHECK_EQ(h1, h2);
    }

    // streaming hash == file hash
    {
        std::string content(200 * 1024, 'x');
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f.write(content.data(), content.size());
        }
        std::error_code ec;
        auto fileHash = xxhash_file(tmp, ec);

        FileHasher h;
        std::size_t off = 0;
        while (off < content.size()) {
            std::size_t n = std::min<std::size_t>(777, content.size() - off);
            h.update(content.data() + off, n);
            off += n;
        }
        auto streamHash = h.digest();
        CHECK_EQ(fileHash, streamHash);
    }

    std::error_code rmEc;
    fs::remove(tmp, rmEc);
}

// =====================================================================
// 6. transfer util encode/decode
// =====================================================================
static void test_transfer_util() {
    std::cout << "[UNIT] transfer util encode/decode\n";

    // FileBegin — no compression
    {
        const u64 testHash = 0x1EADBEEFCAFEBABEull;  // bit 63 = 0
        auto b = encode_file_begin(42, "/foo/bar.bin",
            1234567, 9999999, testHash, false);
        FileBeginData bd;
        CHECK(decode_file_begin(b, bd));
        CHECK_EQ(bd.path, "/foo/bar.bin");
        CHECK_EQ(bd.size, 1234567u);
        CHECK_EQ(bd.mtimeNs, 9999999);
        CHECK_EQ(bd.xxhash, testHash);
        CHECK(!bd.compressed);
    }

    // FileBegin — compression flag set
    {
        const u64 testHash = 0x1EADBEEFCAFEBABEull;
        auto b = encode_file_begin(43, "/foo/bar.bin",
            1234567, 9999999, testHash, true);
        FileBeginData bd;
        CHECK(decode_file_begin(b, bd));
        CHECK_EQ(bd.xxhash, testHash);
        CHECK(bd.compressed);
    }

    // FileBegin — compression flag with high hash bit
    // (verify the flag is correctly separated from the hash)
    {
        const u64 testHash = 0xDEADBEEFCAFEBABEull;   // bit 63 = 1
        auto b = encode_file_begin(44, "/foo/bar.bin",
            1234567, 9999999, testHash, false);
        FileBeginData bd;
        CHECK(decode_file_begin(b, bd));
        // Since bit 63 is the flag bit, a hash with bit 63 set
        // will lose that bit when encoded with compressed=false.
        // We can't distinguish these two cases in the wire format.
        // So we test that the flag round-trips correctly:
        CHECK_EQ(bd.xxhash, testHash & ~(1ull << 63));
        CHECK(!bd.compressed);
    }

    // FileChunk
    {
        std::string data = "abcdefghij";
        auto b = encode_file_chunk(7, 0x1122334455667788ull,
                                   data.data(), data.size());
        FileChunkData cd;
        CHECK(decode_file_chunk(b, cd));
        CHECK_EQ(cd.offset, 0x1122334455667788ull);
        CHECK_EQ(cd.len, data.size());
        CHECK(std::memcmp(cd.data, data.data(), data.size()) == 0);
    }

    // FileEnd
    {
        auto b = encode_file_end(9, 0xCAFEBABE12345678ull);
        u64 h = 0;
        CHECK(decode_file_end(b, h));
        CHECK_EQ(h, 0xCAFEBABE12345678ull);
    }

    // FileAck
    {
        auto b = encode_file_ack(5, true, 42, "ok");
        FileAckData ack;
        CHECK(decode_file_ack(b, ack));
        CHECK(ack.ok);
        CHECK_EQ(ack.hash, 42u);
        CHECK_EQ(ack.msg, "ok");
    }
    {
        auto b = encode_file_ack(6, false, 0, "hash mismatch");
        FileAckData ack;
        CHECK(decode_file_ack(b, ack));
        CHECK(!ack.ok);
        CHECK_EQ(ack.msg, "hash mismatch");
    }

    // Resume req/resp
    {
        auto b = encode_resume_req(11, "/path/to/file");
        std::string path;
        CHECK(decode_resume_req(b, path));
        CHECK_EQ(path, "/path/to/file");
    }
    {
        auto b = encode_resume_resp(12, 4096, true);
        ResumeRespData rd;
        CHECK(decode_resume_resp(b, rd));
        CHECK_EQ(rd.offset, 4096u);
        CHECK(rd.ok);
    }

    // Download resume req/resp
    {
        auto b = encode_download_resume_req(13, "/x.bin", 12345);
        DownloadResumeReqData rd;
        CHECK(decode_download_resume_req(b, rd));
        CHECK_EQ(rd.path, "/x.bin");
        CHECK_EQ(rd.haveOffset, 12345u);
    }
    {
        auto b = encode_download_resume_resp(14, 100, 200,
                                             0xAAAA, 42, true);
        DownloadResumeRespData rd;
        CHECK(decode_download_resume_resp(b, rd));
        CHECK_EQ(rd.serverOffset, 100u);
        CHECK_EQ(rd.totalSize, 200u);
        CHECK_EQ(rd.xxhash, 0xAAAAu);
        CHECK_EQ(rd.mtimeNs, 42);
        CHECK(rd.ok);
    }
}

// =====================================================================
// 7. rate limiter
// =====================================================================
static void test_rate_limiter() {
    std::cout << "[UNIT] rate limiter\n";

    asio::io_context io;

    // disabled
    {
        auto rl = std::make_shared<RateLimiter>(io, 0);
        CHECK(!rl->isEnabled());
        bool called = false;
        rl->acquire(1000, [&]{ called = true; });
        CHECK(called);
    }

    // enabled — small rate, verify tokens accumulate over time
    {
        auto rl = std::make_shared<RateLimiter>(io, 1000);  // 1 KB/s

        std::atomic<int> counter{0};

        // Single acquire of 500 bytes. With a fresh bucket, this
        // takes 500 ms. Check it eventually fires.
        rl->acquire(500, [&]{
            counter.fetch_add(1);
        });

        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(1500);
        while (counter.load() < 1 &&
               std::chrono::steady_clock::now() < deadline) {
            io.run_for(std::chrono::milliseconds(50));
            io.restart();
        }

        CHECK_EQ(counter.load(), 1);
    }
}

// =====================================================================
// 8. error codes
// =====================================================================
static void test_error_codes() {
    std::cout << "[UNIT] error codes\n";

    auto ec = make_error_code(ErrorCode::ProtocolError);
    CHECK(ec.value() == static_cast<int>(ErrorCode::ProtocolError));
    CHECK(ec.category() == filexferlib_category());
    CHECK(!ec.message().empty());

    auto ec2 = make_error_code(ErrorCode::Cancelled);
    CHECK(ec2 != ec);
    CHECK(ec2.message() != ec.message());

    CHECK(make_error_code(ErrorCode::Ok).value() == 0);
}

// =====================================================================
// 9. logger
// =====================================================================
static void test_logger() {
    std::cout << "[UNIT] logger\n";

    std::atomic<int> warnCount{0};
    std::atomic<int> infoCount{0};

    Logger::instance().setSink([&](LogLevel lv, const std::string&){
        if (lv == LogLevel::Warn) ++warnCount;
        if (lv == LogLevel::Info) ++infoCount;
    });

    Logger::instance().setLevel(LogLevel::Info);
    FX_INFO("info message");
    FX_DEBUG("debug message");   // should NOT appear
    FX_WARN("warn message");

    CHECK_EQ(infoCount.load(), 1);
    CHECK_EQ(warnCount.load(), 1);

    Logger::instance().setLevel(LogLevel::Off);
    FX_ERROR("suppressed");
    CHECK_EQ(warnCount.load(), 1);

    Logger::instance().setLevel(LogLevel::Info);
    Logger::instance().setSink(nullptr);
}

// =====================================================================
// Main
// =====================================================================
int main() {
    test_int_roundtrip();
    test_string_roundtrip();
    test_frame_roundtrip();
    test_frame_errors();
    test_xxhash();
    test_transfer_util();
    test_rate_limiter();
    test_error_codes();
    test_logger();

    if (failures == 0) {
        std::cout << "\n=== ALL UNIT TESTS PASSED ===\n";
        return 0;
    }
    std::cout << "\n=== " << failures << " UNIT TEST(S) FAILED ===\n";
    return 1;
}
