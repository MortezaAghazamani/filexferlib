// Minimal filexferlib server example.
//
// Starts a server on port 9000 with optional auth, an optional
// bandwidth limit, and graceful shutdown on Ctrl+C.

#include <filexferlib/server.hpp>
#include <filexferlib/logger.hpp>
#include <filexferlib/detail/protocol.hpp>

#include <iostream>
#include <cstring>
#include <thread>
#include <atomic>
#include <csignal>
#include <filesystem>
#include <condition_variable>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace filexferlib;

static std::condition_variable g_shutdownCv;
static std::mutex              g_shutdownMu;
static std::atomic<bool>       g_shutdown{false};

static void handle_signal(int) {
    g_shutdown.store(true);
    g_shutdownCv.notify_all();
}

int main() {
    Logger::instance().setSink([](LogLevel, const std::string& msg){
        std::cout << msg << "\n";
    });
    Logger::instance().setLevel(LogLevel::Info);

    std::signal(SIGINT,  handle_signal);
    std::signal(SIGTERM, handle_signal);
#ifdef _WIN32
    SetConsoleCtrlHandler([](DWORD type) -> BOOL {
        if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
            type == CTRL_CLOSE_EVENT)
        {
            g_shutdown.store(true);
            g_shutdownCv.notify_all();
            return TRUE;
        }
        return FALSE;
    }, TRUE);
#endif

    ServerConfig cfg;
    cfg.rootDir = std::filesystem::current_path() / "server_root";
    std::filesystem::create_directories(cfg.rootDir);
    cfg.requiredAuthToken = "";               // set to enable auth
    cfg.maxBytesPerSecond = 0;                // 0 = unlimited
    cfg.downloadWindowSize = 4;

    Server server(cfg);

    // Generic handlers
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

    auto port = server.start("127.0.0.1", 9000);
    FX_INFO("server listening on 127.0.0.1:", port);
    FX_INFO("root dir: ", cfg.rootDir.string());
    FX_INFO("press Ctrl+C to stop");

    {
        std::unique_lock<std::mutex> lk(g_shutdownMu);
        g_shutdownCv.wait(lk, []{ return g_shutdown.load(); });
    }

    FX_INFO("shutting down...");
    server.stop();
    FX_INFO("done");
    return 0;
}
