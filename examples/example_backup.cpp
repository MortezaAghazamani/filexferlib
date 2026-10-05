// Example: back up a local folder to a remote server.
// Compares by XXHash, uploads only changed files, shows progress.
//
// Usage:
//   example_backup <host> <port> <local-dir> <remote-dir>

#include <filexferlib/client.hpp>
#include <filexferlib/logger.hpp>
#include <iostream>

using namespace filexferlib;

int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "usage: example_backup <host> <port> "
                     "<local-dir> <remote-dir>\n";
        return 1;
    }

    std::string host = argv[1];
    std::uint16_t port = static_cast<std::uint16_t>(std::stoi(argv[2]));
    std::string local = argv[3];
    std::string remote = argv[4];

    Logger::instance().setSink([](LogLevel, const std::string& m){
        std::cout << m << "\n";
    });
    Logger::instance().setLevel(LogLevel::Info);

    Client client;
    ConnectOptions opts;
    opts.authToken = "my-secret";
    opts.autoReconnect = true;
    opts.maxReconnectAttempts = 10;

    try {
        client.connect(host, port, opts);
    } catch (const std::exception& e) {
        std::cerr << "connect failed: " << e.what() << "\n";
        return 1;
    }

    // Upload sync: local is the source of truth.
    // Files missing on remote are uploaded.
    // Files missing on local are NOT deleted from remote.
    SyncOptions sopts;
    sopts.mode = SyncMode::Upload;
    sopts.compareBy = CompareMethod::XXHash;
    sopts.preserveTimes = true;
    sopts.verifyHash = true;

    auto r = client.syncFolder(local, remote, sopts,
        [](const SyncProgress& sp) {
            std::cout << "\r" << sp.overallPercent() << "% "
                      << sp.currentOperation << " "
                      << sp.currentPath << "        " << std::flush;
        }).get();

    std::cout << "\n"
              << "uploaded=" << r.uploaded << " "
              << "skipped=" << r.skipped << " "
              << "failed=" << r.failed << "\n";

    for (auto& e : r.errors) {
        std::cerr << "error: " << e << "\n";
    }

    client.disconnect();
    return r.failed == 0 ? 0 : 1;
}
