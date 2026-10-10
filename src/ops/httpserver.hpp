#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace mwm
{
struct HttpRequest
{
    std::string method;
    std::string path;
    std::string query;
    std::string body;
    std::optional<std::string> header(std::string const& name) const;
    std::vector<std::pair<std::string, std::string>> headers;
};

struct HttpResponse
{
    int status = 200;
    std::string contentType = "application/json; charset=utf-8";
    std::string body;
    bool close = false;
    std::vector<std::pair<std::string, std::string>> headers{};
};

using HttpHandler = std::function<HttpResponse(HttpRequest const&)>;

class HttpServer
{
public:
    HttpServer();
    ~HttpServer();

    void setHandler(HttpHandler handler);
    bool start(int port);
    void stop();
    int port() const;
    void broadcast(std::string const& text);

private:
    void acceptLoop();
    void handleConnection(int fd);

    HttpHandler handler_;
    int port_ = 0;
    int listenFd_ = -1;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex clientsMu_;
    std::vector<int> clients_;
};
} // namespace mwm
