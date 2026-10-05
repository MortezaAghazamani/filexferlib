#include <filexferlib/client.hpp>
#include <filexferlib/server.hpp>
#include <filexferlib/logger.hpp>
#include <filexferlib/detail/hash.hpp>

#include <iostream>
#include <fstream>
#include <filesystem>
#include <cstring>
#include <thread>
#include <atomic>

namespace fs = std::filesystem;
using namespace filexferlib;

static int failures = 0;
#define CHECK(x) do { if(!(x)) { std::cerr << "FAIL: " #x " at line " << __LINE__ << "\n"; ++failures; } } while(0)

static void write_file(const fs::path& p, const std::string& c) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(c.data(), c.size());
}

static std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

// =====================================================================
// Test 1: empty file
// =====================================================================
static void test_empty_file(Client& client, const fs::path& serverRoot,
                            const fs::path& clientSrc) {
    std::cout << "\n[EDGE-1] empty file\n";
    auto lf = clientSrc / "empty.bin";
    write_file(lf, "");

    auto r = client.upload(lf.string(), "/empty.bin").get();
    CHECK(r.bytesTransferred == 0);
    CHECK(fs::exists(serverRoot / "empty.bin"));
    CHECK(fs::file_size(serverRoot / "empty.bin") == 0);

    auto dl = clientSrc / "empty_dl.bin";
    std::error_code ec;
    fs::remove(dl, ec);
    auto r2 = client.download("/empty.bin", dl.string()).get();
    CHECK(r2.bytesTransferred == 0);
    CHECK(fs::exists(dl));
    CHECK(fs::file_size(dl) == 0);
}

// =====================================================================
// Test 2: 1-byte file
// =====================================================================
static void test_one_byte(Client& client, const fs::path& serverRoot,
                          const fs::path& clientSrc) {
    std::cout << "\n[EDGE-2] 1-byte file\n";
    auto lf = clientSrc / "one.bin";
    write_file(lf, "X");

    auto r = client.upload(lf.string(), "/one.bin").get();
    CHECK(r.bytesTransferred == 1);
    CHECK(r.verified);
    CHECK(read_file(serverRoot / "one.bin") == "X");
}

// =====================================================================
// Test 3: file exactly chunkSize
// =====================================================================
static void test_exact_chunk_size(Client& client, const fs::path& serverRoot,
                                  const fs::path& clientSrc) {
    std::cout << "\n[EDGE-3] file exactly chunkSize\n";

    CopyOptions opts;
    opts.chunkSize = 4096;

    auto lf = clientSrc / "exact.bin";
    std::string content(4096, 'A');
    write_file(lf, content);

    auto r = client.upload(lf.string(), "/exact.bin", opts).get();
    CHECK(r.bytesTransferred == 4096);
    CHECK(r.verified);
    CHECK(read_file(serverRoot / "exact.bin") == content);
}

// =====================================================================
// Test 4: file just under chunkSize
// =====================================================================
static void test_just_under_chunk(Client& client, const fs::path& serverRoot,
                                  const fs::path& clientSrc) {
    std::cout << "\n[EDGE-4] file just under chunkSize\n";

    CopyOptions opts;
    opts.chunkSize = 4096;

    auto lf = clientSrc / "under.bin";
    std::string content(4095, 'B');
    write_file(lf, content);

    auto r = client.upload(lf.string(), "/under.bin", opts).get();
    CHECK(r.bytesTransferred == 4095);
    CHECK(r.verified);
    CHECK(read_file(serverRoot / "under.bin") == content);
}

// =====================================================================
// Test 5: file just over chunkSize
// =====================================================================
static void test_just_over_chunk(Client& client, const fs::path& serverRoot,
                                 const fs::path& clientSrc) {
    std::cout << "\n[EDGE-5] file just over chunkSize\n";

    CopyOptions opts;
    opts.chunkSize = 4096;

    auto lf = clientSrc / "over.bin";
    std::string content(4097, 'C');
    write_file(lf, content);

    auto r = client.upload(lf.string(), "/over.bin", opts).get();
    CHECK(r.bytesTransferred == 4097);
    CHECK(r.verified);
    CHECK(read_file(serverRoot / "over.bin") == content);
}

// =====================================================================
// Test 6: unicode filename
// =====================================================================
static void test_unicode_filename(Client& client, const fs::path& serverRoot,
                                  const fs::path& clientSrc) {
    std::cout << "\n[EDGE-6] unicode filename\n";

    auto lf = clientSrc / "unicode_\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85.bin";
    write_file(lf, "hello unicode");

    auto r = client.upload(lf.string(),
                           "/unicode_\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85.bin").get();
    CHECK(r.verified);
    CHECK(fs::exists(serverRoot /
                     "unicode_\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85.bin"));
}

// =====================================================================
// Test 7: deep directory tree
// =====================================================================
static void test_deep_tree(Client& client, const fs::path& serverRoot,
                           const fs::path& clientSrc) {
    std::cout << "\n[EDGE-7] deep directory tree\n";

    auto lf = clientSrc / "deep.bin";
    write_file(lf, "deep file");

    std::string remote = "/";
    for (int i = 0; i < 10; ++i) {
        remote += "level" + std::to_string(i) + "/";
    }
    remote += "deep.bin";

    auto r = client.upload(lf.string(), remote).get();
    CHECK(r.verified);

    fs::path expected = serverRoot;
    for (int i = 0; i < 10; ++i) {
        expected /= ("level" + std::to_string(i));
    }
    expected /= "deep.bin";
    CHECK(fs::exists(expected));
    CHECK(read_file(expected) == "deep file");
}

// =====================================================================
// Test 8: overwrite existing file
// =====================================================================
static void test_overwrite(Client& client, const fs::path& serverRoot,
                           const fs::path& clientSrc) {
    std::cout << "\n[EDGE-8] overwrite existing\n";

    // First upload
    auto lf = clientSrc / "overwrite.bin";
    write_file(lf, "first");
    client.upload(lf.string(), "/overwrite.bin").get();
    CHECK(read_file(serverRoot / "overwrite.bin") == "first");

    // Second upload, different content
    write_file(lf, "second");
    auto r = client.upload(lf.string(), "/overwrite.bin").get();
    CHECK(r.verified);
    CHECK(read_file(serverRoot / "overwrite.bin") == "second");
}

// =====================================================================
// Test 9: upload to non-existent parent (createDirs)
// =====================================================================
static void test_create_dirs(Client& client, const fs::path& serverRoot,
                             const fs::path& clientSrc) {
    std::cout << "\n[EDGE-9] create intermediate dirs\n";

    auto lf = clientSrc / "mkdirs.bin";
    write_file(lf, "content");

    CopyOptions opts;
    opts.createDirs = true;

    auto r = client.upload(lf.string(),
                           "/a/b/c/d/mkdirs.bin", opts).get();
    CHECK(r.verified);
    CHECK(fs::exists(serverRoot / "a" / "b" / "c" / "d" / "mkdirs.bin"));
}

// =====================================================================
// Test 10: download non-existent file Ã¢â€ â€™ error
// =====================================================================
static void test_download_missing(Client& client, const fs::path& clientSrc) {
    std::cout << "\n[EDGE-10] download missing file\n";

    auto dl = clientSrc / "missing_dl.bin";
    bool got_error = false;
    try {
        client.download("/does_not_exist.bin", dl.string()).get();
    } catch (const std::exception&) {
        got_error = true;
    }
    CHECK(got_error);
}

// =====================================================================
// Test 11: path traversal attempt Ã¢â€ â€™ error
// =====================================================================
static void test_path_traversal(Client& client, const fs::path& clientSrc) {
    std::cout << "\n[EDGE-11] path traversal rejected\n";

    auto lf = clientSrc / "traversal.bin";
    write_file(lf, "evil");

    bool got_error = false;
    try {
        client.upload(lf.string(), "/../../etc/passwd").get();
    } catch (const std::exception&) {
        got_error = true;
    }
    CHECK(got_error);
}

// =====================================================================
// Test 12: zero-length path Ã¢â€ â€™ error
// =====================================================================
static void test_empty_path(Client& client, const fs::path& clientSrc) {
    std::cout << "\n[EDGE-12] empty path rejected\n";

    auto lf = clientSrc / "empty_path.bin";
    write_file(lf, "content");

    bool got_error = false;
    try {
        client.upload(lf.string(), "").get();
    } catch (const std::exception&) {
        got_error = true;
    }
    CHECK(got_error);
}

// =====================================================================
// Main
// =====================================================================
int main() {
    Logger::instance().setLevel(LogLevel::Off);

    auto base = fs::temp_directory_path() / "filexfer_edge";
    fs::remove_all(base);
    auto serverRoot = base / "server";
    auto clientSrc  = base / "client";
    fs::create_directories(serverRoot);
    fs::create_directories(clientSrc);

    ServerConfig cfg;
    cfg.rootDir = serverRoot;
    Server server(cfg);
    server.registerHandler("echo", [](GenericContext& ctx) {
        ctx.respond(ctx.request());
    });
    auto port = server.start("127.0.0.1", 0);

    Client client;
    client.connect("127.0.0.1", port);

    test_empty_file(client, serverRoot, clientSrc);
    test_one_byte(client, serverRoot, clientSrc);
    test_exact_chunk_size(client, serverRoot, clientSrc);
    test_just_under_chunk(client, serverRoot, clientSrc);
    test_just_over_chunk(client, serverRoot, clientSrc);
    test_unicode_filename(client, serverRoot, clientSrc);
    test_deep_tree(client, serverRoot, clientSrc);
    test_overwrite(client, serverRoot, clientSrc);
    test_create_dirs(client, serverRoot, clientSrc);
    test_download_missing(client, clientSrc);
    test_path_traversal(client, clientSrc);
    test_empty_path(client, clientSrc);

    client.disconnect();
    server.stop();
    fs::remove_all(base);

    if (failures == 0) {
        std::cout << "\n=== ALL EDGE TESTS PASSED ===\n";
        return 0;
    }
    std::cout << "\n=== " << failures << " EDGE TEST(S) FAILED ===\n";
    return 1;
}
