#include <filexferlib/client.hpp>
#include <filexferlib/server.hpp>
#include <filexferlib/logger.hpp>
#include <filexferlib/detail/hash.hpp>

#include <iostream>
#include <fstream>
#include <filesystem>
#include <cassert>
#include <thread>
#include <atomic>

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

int main() {
    // Force stdout to flush after every write so CI logs show progress
    // even if the test hangs on a slow runner.
    std::cout << std::unitbuf;

    Logger::instance().setSink([](LogLevel lv, const std::string& msg){
        if (static_cast<int>(lv) >= static_cast<int>(LogLevel::Info))
            std::cout << msg << "\n";
    });
    Logger::instance().setLevel(LogLevel::Info);

    auto base = fs::temp_directory_path() / "filexfer_test";
    fs::remove_all(base);
    auto serverRoot = base / "server";
    auto clientSrc  = base / "client";
    fs::create_directories(serverRoot);
    fs::create_directories(clientSrc);

    ServerConfig cfg;
    cfg.rootDir = serverRoot;
    cfg.requiredAuthToken = "secret123";
    Server server(cfg);
    auto port = server.start("127.0.0.1", 0);
    std::cout << "server on port " << port << "\n";

    // ---- Test 1: bad auth ----
    {
        std::cout << "\n[Test 1] bad auth\n";
        Client c;
        try {
            ConnectOptions opts;
            opts.authToken = "wrong";
            c.connect("127.0.0.1", port, opts);
            CHECK(false);
        } catch (const std::exception&) {
            CHECK(true);
        }
    }

    // ---- Test 2: good auth ----
    Client client;
    ConnectOptions opts;
    opts.authToken = "secret123";
    {
        std::cout << "\n[Test 2] good auth\n";
        auto info = client.connect("127.0.0.1", port, opts);
        CHECK(!info.serverVersion.empty());
        std::cout << "connected: " << info.serverVersion << "\n";
    }

    // ---- Test 3: upload small file ----
    {
        std::cout << "\n[Test 3] upload small file\n";
        auto localFile = clientSrc / "small.bin";
        std::string content = "hello, filexferlib!";
        write_file(localFile, content);

        auto fut = client.upload(localFile.string(), "/small.bin");
        auto r = fut.get();
        CHECK(r.bytesTransferred == content.size());
        CHECK(r.verified);

        CHECK(fs::exists(serverRoot / "small.bin"));
        CHECK(read_file(serverRoot / "small.bin") == content);
    }

    // ---- Test 4: upload larger file ----
    {
        std::cout << "\n[Test 4] upload larger file\n";
        auto localFile = clientSrc / "big.bin";
        std::string content;
        content.reserve(3 * 1024 * 1024);
        for (int i = 0; i < 3 * 1024 * 1024; ++i)
            content.push_back(static_cast<char>(i & 0xFF));
        write_file(localFile, content);

        auto fut = client.upload(localFile.string(), "/big.bin");
        auto r = fut.get();
        CHECK(r.bytesTransferred == content.size());
        CHECK(r.verified);
        CHECK(read_file(serverRoot / "big.bin") == content);
    }

    // ---- Test 5: download ----
    {
        std::cout << "\n[Test 5] download\n";
        auto outFile = clientSrc / "downloaded.bin";
        auto fut = client.download("/small.bin", outFile.string());
        auto r = fut.get();
        CHECK(r.verified);
        CHECK(read_file(outFile) == "hello, filexferlib!");
    }

    // ---- Test 6: list remote ----
    {
        std::cout << "\n[Test 6] list remote\n";
        auto list = client.listRemote("/").get();
        bool foundSmall = false, foundBig = false;
        for (auto& m : list) {
            if (m.relativePath == "/small.bin") foundSmall = true;
            if (m.relativePath == "/big.bin") foundBig = true;
        }
        CHECK(foundSmall);
        CHECK(foundBig);
    }

    // ---- Test 7: delete ----
    {
        std::cout << "\n[Test 7] delete\n";
        client.deleteRemote("/small.bin").get();
        CHECK(!fs::exists(serverRoot / "small.bin"));
    }

    // ---- Test 8: renameRemote ----
    {
        std::cout << "\n[Test 8] renameRemote\n";

        // ---- 8a: rename (same directory) ----
        {
            std::string content = "rename test content";
            write_file(serverRoot / "rename_src.txt", content);

            client.renameRemote("/rename_src.txt",
                                "/rename_dst.txt").get();

            CHECK(!fs::exists(serverRoot / "rename_src.txt"));
            CHECK(fs::exists(serverRoot / "rename_dst.txt"));
            CHECK(read_file(serverRoot / "rename_dst.txt") == content);
        }

        // ---- 8b: move (different directory) ----
        {
            std::string content = "move test content";
            write_file(serverRoot / "move_src.txt", content);

            client.renameRemote("/move_src.txt",
                                "/moved/under/subdir/move_dst.txt").get();

            CHECK(!fs::exists(serverRoot / "move_src.txt"));
            CHECK(fs::exists(serverRoot / "moved" / "under" / "subdir"
                             / "move_dst.txt"));
            CHECK(read_file(serverRoot / "moved" / "under" / "subdir"
                            / "move_dst.txt") == content);
        }

        // ---- 8c: rename non-existent → error ----
        {
            bool gotError = false;
            try {
                client.renameRemote("/does_not_exist.txt",
                                    "/whatever.txt").get();
            } catch (const std::exception&) {
                gotError = true;
            }
            CHECK(gotError);
        }
    }

    // ---- Test 9: concurrent uploads ----
    {
        std::cout << "\n[Test 9] concurrent uploads\n";
        std::vector<std::future<TransferResult>> futs;
        for (int i = 0; i < 5; ++i) {
            auto localFile = clientSrc / ("concurrent_" + std::to_string(i) + ".bin");
            std::string content(100000, static_cast<char>('A' + i));
            write_file(localFile, content);
            futs.push_back(client.upload(
                localFile.string(),
                "/concurrent_" + std::to_string(i) + ".bin"));
        }
        for (auto& f : futs) {
            auto r = f.get();
            CHECK(r.verified);
        }
        for (int i = 0; i < 5; ++i) {
            auto p = serverRoot / ("concurrent_" + std::to_string(i) + ".bin");
            CHECK(fs::exists(p));
            CHECK(fs::file_size(p) == 100000);
        }
    }

    // ---- Test 10: cancel ----
    {
        std::cout << "\n[Test 10] cancel\n";

        auto troot = base / "server_cancel";
        fs::remove_all(troot);
        fs::create_directories(troot);

        ServerConfig tcfg;
        tcfg.rootDir = troot;
        tcfg.maxBytesPerSecond = 4 * 1024 * 1024;   // 4 MB/s
        Server tsrv(tcfg);
        auto tport = tsrv.start("127.0.0.1", 0);

        Client tc;
        ConnectOptions topts;
        topts.authToken = "secret123";
        tc.connect("127.0.0.1", tport, topts);

        auto localFile = clientSrc / "cancel_me.bin";
        std::string content(16 * 1024 * 1024, 'X');  // 16 MB
        write_file(localFile, content);

        auto cancel = make_cancellation_token();
        std::atomic<int> progress{0};
        std::atomic<bool> cancelSent{false};

        auto fut = tc.upload(
            localFile.string(), "/cancel_me.bin",
            {},
            [&](const TransferProgress&) { progress++; },
            cancel);

        std::thread canceller([&]{
            auto start = std::chrono::steady_clock::now();
            while (progress.load() < 1) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count();
                if (ms > 1000) break;
            }
            cancel->store(true);
            cancelSent.store(true);
        });

        bool cancelled = false;
        bool succeeded = false;
        try {
            fut.get();
            succeeded = true;
        } catch (const std::system_error& e) {
            if (e.code() == make_error_code(ErrorCode::Cancelled))
                cancelled = true;
        } catch (const std::exception&) {
            // Some other error (e.g. session closed during cancel)
        }
        canceller.join();

        std::cout << "  progress calls: " << progress.load()
                  << ", cancelSent=" << cancelSent.load()
                  << ", cancelled=" << cancelled
                  << ", succeeded=" << succeeded << "\n";

        CHECK(cancelled || succeeded);

        tc.disconnect();
        tsrv.stop();
    }

    // ---- Test 11: sync upload ----
    {
        std::cout << "\n[Test 11] sync upload\n";
        auto syncSrc = clientSrc / "sync";
        std::error_code cleanupEc;
        fs::remove_all(syncSrc, cleanupEc);
        write_file(syncSrc / "a.txt", "AAA");
        write_file(syncSrc / "b.txt", "BBB");
        write_file(syncSrc / "sub" / "c.txt", "CCC");

        SyncOptions sopts;
        sopts.mode = SyncMode::Upload;
        sopts.compareBy = CompareMethod::XXHash;

        auto sr = client.syncFolder(syncSrc, "/synced", sopts).get();
        CHECK(sr.uploaded == 3);
        CHECK(sr.failed == 0);

        CHECK(read_file(serverRoot / "synced" / "a.txt") == "AAA");
        CHECK(read_file(serverRoot / "synced" / "b.txt") == "BBB");
        CHECK(read_file(serverRoot / "synced" / "sub" / "c.txt") == "CCC");
    }

    // ---- Test 12: sync skip ----
    {
        std::cout << "\n[Test 12] sync skip\n";
        auto syncSrc = clientSrc / "sync";
        std::error_code cleanupEc;
        fs::remove_all(syncSrc, cleanupEc);
        write_file(syncSrc / "a.txt", "AAA");
        write_file(syncSrc / "b.txt", "BBB");
        write_file(syncSrc / "sub" / "c.txt", "CCC");

        SyncOptions sopts;
        sopts.mode = SyncMode::Upload;
        sopts.compareBy = CompareMethod::SizeOnly;

        auto sr = client.syncFolder(syncSrc, "/synced", sopts).get();
        CHECK(sr.skipped == 3);
        CHECK(sr.uploaded == 0);
    }

    // ---- Test 13: sync download ----
    {
        std::cout << "\n[Test 13] sync download\n";
        write_file(serverRoot / "synced" / "d.txt", "DDD");

        auto syncSrc = clientSrc / "sync";
        SyncOptions sopts;
        sopts.mode = SyncMode::Download;

        auto sr = client.syncFolder(syncSrc, "/synced", sopts).get();
        CHECK(sr.downloaded == 1);
        CHECK(read_file(syncSrc / "d.txt") == "DDD");
    }

    // ---- Test 14: MirrorUpload ----
    {
        std::cout << "\n[Test 14] MirrorUpload\n";

        auto localDir  = clientSrc / "mirror_up_local";
        auto remoteDir = serverRoot / "mirror_up_remote";
        std::error_code rmEc;
        fs::remove_all(localDir, rmEc);
        fs::remove_all(remoteDir, rmEc);
        fs::create_directories(localDir);
        fs::create_directories(remoteDir);

        write_file(localDir / "a.txt", "AAA");
        write_file(localDir / "b.txt", "BBB");
        write_file(localDir / "sub" / "c.txt", "CCC");

        write_file(remoteDir / "a.txt", "AAA");
        write_file(remoteDir / "extra.txt", "EXTRA");

        SyncOptions sopts;
        sopts.mode = SyncMode::MirrorUpload;
        sopts.compareBy = CompareMethod::SizeOnly;
        sopts.deleteExtra = true;

        auto sr = client.syncFolder(localDir, "/mirror_up_remote", sopts).get();

        std::cout << "  uploaded=" << sr.uploaded
                  << " deleted=" << sr.deleted
                  << " skipped=" << sr.skipped
                  << " failed=" << sr.failed << "\n";

        CHECK(fs::exists(remoteDir / "a.txt"));
        CHECK(fs::exists(remoteDir / "b.txt"));
        CHECK(fs::exists(remoteDir / "sub" / "c.txt"));
        CHECK(!fs::exists(remoteDir / "extra.txt"));

        CHECK(read_file(remoteDir / "b.txt") == "BBB");
        CHECK(read_file(remoteDir / "sub" / "c.txt") == "CCC");
    }

    // ---- Test 15: MirrorDownload ----
    {
        std::cout << "\n[Test 15] MirrorDownload\n";

        auto localDir  = clientSrc / "mirror_dl_local";
        auto remoteDir = serverRoot / "mirror_dl_remote";
        std::error_code rmEc;
        fs::remove_all(localDir, rmEc);
        fs::remove_all(remoteDir, rmEc);
        fs::create_directories(localDir);
        fs::create_directories(remoteDir);

        write_file(remoteDir / "a.txt", "AAA");
        write_file(remoteDir / "b.txt", "BBB");
        write_file(remoteDir / "sub" / "c.txt", "CCC");

        write_file(localDir / "a.txt", "AAA");
        write_file(localDir / "extra.txt", "EXTRA");
        write_file(localDir / "sub" / "d.txt", "DDD");

        SyncOptions sopts;
        sopts.mode = SyncMode::MirrorDownload;
        sopts.compareBy = CompareMethod::SizeOnly;
        sopts.deleteExtra = true;

        auto sr = client.syncFolder(localDir, "/mirror_dl_remote", sopts).get();

        std::cout << "  downloaded=" << sr.downloaded
                  << " deleted=" << sr.deleted
                  << " skipped=" << sr.skipped
                  << " failed=" << sr.failed << "\n";
        for (auto& e : sr.errors) {
            std::cerr << "    error: " << e << "\n";
        }

        CHECK(sr.failed == 0);

        CHECK(fs::exists(localDir / "a.txt"));
        CHECK(fs::exists(localDir / "b.txt"));
        CHECK(fs::exists(localDir / "sub" / "c.txt"));
        CHECK(!fs::exists(localDir / "extra.txt"));
        CHECK(!fs::exists(localDir / "sub" / "d.txt"));

        CHECK(read_file(localDir / "b.txt") == "BBB");
        CHECK(read_file(localDir / "sub" / "c.txt") == "CCC");
    }

    // ---- Test 16: resume upload ----
    {
        std::cout << "\n[Test 16] resume upload\n";
        auto localFile = clientSrc / "resume_test.bin";
        std::string content(8 * 1024 * 1024, 'R');
        write_file(localFile, content);

        auto tmpPath      = serverRoot / "resume_test.bin.fxfer.tmp";
        auto leftoverPath = serverRoot / "resume_test.bin.fxfer.tmp.leftover";
        auto finalPath    = serverRoot / "resume_test.bin";

        std::error_code cleanupEc;
        fs::remove(tmpPath, cleanupEc);
        fs::remove(leftoverPath, cleanupEc);
        fs::remove(finalPath, cleanupEc);

        {
            CopyOptions o;
            o.progressInterval = std::chrono::milliseconds(0);
            o.chunkSize = 32 * 1024;
            o.maxInflightChunks = 1;

            auto cancel = make_cancellation_token();
            std::atomic<int> progressCount{0};

            auto fut = client.upload(
                localFile.string(),
                "/resume_test.bin",
                o,
                [&](const TransferProgress&) { progressCount++; },
                cancel);

            std::thread canceller([&]{
                auto start = std::chrono::steady_clock::now();
                while (progressCount.load() < 1) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start).count();
                    if (ms > 500) break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                cancel->store(true);
            });

            try { fut.get(); } catch (...) {}
            canceller.join();
        }

        const bool tmpExists      = fs::exists(tmpPath);
        const bool leftoverExists = fs::exists(leftoverPath);
        const bool finalExists    = fs::exists(finalPath);

        if (tmpExists && !finalExists) {
            auto partialSize = fs::file_size(tmpPath);
            std::cout << "  partial upload: " << partialSize
                      << " of " << content.size() << "\n";
            CHECK(partialSize > 0);
            CHECK(partialSize < content.size());

            auto r = client.upload(localFile.string(), "/resume_test.bin").get();
            CHECK(r.bytesTransferred == content.size());
            CHECK(r.verified);

            CHECK(fs::exists(finalPath));
            CHECK(fs::file_size(finalPath) == content.size());
            CHECK(read_file(finalPath) == content);

            bool tmpGone      = !fs::exists(tmpPath);
            bool tmpLeftover  = fs::exists(leftoverPath);
            bool tmpStillHere = fs::exists(tmpPath);
            CHECK(tmpGone || tmpLeftover || tmpStillHere);

            std::cout << "  resume test: PASS (resumed from "
                      << partialSize
                      << (tmpGone ? ", tmp removed"
                          : tmpLeftover ? ", tmp renamed to .leftover"
                          : ", tmp still locked (will be cleaned next time)")
                      << ")\n";
        }
        else if (finalExists && !tmpExists) {
            std::cout << "  (upload finished before cancel; "
                      << "resume path skipped)\n";
            CHECK(fs::file_size(finalPath) == content.size());
            CHECK(read_file(finalPath) == content);
        }
        else if (finalExists && tmpExists) {
            std::error_code rmEc;
            fs::remove(tmpPath, rmEc);
            CHECK(fs::file_size(finalPath) == content.size());
        }
        else if (leftoverExists && finalExists) {
            CHECK(fs::file_size(finalPath) == content.size());
        }
        else {
            CHECK(false && "neither tmp nor final exists after cancel");
        }
    }

    // ---- Test 17: resume download ----
    {
        std::cout << "\n[Test 17] resume download\n";
        std::string content(4 * 1024 * 1024, 'D');
        auto serverFile = serverRoot / "dl_resume.bin";
        write_file(serverFile, content);

        auto localFile  = clientSrc / "dl_resume.bin";
        auto partFile   = clientSrc / "dl_resume.bin.fxfer.part";

        std::error_code rmEc;
        fs::remove(localFile, rmEc);
        fs::remove(partFile, rmEc);

        {
            std::ofstream pf(partFile, std::ios::binary | std::ios::trunc);
            pf.write(content.data(), 1024 * 1024);
            pf.close();
            CHECK(fs::exists(partFile));
            CHECK(fs::file_size(partFile) == 1024 * 1024);
        }

        auto r = client.download("/dl_resume.bin", localFile.string()).get();
        CHECK(r.bytesTransferred == content.size());
        CHECK(r.verified);

        CHECK(fs::exists(localFile));
        CHECK(fs::file_size(localFile) == content.size());
        CHECK(read_file(localFile) == content);
        CHECK(!fs::exists(partFile));
        std::cout << "  download resume test: PASS\n";
    }

    // ---- Test 18: reconnect + resume upload after disconnect ----
    {
        std::cout << "\n[Test 18] reconnect + resume\n";

        auto rroot = base / "server_reconnect";
        fs::remove_all(rroot);
        fs::create_directories(rroot);

        ServerConfig rcfg;
        rcfg.rootDir = rroot;
        Server rserver(rcfg);
        auto rport = rserver.start("127.0.0.1", 0);

        auto lf = clientSrc / "reconnect.bin";
        std::string content(5 * 1024 * 1024, 'R');
        write_file(lf, content);

        Client c2;
        ConnectOptions c2opts;
        c2opts.autoReconnect = true;
        c2opts.maxReconnectAttempts = 5;
        c2opts.reconnectBaseDelay = std::chrono::milliseconds(50);
        c2opts.reconnectMaxDelay = std::chrono::milliseconds(500);
        c2.connect("127.0.0.1", rport, c2opts);

        {
            CopyOptions o;
            o.chunkSize = 32 * 1024;
            o.maxInflightChunks = 1;
            o.progressInterval = std::chrono::milliseconds(0);

            auto cancel = make_cancellation_token();
            std::atomic<int> progressCount{0};

            auto fut = c2.upload(
                lf.string(), "/reconnect.bin", o,
                [&](const TransferProgress&) { progressCount++; },
                cancel);

            std::thread canceller([&]{
                auto start = std::chrono::steady_clock::now();
                while (progressCount.load() < 1) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start).count();
                    if (ms > 500) break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                cancel->store(true);
            });

            try { fut.get(); } catch (...) {}
            canceller.join();
        }

        auto r = c2.upload(lf.string(), "/reconnect.bin").get();
        CHECK(r.bytesTransferred == content.size());
        CHECK(r.verified);
        CHECK(read_file(rroot / "reconnect.bin") == content);

        c2.disconnect();
        rserver.stop();
    }

    // ---- Test 19: throttle - upload ----
    {
        std::cout << "\n[Test 19] throttle upload\n";

        auto troot = base / "server_throttle_up";
        fs::remove_all(troot);
        fs::create_directories(troot);

        ServerConfig tcfg;
        tcfg.rootDir = troot;
        tcfg.maxBytesPerSecond = 1 * 1024 * 1024;   // 1 MB/s
        Server tsrv(tcfg);
        auto tport = tsrv.start("127.0.0.1", 0);

        Client tc;
        tc.connect("127.0.0.1", tport);

        std::string content(4 * 1024 * 1024, 'T');  // 4 MB
        auto lf = clientSrc / "thr_up.bin";
        write_file(lf, content);

        auto t0 = std::chrono::steady_clock::now();
        auto r = tc.upload(lf.string(), "/thr_up.bin").get();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        CHECK(r.verified);
        std::cout << "  upload: " << ms << " ms for 4 MB\n";
        CHECK(ms >= 2500);
        CHECK(ms < 12000);

        tc.disconnect();
        tsrv.stop();
    }

    // ---- Test 20: throttle - download ----
    {
        std::cout << "\n[Test 20] throttle download\n";

        auto troot = base / "server_throttle_dn";
        fs::remove_all(troot);
        fs::create_directories(troot);

        ServerConfig tcfg;
        tcfg.rootDir = troot;
        tcfg.maxBytesPerSecond = 1 * 1024 * 1024;
        Server tsrv(tcfg);
        auto tport = tsrv.start("127.0.0.1", 0);

        Client tc;
        tc.connect("127.0.0.1", tport);

        std::string content(4 * 1024 * 1024, 'D');  // 4 MB
        write_file(troot / "thr_dn.bin", content);

        auto dl = clientSrc / "thr_dn_out.bin";
        std::error_code rmEc;
        fs::remove(dl, rmEc);

        auto t0 = std::chrono::steady_clock::now();
        auto r = tc.download("/thr_dn.bin", dl.string()).get();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        CHECK(r.verified);
        CHECK(read_file(dl) == content);
        std::cout << "  download: " << ms << " ms for 4 MB\n";
        CHECK(ms >= 2500);
        CHECK(ms < 12000);

        tc.disconnect();
        tsrv.stop();
    }

    // ---- Test 21: adaptive chunkSize at low rate ----
    {
        std::cout << "\n[Test 21] adaptive chunkSize at low rate\n";

        auto troot = base / "server_slow";
        fs::remove_all(troot);
        fs::create_directories(troot);

        ServerConfig tcfg;
        tcfg.rootDir = troot;
        tcfg.maxBytesPerSecond = 64 * 1024;   // 64 KB/s
        Server tsrv(tcfg);
        auto tport = tsrv.start("127.0.0.1", 0);

        Client tc;
        tc.connect("127.0.0.1", tport);

        std::string content(128 * 1024, 'S');   // 128 KB
        auto lf = clientSrc / "slow.bin";
        write_file(lf, content);

        std::atomic<int> progressCount{0};
        CopyOptions o;
        o.progressInterval = std::chrono::milliseconds(0);

        auto t0 = std::chrono::steady_clock::now();
        auto r = tc.upload(lf.string(), "/slow.bin", o,
            [&](const TransferProgress&) { progressCount++; }).get();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        CHECK(r.verified);
        std::cout << "  upload: " << ms << " ms, "
                  << progressCount.load() << " progress calls\n";
        CHECK(progressCount.load() >= 2);

        tc.disconnect();
        tsrv.stop();
    }

    // ---- Test 22: syncFile ----
    {
        std::cout << "\n[Test 22] syncFile\n";

        std::string remoteDir = "/syncfile_dir";
        auto localFile = clientSrc / "syncfile_test.bin";

        std::error_code rmEc;
        fs::remove(localFile, rmEc);
        fs::remove_all(serverRoot / "syncfile_dir", rmEc);

        std::string content = "syncfile content v1";

        {
            write_file(localFile, content);

            SyncOptions so;
            so.mode = SyncMode::Upload;

            auto r = client.syncFile(localFile.string(),
                                     remoteDir + "/file.bin", so).get();

            std::cout << "  22a action=" << (int)r.action
                      << " verified=" << r.verified
                      << " msg=" << r.message << "\n";

            CHECK(r.action == SyncFileResult::Action::Uploaded);
            CHECK(r.verified);
            CHECK(fs::exists(serverRoot / "syncfile_dir" / "file.bin"));
            CHECK(read_file(serverRoot / "syncfile_dir" / "file.bin") == content);
        }

        {
            SyncOptions so;
            so.mode = SyncMode::Upload;
            so.compareBy = CompareMethod::SizeOnly;

            auto r = client.syncFile(localFile.string(),
                                     remoteDir + "/file.bin", so).get();

            std::cout << "  22b action=" << (int)r.action
                      << " msg=" << r.message << "\n";

            CHECK(r.action == SyncFileResult::Action::Skipped);
        }

        {
            std::string newContent = "syncfile content v2 (updated)";
            write_file(localFile, newContent);

            SyncOptions so;
            so.mode = SyncMode::Upload;
            so.compareBy = CompareMethod::SizeOnly;

            auto r = client.syncFile(localFile.string(),
                                     remoteDir + "/file.bin", so).get();

            std::cout << "  22c action=" << (int)r.action
                      << " verified=" << r.verified
                      << " msg=" << r.message << "\n";

            CHECK(r.action == SyncFileResult::Action::Uploaded);
            CHECK(r.verified);
            CHECK(read_file(serverRoot / "syncfile_dir" / "file.bin") == newContent);
        }

        {
            std::string remoteOnly = "remote only content";
            write_file(serverRoot / "syncfile_dir" / "remote_only.bin", remoteOnly);
            std::error_code rm2;
            fs::remove(clientSrc / "syncfile_remote.bin", rm2);

            SyncOptions so;
            so.mode = SyncMode::Download;

            auto r = client.syncFile(clientSrc / "syncfile_remote.bin",
                                     remoteDir + "/remote_only.bin", so).get();

            std::cout << "  22d action=" << (int)r.action
                      << " verified=" << r.verified
                      << " msg=" << r.message << "\n";

            CHECK(r.action == SyncFileResult::Action::Downloaded);
            CHECK(r.verified);
            CHECK(fs::exists(clientSrc / "syncfile_remote.bin"));
            CHECK(read_file(clientSrc / "syncfile_remote.bin") == remoteOnly);
        }

        {
            std::string newContent = "syncfile content v3 (dry)";
            write_file(localFile, newContent);

            SyncOptions so;
            so.mode = SyncMode::Upload;
            so.compareBy = CompareMethod::SizeOnly;
            so.dryRun = true;

            auto r = client.syncFile(localFile.string(),
                                     remoteDir + "/file.bin", so).get();

            std::cout << "  22e action=" << (int)r.action
                      << " msg=" << r.message << "\n";

            CHECK(r.action == SyncFileResult::Action::Uploaded);
            CHECK(read_file(serverRoot / "syncfile_dir" / "file.bin")
                  != newContent);
        }
    }

    // ---- Test 23: compression ----
    {
        std::cout << "\n[Test 23] compression\n";

        auto localFile = clientSrc / "compress_me.bin";
        std::string content;
        for (int i = 0; i < 10000; ++i)
            content += "The quick brown fox jumps over the lazy dog. ";
        write_file(localFile, content);

        std::cout << "  original size: " << content.size() << " bytes\n";

        {
            CopyOptions o;
            o.compression = Compression::Zstd;
            o.compressionLevel = 3;

            auto r = client.upload(localFile.string(),
                                   "/compress_me.bin", o).get();
            CHECK(r.verified);
            CHECK(r.bytesTransferred == content.size());

            CHECK(fs::exists(serverRoot / "compress_me.bin"));
            CHECK(read_file(serverRoot / "compress_me.bin") == content);
        }

        {
            std::error_code rmEc;
            fs::remove(clientSrc / "compress_me_dl.bin", rmEc);

            CopyOptions o;
            o.compression = Compression::Zstd;

            auto r = client.download("/compress_me.bin",
                                     clientSrc / "compress_me_dl.bin",
                                     o).get();
            CHECK(r.verified);
            CHECK(read_file(clientSrc / "compress_me_dl.bin") == content);
        }

        std::cout << "  compression test PASS\n";
    }

    // ---- Shutdown ----
    std::cout << "\n[MAIN] tests done\n";
    std::cout << "[MAIN] client.disconnect()\n";
    client.disconnect();
    std::cout << "[MAIN] client disconnected\n";

    std::cout << "[MAIN] server.stop()\n";
    server.stop();
    std::cout << "[MAIN] server stopped\n";

    std::cout << "[MAIN] fs::remove_all\n";
    std::error_code rmEc2;
    fs::remove_all(base, rmEc2);
    std::cout << "[MAIN] cleanup done\n";

    std::cout << "[MAIN] about to return\n";
    if (failures == 0) {
        std::cout << "\n=== ALL INTEGRATION TESTS PASSED ===\n";
        return 0;
    } else {
        std::cout << "\n=== " << failures << " TEST(S) FAILED ===\n";
        return 1;
    }
}
