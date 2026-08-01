#include "http/http_server.hpp"

#include <csignal>
#include <cstdlib>
#include <iostream>

namespace {
stratadb::HttpServer* active_server = nullptr;

void handle_signal(int) {
    if (active_server != nullptr) {
        active_server->stop();
    }
}
}  // namespace

int main(int argc, char** argv) {
    const std::string wal_path = argc > 1 ? argv[1] : "data/stratadb.wal";
    const auto port = static_cast<std::uint16_t>(argc > 2 ? std::atoi(argv[2]) : 8080);
    try {
        stratadb::KeyValueStore store(wal_path);
        stratadb::HttpServer server(store, port);
        active_server = &server;
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);
        std::cout << "StrataDB listening on port " << port << " with WAL " << wal_path << '\n';
        server.run();
    } catch (const std::exception& error) {
        std::cerr << "StrataDB fatal: " << error.what() << '\n';
        return 1;
    }
}
