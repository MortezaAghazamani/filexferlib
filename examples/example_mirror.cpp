// Example: mirror a local folder to a remote server.
//
// This uploads every file from the local folder and then deletes
// any file on the remote that does not exist locally. The remote
// becomes an exact copy of the local folder.
//
// Warning: this deletes files on the remote. Use with care.
//
// Usage:
//   example_mirror <host> <port> <local-dir> <remote-dir>

#include <filexferlib/client.hpp>
#include <iostream>

using namespace filexferlib;

int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "usage: example_mirror <host> <port> "
                     "<local-dir> <remote-dir>\n";
        return 1;
    }

    std::string host = argv[1];
    std::uint16_t port = static_cast<std::uint16_t>(std::stoi(argv[2]));
    std::string local = argv[3];
    std::string remote = argv[4];

    Client client;
    ConnectOptions opts;
    opts.authToken = "my-secret";

    try {
        client.connect(host, port, opts);
    } catch (const std::exception& e) {
        std::cerr << "connect failed: " << e.what() << "\n";
        return 1;
    }

    // Mirror: local is the source of truth. Upload missing files,
    // then delete remote files that are not present locally.
    SyncOptions sopts;
    sopts.mode = SyncMode::MirrorUpload;
    sopts.compareBy = CompareMethod::XXHash;
    sopts.deleteExtra = true;

    auto r = client.syncFolder(local, remote, sopts,
        [](const SyncProgress& sp) {
            std::cout << sp.overallPercent() << "% "
                      << sp.currentOperation << " "
                      << sp.currentPath << "\n";
        }).get();

    std::cout << "uploaded=" << r.uploaded
              << " deleted=" << r.deleted
              << " skipped=" << r.skipped
              << " failed=" << r.failed << "\n";

    client.disconnect();
    return r.failed == 0 ? 0 : 1;
}
