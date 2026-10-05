#include <filexferlib/client.hpp>
#include <filexferlib/server.hpp>
#include <filexferlib/logger.hpp>

#include <iostream>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <random>
#include <vector>
#include <iomanip>
#include <thread>

namespace fs = std::filesystem;
using namespace filexferlib;

struct BenchResult {
    std::string name;
    std::uint64_t bytes = 0;
    std::uint64_t ms = 0;
    double mbps() const {
        return ms > 0 ? (double(bytes) / (1024.0 * 1024.0)) /
                        (double(ms) / 1000.0) : 0.0;
    }
};

static void write_random_file(const fs::path& p, std::size_t sz, unsigned seed) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    std::mt19937 rng(seed);
    std::vector<char> buf(64 * 1024);
    std::size_t written = 0;
    while (written < sz) {
        for (auto& b : buf) b = static_cast<char>(rng() & 0xFF);
        std::size_t n = std::min(buf.size(), sz - written);
        f.write(buf.data(), static_cast<std::streamsize>(n));
        written += n;
    }
}

static BenchResult bench_upload(Client& client, const fs::path& src,
                                const std::string& remote,
                                CopyOptions opts) {
    auto t0 = std::chrono::steady_clock::now();
    auto r = client.upload(src.string(), remote, opts).get();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    BenchResult br;
    br.name = "upload";
    br.bytes = r.bytesTransferred;
    br.ms = ms;
    return br;
}

static BenchResult bench_download(Client& client, const std::string& remote,
                                  const fs::path& dst, CopyOptions opts) {
    std::error_code ec;
    fs::remove(dst, ec);

    auto t0 = std::chrono::steady_clock::now();
    auto r = client.download(remote, dst.string(), opts).get();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    BenchResult br;
    br.name = "download";
    br.bytes = r.bytesTransferred;
    br.ms = ms;
    return br;
}

static void print(const BenchResult& r) {
    std::cout << "  " << std::setw(28) << std::left << r.name
              << std::setw(12) << r.bytes
              << std::setw(10) << r.ms << " ms"
              << std::fixed << std::setprecision(1)
              << "  " << std::setw(8) << r.mbps() << " MB/s\n";
}

int main(int argc, char** argv) {
    Logger::instance().setLevel(LogLevel::Warn);
    Logger::instance().setSink([](LogLevel, const std::string&){});
    (void)argc; (void)argv;

    auto base = fs::temp_directory_path() / "filexfer_bench";
    fs::remove_all(base);
    auto serverRoot = base / "server";
    auto clientDir  = base / "client";
    fs::create_directories(serverRoot);
    fs::create_directories(clientDir);

    ServerConfig cfg;
    cfg.rootDir = serverRoot;
    Server server(cfg);
    auto port = server.start("127.0.0.1", 0);

    Client client;
    client.connect("127.0.0.1", port);

    std::cout << "\n===== filexferlib benchmark =====\n\n";

    // ---- Chunk size sweep ----
    std::cout << "[Chunk size sweep] 64 MB file\n";
    {
        constexpr std::size_t SZ = 64ull * 1024 * 1024;
        auto lf = clientDir / "sweep.bin";
        write_random_file(lf, SZ, 1);

        std::vector<std::uint32_t> chunkSizes = {
            16 * 1024, 64 * 1024, 256 * 1024, 1024 * 1024, 4 * 1024 * 1024
        };

        for (auto cs : chunkSizes) {
            CopyOptions opts;
            opts.chunkSize = cs;
            opts.verifyHash = false;
            opts.progressInterval = std::chrono::milliseconds(0);

            auto r = bench_upload(client, lf, "/sweep.bin", opts);
            r.name = "chunk=" + std::to_string(cs / 1024) + "KB";
            print(r);
        }
    }

    // ---- Pipelining sweep ----
    std::cout << "\n[Pipelining sweep] 64 MB file, chunk=256KB\n";
    {
        constexpr std::size_t SZ = 64ull * 1024 * 1024;
        auto lf = clientDir / "pipe.bin";
        write_random_file(lf, SZ, 2);

        std::vector<std::uint32_t> windows = {1, 2, 4, 8, 16};
        for (auto w : windows) {
            CopyOptions opts;
            opts.chunkSize = 256 * 1024;
            opts.maxInflightChunks = w;
            opts.verifyHash = false;
            opts.progressInterval = std::chrono::milliseconds(0);

            auto r = bench_upload(client, lf, "/pipe.bin", opts);
            r.name = "window=" + std::to_string(w);
            print(r);
        }
    }

    // ---- Verify on/off ----
    std::cout << "\n[Verify hash on/off] 64 MB file\n";
    {
        constexpr std::size_t SZ = 64ull * 1024 * 1024;
        auto lf = clientDir / "verify.bin";
        write_random_file(lf, SZ, 3);

        for (bool verify : {false, true}) {
            CopyOptions opts;
            opts.chunkSize = 256 * 1024;
            opts.verifyHash = verify;
            opts.progressInterval = std::chrono::milliseconds(0);

            auto r = bench_upload(client, lf, "/verify.bin", opts);
            r.name = verify ? "verify=on" : "verify=off";
            print(r);
        }
    }

    // ---- Bandwidth throttle ----
    std::cout << "\n[Bandwidth throttle] 16 MB file, server limit 2 MB/s\n";
    {
        // Note: server throttle was configured at construction.
        // For this bench we start a second server with throttle.
        auto base2 = base / "server_throttled";
        fs::create_directories(base2);

        ServerConfig cfg2;
        cfg2.rootDir = base2;
        cfg2.maxBytesPerSecond = 2 * 1024 * 1024;
        Server server2(cfg2);
        auto port2 = server2.start("127.0.0.1", 0);

        Client c2;
        c2.connect("127.0.0.1", port2);

        constexpr std::size_t SZ = 16ull * 1024 * 1024;
        auto lf = clientDir / "throttled.bin";
        write_random_file(lf, SZ, 4);

        CopyOptions opts;
        opts.chunkSize = 256 * 1024;
        opts.verifyHash = false;

        auto r = bench_upload(c2, lf, "/throttled.bin", opts);
        r.name = "throttle=2MB/s";
        print(r);

        c2.disconnect();
        server2.stop();
    }

    // ---- Download ----
    std::cout << "\n[Download] 64 MB file\n";
    {
        auto dl = clientDir / "downloaded.bin";
        CopyOptions opts;
        opts.chunkSize = 256 * 1024;
        opts.verifyHash = true;

        auto r = bench_download(client, "/sweep.bin", dl, opts);
        r.name = "download 64MB";
        print(r);
    }

    // ---- Small files latency ----
    std::cout << "\n[Small files latency] 100 Ãƒâ€” 1KB files\n";
    {
        std::vector<fs::path> files;
        for (int i = 0; i < 100; ++i) {
            auto lf = clientDir / ("tiny_" + std::to_string(i) + ".bin");
            write_random_file(lf, 1024, i);
            files.push_back(lf);
        }

        CopyOptions opts;
        opts.verifyHash = false;

        auto t0 = std::chrono::steady_clock::now();
        std::vector<std::future<TransferResult>> futs;
        for (int i = 0; i < 100; ++i) {
            futs.push_back(client.upload(
                files[i].string(),
                "/tiny_" + std::to_string(i) + ".bin",
                opts));
        }
        std::uint64_t totalBytes = 0;
        for (auto& f : futs) totalBytes += f.get().bytesTransferred;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        BenchResult br;
        br.name = "100Ãƒâ€” 1KB";
        br.bytes = totalBytes;
        br.ms = ms;
        std::cout << "  " << std::setw(28) << std::left << br.name
                  << std::setw(12) << br.bytes
                  << std::setw(10) << br.ms << " ms"
                  << "  " << std::fixed << std::setprecision(2)
                  << (double(br.ms) / 100.0) << " ms/op\n";
    }

    client.disconnect();
    server.stop();
    fs::remove_all(base);

    std::cout << "\n===== done =====\n";
    return 0;
}
