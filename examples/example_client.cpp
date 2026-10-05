// Minimal filexferlib client example.
//
// Connects to a server, calls an echo method, optionally uploads a
// file passed on the command line, then lists the remote root.

#include <filexferlib/client.hpp>
#include <filexferlib/logger.hpp>

#include <iostream>
#include <cstring>
#include <filesystem>

using namespace filexferlib;

int main(int argc, char** argv) {
    Logger::instance().setSink([](LogLevel, const std::string& msg){
        std::cout << msg << "\n";
    });
    Logger::instance().setLevel(LogLevel::Info);

    Client client;
    ConnectOptions opts;
    opts.authToken = "";
    opts.autoReconnect = false;

    try {
        auto info = client.connect("127.0.0.1", 9000, opts);
        std::cout << "connected: " << info.serverVersion << "\n";
    } catch (const std::exception& e) {
        std::cerr << "connect failed: " << e.what() << "\n";
        return 1;
    }

    // echo test
    {
        std::string msg = "hello from client!";
        ByteBuffer payload(msg.size());
        std::memcpy(payload.data(), msg.data(), msg.size());
        auto resp = client.call("echo", std::move(payload)).get();
        std::string got(reinterpret_cast<const char*>(resp.payload.data()),
                        resp.payload.size());
        std::cout << "echo: " << got << "\n";
    }

    // upload if a file argument is given
    if (argc >= 2) {
        std::filesystem::path local = argv[1];
        std::string remote = argc >= 3
            ? argv[2]
            : ("/" + local.filename().string());

        auto fut = client.upload(local.string(), remote, {},
            [](const TransferProgress& p) {
                std::cout << "\r  " << p.percent() << "% ("
                          << p.speedMBps() << " MB/s)" << std::flush;
            });

        try {
            auto r = fut.get();
            std::cout << "\nuploaded " << r.bytesTransferred
                      << " bytes, verified=" << (r.verified ? "yes" : "no")
                      << "\n";
        } catch (const std::exception& e) {
            std::cerr << "\nupload failed: " << e.what() << "\n";
        }
    }

    // list remote
    {
        std::cout << "\nremote listing:\n";
        auto list = client.listRemote("/").get();
        for (auto& m : list) {
            std::cout << (m.isDirectory ? "d " : "- ")
                      << m.relativePath << "  (" << m.size << " bytes)\n";
        }
    }

    client.disconnect();
    return 0;
}
