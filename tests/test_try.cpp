#include <filexferlib/client.hpp>
#include <filexferlib/server.hpp>
#include <filexferlib/logger.hpp>

#include <iostream>
#include <fstream>
#include <filesystem>
#include <thread>

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

int main() {
    std::cout << std::unitbuf;
    Logger::instance().setLevel(LogLevel::Off);

    auto base = fs::temp_directory_path() / "filexfer_try";
    fs::remove_all(base);
    auto serverRoot = base / "server";
    auto clientSrc  = base / "client";
    fs::create_directories(serverRoot);
    fs::create_directories(clientSrc);

    // =================================================================
    // Test 1: tryConnect to a non-existent server → error
    // =================================================================
    {
        std::cout << "\n[TRY-1] tryConnect to non-existent server\n";

        Client c;
        ConnectOptions o;
        o.connectTimeout = std::chrono::milliseconds(500);
        o.maxReconnectAttempts = 1;

        auto r = c.tryConnect("127.0.0.1", 65000, o);

        CHECK(!r.ok());
        CHECK(r.error());
        std::cout << "  error: " << r.error().message()
                  << " (" << r.message() << ")\n";
        // Should not throw even if we call value() on error
        bool threw = false;
        try { (void)r.value(); } catch (const std::logic_error&) { threw = true; }
        CHECK(threw);
    }

    // =================================================================
    // Start a real server for the rest of the tests
    // =================================================================
    ServerConfig cfg;
    cfg.rootDir = serverRoot;
    cfg.requiredAuthToken = "secret123";
    Server server(cfg);
    server.registerHandler("echo", [](GenericContext& ctx) {
        ctx.respond(ctx.request());
    });
    auto port = server.start("127.0.0.1", 0);
    std::cout << "server on port " << port << "\n";

    Client client;
    ConnectOptions opts;
    opts.authToken = "secret123";

    // =================================================================
    // Test 2: tryConnect success
    // =================================================================
    {
        std::cout << "\n[TRY-2] tryConnect success\n";
        auto r = client.tryConnect("127.0.0.1", port, opts);
        CHECK(r.ok());
        CHECK(!r.error());
        CHECK(!r.value().serverVersion.empty());
        std::cout << "  connected: " << r.value().serverVersion << "\n";
    }

    // =================================================================
    // Test 3: tryConnect with bad auth
    // =================================================================
    {
        std::cout << "\n[TRY-3] tryConnect bad auth\n";
        Client c;
        ConnectOptions badOpts;
        badOpts.authToken = "wrong";
        auto r = c.tryConnect("127.0.0.1", port, badOpts);
        CHECK(!r.ok());
        std::cout << "  error: " << r.error().message() << "\n";
    }

    // =================================================================
    // Test 4: tryUpload success
    // =================================================================
    {
        std::cout << "\n[TRY-4] tryUpload\n";
        auto lf = clientSrc / "try_upload.bin";
        std::string content = "exception-free upload";
        write_file(lf, content);

        auto r = client.tryUpload(lf.string(), "/try_upload.bin").get();
        CHECK(r.ok());
        CHECK(r.value().verified);
        CHECK(r.value().bytesTransferred == content.size());
        CHECK(read_file(serverRoot / "try_upload.bin") == content);
    }

    // =================================================================
    // Test 5: tryUpload non-existent local file → error
    // =================================================================
    {
        std::cout << "\n[TRY-5] tryUpload non-existent local\n";
        auto r = client.tryUpload(
            (clientSrc / "does_not_exist.bin").string(),
            "/whatever.bin").get();
        CHECK(!r.ok());
        std::cout << "  error: " << r.error().message()
                  << " (" << r.message() << ")\n";
    }

    // =================================================================
    // Test 6: tryDownload success
    // =================================================================
    {
        std::cout << "\n[TRY-6] tryDownload\n";
        auto out = clientSrc / "try_download.bin";
        std::error_code rmEc;
        fs::remove(out, rmEc);

        auto r = client.tryDownload("/try_upload.bin", out.string()).get();
        CHECK(r.ok());
        CHECK(r.value().verified);
        CHECK(read_file(out) == "exception-free upload");
    }

    // =================================================================
    // Test 7: tryDownload missing remote → error
    // =================================================================
    {
        std::cout << "\n[TRY-7] tryDownload missing remote\n";
        auto r = client.tryDownload(
            "/does_not_exist_on_server.bin",
            (clientSrc / "never.bin").string()).get();
        CHECK(!r.ok());
        std::cout << "  error: " << r.error().message() << "\n";
    }

    // =================================================================
    // Test 8: tryListRemote
    // =================================================================
    {
        std::cout << "\n[TRY-8] tryListRemote\n";
        auto r = client.tryListRemote("/").get();
        CHECK(r.ok());
        bool found = false;
        for (auto& m : r.value())
            if (m.relativePath == "/try_upload.bin") found = true;
        CHECK(found);
    }

    // =================================================================
    // Test 9: tryRenameRemote
    // =================================================================
    {
        std::cout << "\n[TRY-9] tryRenameRemote\n";
        auto r = client.tryRenameRemote("/try_upload.bin",
                                        "/try_renamed.bin").get();
        CHECK(r.ok());
        CHECK(fs::exists(serverRoot / "try_renamed.bin"));

        // missing source → error
        auto r2 = client.tryRenameRemote("/missing.bin",
                                         "/any.bin").get();
        CHECK(!r2.ok());
    }

    // =================================================================
    // Test 10: tryDeleteRemote
    // =================================================================
    {
        std::cout << "\n[TRY-10] tryDeleteRemote\n";
        auto r = client.tryDeleteRemote("/try_renamed.bin").get();
        CHECK(r.ok());
        CHECK(!fs::exists(serverRoot / "try_renamed.bin"));
    }

    // =================================================================
    // Test 11: tryMkdirRemote
    // =================================================================
    {
        std::cout << "\n[TRY-11] tryMkdirRemote\n";
        auto r = client.tryMkdirRemote("/try_dir/sub").get();
        CHECK(r.ok());
        CHECK(fs::exists(serverRoot / "try_dir" / "sub"));
    }

    // =================================================================
    // Test 12: trySetRemoteMtime
    // =================================================================
    {
        std::cout << "\n[TRY-12] trySetRemoteMtime\n";
        auto lf = clientSrc / "mtime.bin";
        write_file(lf, "x");
        client.upload(lf.string(), "/mtime.bin").get();

        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        auto r = client.trySetRemoteMtime("/mtime.bin", ns).get();
        CHECK(r.ok());
    }

    // =================================================================
    // Test 13: tryCopyRemoteToRemote
    // =================================================================
    {
        std::cout << "\n[TRY-13] tryCopyRemoteToRemote\n";
        auto lf = clientSrc / "copy_src.bin";
        write_file(lf, "src content");
        client.upload(lf.string(), "/copy_src.bin").get();

        auto r = client.tryCopyRemoteToRemote("/copy_src.bin",
                                              "/copy_dst.bin").get();
        CHECK(r.ok());
        CHECK(fs::exists(serverRoot / "copy_dst.bin"));

        // missing source → error
        auto r2 = client.tryCopyRemoteToRemote("/missing.bin",
                                               "/any.bin").get();
        CHECK(!r2.ok());
    }

    // =================================================================
    // Test 14: trySyncFolder
    // =================================================================
    {
        std::cout << "\n[TRY-14] trySyncFolder\n";
        auto syncSrc = clientSrc / "sync_try";
        std::error_code rmEc;
        fs::remove_all(syncSrc, rmEc);
        write_file(syncSrc / "a.txt", "AAA");
        write_file(syncSrc / "b.txt", "BBB");

        SyncOptions so;
        so.mode = SyncMode::Upload;

        auto r = client.trySyncFolder(syncSrc, "/synced_try", so).get();
        CHECK(r.ok());
        CHECK(r.value().uploaded == 2);
        CHECK(r.value().failed == 0);
        CHECK(read_file(serverRoot / "synced_try" / "a.txt") == "AAA");
    }

    // =================================================================
    // Test 15: trySyncFile
    // =================================================================
    {
        std::cout << "\n[TRY-15] trySyncFile\n";
        auto lf = clientSrc / "syncfile_try.bin";
        std::error_code rmEc;
        fs::remove(lf, rmEc);
        write_file(lf, "syncfile try content");

        SyncOptions so;
        so.mode = SyncMode::Upload;

        auto r = client.trySyncFile(lf.string(), "/syncfile_try.bin", so).get();
        CHECK(r.ok());
        CHECK(r.value().action == SyncFileResult::Action::Uploaded);
    }

    // =================================================================
    // Test 16: tryCall
    // =================================================================
    {
        std::cout << "\n[TRY-16] tryCall\n";
        std::string msg = "hello tryCall";
        ByteBuffer payload(msg.size());
        std::memcpy(payload.data(), msg.data(), msg.size());

        auto r = client.tryCall("echo", std::move(payload)).get();
        CHECK(r.ok());
        CHECK(r.value().statusCode == 0);

        // missing method → statusCode != 0
        ByteBuffer empty;
        auto r2 = client.tryCall("does.not.exist", std::move(empty)).get();
        CHECK(r2.ok());   // network ok, but statusCode != 0
        CHECK(r2.value().statusCode != 0);
    }

    // =================================================================
    // Test 17: tryConnectAsync
    // =================================================================
    {
        std::cout << "\n[TRY-17] tryConnectAsync\n";
        Client c;
        ConnectOptions o;
        o.authToken = "secret123";
        auto r = c.tryConnectAsync("127.0.0.1", port, o).get();
        CHECK(r.ok());
        CHECK(!r.value().serverVersion.empty());
    }

    // =================================================================
    // Shutdown
    // =================================================================
    client.disconnect();
    server.stop();
    fs::remove_all(base);

    if (failures == 0) {
        std::cout << "\n=== ALL TRY TESTS PASSED ===\n";
        return 0;
    }
    std::cout << "\n=== " << failures << " TRY TEST(S) FAILED ===\n";
    return 1;
}
