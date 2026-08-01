#pragma once

#include "storage/key_value_store.hpp"

#include <atomic>
#include <cstdint>

namespace stratadb {

class HttpServer {
public:
    HttpServer(KeyValueStore& store, std::uint16_t port);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    void run();
    void stop();

private:
    void handle_client(int client_fd);

    KeyValueStore& store_;
    std::uint16_t port_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> requests_total_{0};
    int listen_fd_{-1};
};

}  // namespace stratadb
