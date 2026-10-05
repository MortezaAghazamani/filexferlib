#include <filexferlib/client.hpp>
#include <filexferlib/server.hpp>
#include <filexferlib/logger.hpp>
#include <filexferlib/detail/hash.hpp>

#include <iostream>
#include <fstream>
#include <filesystem>
#include <atomic>
#include <thread>
#include <chrono>
#include <random>
#include <cassert>
#include <vector>
#include <future>
#include <cstring>

namespace fs = std::filesystem;
using namespace filexferlib;

static int failures = 0;
#define CHECK(x) do { if(!(x)) { std::cerr << "FAIL: " #x " at line " << __LINE__ << "\n"; ++failures; } } while(0)

static void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(content.data(), content.size());
}

static std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

static std::string random_bytes(std::size_t n, unsigned seed = 42) {
    std::mt19937 rng(seed);
    std::string s(n, '\0');
    for (std::size_t i = 0; i < n; ++i) s[i] = static_cast<char>(rng() & 0xFF);
    return s;
}

// =====================================================================
// Test 1: many small files in parallel
// =====================================================================
static void test_many_small_files(Client& client, const fs::path& serverRoot,
                                  const fs::path& clientSrc) {
    std::cout << "\n[STRESS-1] 100 small files in parallel\n";

    constexpr int N = 100;
    std::vector<fs::path> localFiles;
    std::vector<std::future<TransferResult>> futs;

    for (int i = 0; i < N; ++i) {
        auto lf = clientSrc / ("small_" + std::to_string(i) + ".bin");
        std::string content = "file #" + std::to_string(i) +
                              " content " + std::string(64, 'x');
        write_file(lf, content);
        localFiles.push_back(lf);
    }

    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) {
        std::string remote = "/small_" + std::to_string(i) + ".bin";
        futs.push_back(client.upload(localFiles[i].string(), remote));
    }

    int ok = 0;
    for (auto& f : futs) {
        try {
            auto r = f.get();
            if (r.verified) ++ok;
        } catch (const std::exception& e) {
            std::cerr << "  upload failed: " << e.what() << "\n";
        }
    }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    std::cout << "  uploaded " << ok << "/" << N << " in " << ms << "ms\n";
    CHECK(ok == N);

    int found = 0;
    for (int i = 0; i < N; ++i) {
        auto p = serverRoot / ("small_" + std::to_string(i) + ".bin");
        if (fs::exists(p)) ++found;
    }
    CHECK(found == N);
}

// =====================================================================
// Test 2: large file
// =====================================================================
static void test_large_file(Client& client, const fs::path& serverRoot,
                            const fs::path& clientSrc) {
    std::cout << "\n[STRESS-2] large file (256 MB)\n";

    constexpr std::size_t SZ = 256ull * 1024 * 1024;
    auto lf = clientSrc / "large.bin";

    {
        std::ofstream f(lf, std::ios::binary | std::ios::trunc);
        std::mt19937 rng(12345);
        std::vector<char> buf(1024 * 1024);
        std::size_t written = 0;
        while (written < SZ) {
            for (auto& b : buf) b = static_cast<char>(rng() & 0xFF);
            std::size_t toWrite = std::min(buf.size(), SZ - written);
            f.write(buf.data(), static_cast<std::streamsize>(toWrite));
            written += toWrite;
        }
    }

    auto t0 = std::chrono::steady_clock::now();
    auto r = client.upload(lf.string(), "/large.bin", {},
        [](const TransferProgress&) {}).get();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    double mbps = (static_cast<double>(SZ) / (1024.0 * 1024.0)) /
                  (static_cast<double>(ms) / 1000.0);
    std::cout << "  " << SZ / (1024 * 1024) << " MB in " << ms << "ms ("
              << mbps << " MB/s)\n";
    CHECK(r.bytesTransferred == SZ);
    CHECK(r.verified);
    CHECK(fs::file_size(serverRoot / "large.bin") == SZ);
}

// =====================================================================
// Test 3: rapid connect/disconnect (with error reporting)
// =====================================================================
static void test_connect_churn(std::uint16_t port) {
    std::cout << "\n[STRESS-3] 30 rapid connect/disconnect cycles\n";

    constexpr int N = 30;
    int ok = 0;
    for (int i = 0; i < N; ++i) {
        try {
            Client c;
            ConnectOptions opts;
            opts.authToken = "secret123";
            opts.autoReconnect = false;
            opts.connectTimeout = std::chrono::milliseconds(3000);

            c.connect("127.0.0.1", port, opts);

            ByteBuffer payload(4);
            std::memcpy(payload.data(), "ping", 4);
            auto resp = c.call("echo", std::move(payload)).get();

            if (resp.statusCode == 0) {
                ++ok;
            } else {
                std::cerr << "  cycle " << i << ": statusCode=0x"
                          << std::hex << resp.statusCode << std::dec
                          << " payload_size=" << resp.payload.size() << "\n";
            }
            c.disconnect();
        } catch (const std::exception& e) {
            std::cerr << "  cycle " << i << " failed: " << e.what() << "\n";
            if (i >= 3) break;   // stop flooding after a few errors
        } catch (...) {
            std::cerr << "  cycle " << i << " failed: unknown\n";
            if (i >= 3) break;
        }
    }
    std::cout << "  " << ok << "/" << N << " cycles OK\n";
    CHECK(ok == N);
}

// =====================================================================
// Test 4: concurrent uploads + downloads
// =====================================================================
static void test_mixed_load(Client& client, const fs::path& serverRoot,
                            const fs::path& clientSrc) {
    std::cout << "\n[STRESS-4] mixed: 10 uploads + 10 downloads concurrently\n";

    std::vector<std::string> expected;
    for (int i = 0; i < 10; ++i) {
        auto lf = clientSrc / ("mix_" + std::to_string(i) + ".bin");
        std::string content = random_bytes(512 * 1024, i);
        write_file(lf, content);
        expected.push_back(content);
        client.upload(lf.string(), "/mix_" + std::to_string(i) + ".bin").get();
    }

    std::vector<std::future<TransferResult>> futs;
    for (int i = 0; i < 10; ++i) {
        auto lf = clientSrc / ("mix2_" + std::to_string(i) + ".bin");
        std::string content = random_bytes(512 * 1024, 100 + i);
        write_file(lf, content);
        futs.push_back(client.upload(lf.string(),
            "/mix2_" + std::to_string(i) + ".bin"));
    }
    std::vector<fs::path> dlPaths;
    for (int i = 0; i < 10; ++i) {
        auto dl = clientSrc / ("dl_" + std::to_string(i) + ".bin");
        std::error_code ec;
        fs::remove(dl, ec);
        dlPaths.push_back(dl);
        futs.push_back(client.download("/mix_" + std::to_string(i) + ".bin",
                                       dl.string()));
    }

    int ok = 0;
    for (auto& f : futs) {
        try {
            auto r = f.get();
            if (r.verified) ++ok;
        } catch (const std::exception& e) {
            std::cerr << "  failed: " << e.what() << "\n";
        }
    }
    std::cout << "  " << ok << "/20 operations succeeded\n";
    CHECK(ok == 20);

    for (int i = 0; i < 10; ++i) {
        CHECK(read_file(dlPaths[i]) == expected[i]);
    }
}

// =====================================================================
// Test 5: concurrent clients
// =====================================================================
static void test_multi_client(std::uint16_t /*port*/,
                              const fs::path& /*serverRoot*/,
                              const fs::path& clientSrc) {
    Logger::instance().setLevel(LogLevel::Warn);   // Ã¢â€ Â Ã˜Â¯Ã˜Â± Ã˜Â§Ã˜Â¨Ã˜ÂªÃ˜Â¯Ã˜Â§Ã›Å’ Ã˜ÂªÃ˜Â§Ã˜Â¨Ã˜Â¹
    Logger::instance().setSink([](LogLevel lv, const std::string& msg){
        std::cerr << msg << "\n";
    });
    std::cout << "\n[STRESS-5] 5 clients uploading simultaneously\n";

    // Start a fresh server for this test
    auto base5 = fs::temp_directory_path() / "filexfer_stress_mc";
    fs::remove_all(base5);
    fs::create_directories(base5);

    ServerConfig cfg5;
    cfg5.rootDir = base5;
    cfg5.requiredAuthToken = "secret123";
    Server server5(cfg5);
    server5.registerHandler("echo", [](GenericContext& ctx) {
        ctx.respond(ctx.request());
    });
    auto port5 = server5.start("127.0.0.1", 0);

    constexpr int NC = 5;
    std::vector<std::thread> threads;
    std::atomic<int> totalOk{0};
    std::atomic<int> totalTried{0};

    for (int c = 0; c < NC; ++c) {
        threads.emplace_back([&, c]{
            try {
                Client client;
                ConnectOptions opts;
                opts.authToken = "secret123";
                opts.autoReconnect = false;
                opts.connectTimeout = std::chrono::milliseconds(5000);
                opts.readTimeout = std::chrono::milliseconds(60000);
                client.connect("127.0.0.1", port5, opts);

                std::vector<std::future<TransferResult>> futs;
                for (int i = 0; i < 4; ++i) {
                    auto lf = clientSrc /
                        ("mc_" + std::to_string(c) + "_" +
                         std::to_string(i) + ".bin");
                    std::string content = random_bytes(256 * 1024, c * 100 + i);
                    write_file(lf, content);
                    std::string remote = "/mc_" +
                        std::to_string(c) + "_" + std::to_string(i) + ".bin";
                    futs.push_back(client.upload(lf.string(), remote));
                    ++totalTried;
                }
                for (auto& f : futs) {
                    try {
                        auto r = f.get();
                        if (r.verified) ++totalOk;
                    } catch (const std::exception& e) {
                        // silently count as failure
                    }
                }
                client.disconnect();
            } catch (const std::exception&) {
                // silently count as failure
            }
        });
    }
    for (auto& t : threads) t.join();

    std::cout << "  " << totalOk.load() << "/" << (NC * 4) << " uploads OK\n";
    CHECK(totalOk.load() == NC * 4);

    server5.stop();
    fs::remove_all(base5);
    Logger::instance().setLevel(LogLevel::Off);
    Logger::instance().setSink(nullptr);
}

// =====================================================================
// Main
// =====================================================================
int main() {
    // Disable logging completely for stress tests to avoid stdout races
    Logger::instance().setLevel(LogLevel::Off);

    auto base = fs::temp_directory_path() / "filexfer_stress";
    fs::remove_all(base);
    auto serverRoot = base / "server";
    auto clientSrc  = base / "client";
    fs::create_directories(serverRoot);
    fs::create_directories(clientSrc);

    ServerConfig cfg;
    cfg.rootDir = serverRoot;
    cfg.requiredAuthToken = "secret123";
    Server server(cfg);

    // ---- register generic handlers ----
    server.registerHandler("echo", [](GenericContext& ctx) {
        ctx.respond(ctx.request());
    });

    server.registerHandler("sum", [](GenericContext& ctx) {
        const auto& in = ctx.request();
        if (in.size() % 4 != 0) { ctx.respond({}, 2); return; }
        std::int64_t sum = 0;
        for (std::size_t i = 0; i < in.size(); i += 4) {
            std::int32_t v;
            std::memcpy(&v, in.data() + i, 4);
            sum += v;
        }
        ByteBuffer out(8);
        std::memcpy(out.data(), &sum, 8);
        ctx.respond(std::move(out));
    });

    auto port = server.start("127.0.0.1", 0);

    Client client;
    ConnectOptions opts;
    opts.authToken = "secret123";
    opts.autoReconnect = false;
    client.connect("127.0.0.1", port, opts);

    test_many_small_files(client, serverRoot, clientSrc);
    test_large_file(client, serverRoot, clientSrc);
    test_connect_churn(port);
    test_mixed_load(client, serverRoot, clientSrc);
    test_multi_client(port, serverRoot, clientSrc);

    client.disconnect();
    server.stop();
    fs::remove_all(base);

    if (failures == 0) {
        std::cout << "\n=== ALL STRESS TESTS PASSED ===\n";
        return 0;
    }
    std::cout << "\n=== " << failures << " STRESS TEST(S) FAILED ===\n";
    return 1;
}
