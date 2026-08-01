#include "http/http_server.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace stratadb {
namespace {

constexpr std::size_t kMaxRequestBytes = 4 * 1024 * 1024;

struct Request {
    std::string method;
    std::string target;
    std::string body;
};

std::string html() {
    return R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>StrataDB</title><style>body{margin:0;background:#07090d;color:#eef4f6;font:16px system-ui;padding:40px}main{max-width:760px;margin:auto}h1{font-size:52px;margin:0;color:#18d69b}p{color:#91a0a6}.card{border:1px solid #29404a;background:#0d141a;padding:22px;margin-top:20px}code{color:#24c8ee}</style></head><body><main><div class="card"><h1>StrataDB</h1><p>C++20 durable key-value storage engine</p><p><code>WAL enabled</code> · thread-safe · HTTP API online</p></div></main></body></html>)HTML";
}

bool send_all(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto result = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (result <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(result);
    }
    return true;
}

void response(int fd, int status, const std::string& content_type, const std::string& body) {
    const std::string status_text = status == 200 ? "OK" : status == 201 ? "Created" : status == 204 ? "No Content" : status == 404 ? "Not Found" : status == 400 ? "Bad Request" : "Internal Server Error";
    std::ostringstream output;
    output << "HTTP/1.1 " << status << ' ' << status_text << "\r\n"
           << "Content-Type: " << content_type << "\r\n"
           << "Content-Length: " << body.size() << "\r\n"
           << "Connection: close\r\n\r\n" << body;
    send_all(fd, output.str());
}

Request parse_request(const std::string& raw) {
    const auto header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        throw std::invalid_argument("incomplete HTTP request");
    }
    const auto line_end = raw.find("\r\n");
    std::istringstream line(raw.substr(0, line_end));
    Request request;
    std::string version;
    line >> request.method >> request.target >> version;
    if (request.method.empty() || request.target.empty()) {
        throw std::invalid_argument("invalid HTTP request line");
    }
    request.body = raw.substr(header_end + 4);
    return request;
}

}  // namespace

HttpServer::HttpServer(KeyValueStore& store, std::uint16_t port) : store_(store), port_(port) {}

HttpServer::~HttpServer() {
    stop();
}

void HttpServer::run() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
    }
    int reuse = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port_);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 || ::listen(listen_fd_, 128) < 0) {
        throw std::runtime_error(std::string("bind/listen: ") + std::strerror(errno));
    }
    running_ = true;
    while (running_) {
        const int client = ::accept(listen_fd_, nullptr, nullptr);
        if (client < 0) {
            if (running_) {
                continue;
            }
            break;
        }
        std::thread(&HttpServer::handle_client, this, client).detach();
    }
}

void HttpServer::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

void HttpServer::handle_client(int client_fd) {
    ++requests_total_;
    std::string raw;
    char buffer[8192];
    while (raw.find("\r\n\r\n") == std::string::npos && raw.size() < kMaxRequestBytes) {
        const auto received = ::recv(client_fd, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            ::close(client_fd);
            return;
        }
        raw.append(buffer, static_cast<std::size_t>(received));
    }
    try {
        const auto header_end = raw.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            throw std::invalid_argument("request headers too large or incomplete");
        }
        std::size_t content_length = 0;
        const auto content_length_position = raw.find("Content-Length:");
        if (content_length_position != std::string::npos && content_length_position < header_end) {
            const auto value_start = content_length_position + std::strlen("Content-Length:");
            const auto value_end = raw.find("\r\n", value_start);
            content_length = static_cast<std::size_t>(std::stoul(raw.substr(value_start, value_end - value_start)));
        }
        const auto request_size = header_end + 4 + content_length;
        while (raw.size() < request_size && raw.size() < kMaxRequestBytes) {
            const auto received = ::recv(client_fd, buffer, sizeof(buffer), 0);
            if (received <= 0) {
                throw std::invalid_argument("incomplete HTTP body");
            }
            raw.append(buffer, static_cast<std::size_t>(received));
        }
        if (raw.size() < request_size) {
            throw std::invalid_argument("request body too large or incomplete");
        }
        const auto request = parse_request(raw);
        if (request.target == "/") {
            response(client_fd, 200, "text/html; charset=utf-8", html());
        } else if (request.target == "/health") {
            response(client_fd, 200, "application/json", "{\"status\":\"ok\"}");
        } else if (request.target == "/metrics") {
            response(client_fd, 200, "text/plain; version=0.0.4", "stratadb_http_requests_total " + std::to_string(requests_total_.load()) + "\nstratadb_keys " + std::to_string(store_.size()) + "\n");
        } else if (request.target.rfind("/kv/", 0) == 0) {
            const auto key = request.target.substr(4);
            if (key.empty() || key.find('/') != std::string::npos) {
                response(client_fd, 400, "text/plain", "invalid key\n");
            } else if (request.method == "GET") {
                const auto value = store_.get(key);
                response(client_fd, value ? 200 : 404, "text/plain; charset=utf-8", value.value_or("not found\n"));
            } else if (request.method == "PUT") {
                store_.put(key, request.body);
                response(client_fd, 201, "application/json", "{\"status\":\"stored\"}");
            } else if (request.method == "DELETE") {
                response(client_fd, store_.erase(key) ? 204 : 404, "text/plain", "");
            } else {
                response(client_fd, 400, "text/plain", "method not supported\n");
            }
        } else {
            response(client_fd, 404, "text/plain", "not found\n");
        }
    } catch (const std::exception& error) {
        response(client_fd, 400, "text/plain", std::string(error.what()) + "\n");
    }
    ::shutdown(client_fd, SHUT_RDWR);
    ::close(client_fd);
}

}  // namespace stratadb
