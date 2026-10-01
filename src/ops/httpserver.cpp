#include "ops/httpserver.hpp"

#include "util/logging.hpp"
#include "util/sha1.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace mwm
{
namespace
{
std::string lower(std::string text)
{
    for (auto& c : text)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

char const* statusText(int status)
{
    switch (status)
    {
    case 200:
        return "OK";
    case 201:
        return "Created";
    case 204:
        return "No Content";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 409:
        return "Conflict";
    case 500:
        return "Internal Server Error";
    case 503:
        return "Service Unavailable";
    default:
        return "Error";
    }
}

bool writeAll(int fd, char const* data, std::size_t size)
{
    std::size_t sent = 0;
    while (sent < size)
    {
        auto const n = ::send(fd, data + sent, size - sent, MSG_NOSIGNAL);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
        if (n == 0)
        {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool readSome(int fd, std::string& buffer)
{
    char tmp[4096];
    auto const n = ::recv(fd, tmp, sizeof(tmp), 0);
    if (n <= 0)
    {
        return false;
    }
    buffer.append(tmp, static_cast<std::size_t>(n));
    return true;
}

std::string websocketAccept(std::string const& key)
{
    auto const digest = sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    return base64Encode(digest.data(), digest.size());
}
} // namespace

std::optional<std::string> HttpRequest::header(std::string const& name) const
{
    auto const wanted = lower(name);
    for (auto const& [key, value] : headers)
    {
        if (lower(key) == wanted)
        {
            return value;
        }
    }
    return std::nullopt;
}

HttpServer::HttpServer() = default;

HttpServer::~HttpServer()
{
    stop();
}

void HttpServer::setHandler(HttpHandler handler)
{
    handler_ = std::move(handler);
}

int HttpServer::port() const
{
    return port_;
}

bool HttpServer::start(int port)
{
    listenFd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listenFd_ < 0)
    {
        return false;
    }
    int const one = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&bound), &len) == 0)
    {
        port_ = ntohs(bound.sin_port);
    }
    else
    {
        port_ = port;
    }
    if (::listen(listenFd_, 64) != 0)
    {
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    running_.store(true);
    thread_ = std::thread([this] { acceptLoop(); });
    return true;
}

void HttpServer::stop()
{
    if (!running_.exchange(false) && listenFd_ < 0)
    {
        return;
    }
    if (listenFd_ >= 0)
    {
        ::shutdown(listenFd_, SHUT_RDWR);
        ::close(listenFd_);
        listenFd_ = -1;
    }
    if (thread_.joinable())
    {
        thread_.join();
    }
    std::lock_guard const lock{clientsMu_};
    for (int fd : clients_)
    {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }
    clients_.clear();
}

void HttpServer::broadcast(std::string const& text)
{
    std::vector<int> dead;
    std::lock_guard const lock{clientsMu_};
    for (int fd : clients_)
    {
        std::string frame;
        frame.push_back(static_cast<char>(0x81));
        auto const size = text.size();
        if (size < 126)
        {
            frame.push_back(static_cast<char>(size));
        }
        else if (size <= 65535)
        {
            frame.push_back(126);
            frame.push_back(static_cast<char>((size >> 8) & 0xff));
            frame.push_back(static_cast<char>(size & 0xff));
        }
        else
        {
            frame.push_back(127);
            for (int shift = 56; shift >= 0; shift -= 8)
            {
                frame.push_back(static_cast<char>((static_cast<unsigned long long>(size) >> shift) & 0xff));
            }
        }
        frame += text;
        if (!writeAll(fd, frame.data(), frame.size()))
        {
            dead.push_back(fd);
        }
    }
    for (int fd : dead)
    {
        ::close(fd);
        clients_.erase(std::remove(clients_.begin(), clients_.end(), fd), clients_.end());
    }
}

void HttpServer::acceptLoop()
{
    while (running_.load())
    {
        sockaddr_in client{};
        socklen_t len = sizeof(client);
        int const fd = ::accept(listenFd_, reinterpret_cast<sockaddr*>(&client), &len);
        if (fd < 0)
        {
            if (!running_.load())
            {
                break;
            }
            continue;
        }
        std::thread(&HttpServer::handleConnection, this, fd).detach();
    }
}

void HttpServer::handleConnection(int fd)
{
    std::string buffer;
    bool websocket = false;
    while (running_.load())
    {
        if (!websocket)
        {
            auto const headerEnd = buffer.find("\r\n\r\n");
            if (headerEnd == std::string::npos)
            {
                if (!readSome(fd, buffer))
                {
                    break;
                }
                if (buffer.size() > 1024 * 1024)
                {
                    break;
                }
                continue;
            }
            std::string const head = buffer.substr(0, headerEnd);
            std::istringstream lines(head);
            std::string requestLine;
            std::getline(lines, requestLine);
            if (!requestLine.empty() && requestLine.back() == '\r')
            {
                requestLine.pop_back();
            }
            std::istringstream req(requestLine);
            HttpRequest request;
            std::string target;
            req >> request.method >> target;
            auto const q = target.find('?');
            if (q == std::string::npos)
            {
                request.path = target;
            }
            else
            {
                request.path = target.substr(0, q);
                request.query = target.substr(q + 1);
            }
            std::string headerLine;
            std::size_t contentLength = 0;
            while (std::getline(lines, headerLine))
            {
                if (!headerLine.empty() && headerLine.back() == '\r')
                {
                    headerLine.pop_back();
                }
                auto const colon = headerLine.find(':');
                if (colon == std::string::npos)
                {
                    continue;
                }
                auto key = headerLine.substr(0, colon);
                auto value = headerLine.substr(colon + 1);
                while (!value.empty() && value.front() == ' ')
                {
                    value.erase(value.begin());
                }
                request.headers.emplace_back(key, value);
                if (lower(key) == "content-length")
                {
                    contentLength = static_cast<std::size_t>(std::strtoul(value.c_str(), nullptr, 10));
                }
            }
            auto const bodyStart = headerEnd + 4;
            while (buffer.size() < bodyStart + contentLength)
            {
                if (!readSome(fd, buffer))
                {
                    ::close(fd);
                    return;
                }
            }
            request.body = buffer.substr(bodyStart, contentLength);
            buffer.erase(0, bodyStart + contentLength);

            auto const upgrade = request.header("upgrade");
            if (upgrade && lower(*upgrade) == "websocket" && request.path == "/api/v1/events")
            {
                auto const key = request.header("sec-websocket-key");
                if (!key)
                {
                    break;
                }
                std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ";
                response += websocketAccept(*key);
                response += "\r\n\r\n";
                if (!writeAll(fd, response.data(), response.size()))
                {
                    break;
                }
                {
                    std::lock_guard const lock{clientsMu_};
                    clients_.push_back(fd);
                }
                websocket = true;
                continue;
            }

            HttpResponse response;
            if (handler_)
            {
                try
                {
                    response = handler_(request);
                }
                catch (std::exception const& ex)
                {
                    response.status = 500;
                    response.contentType = "application/json";
                    response.body = std::string("{\"error\":\"") + log::jsonEscape(ex.what()) + "\"}";
                }
            }
            else
            {
                response.status = 404;
                response.body = "{\"error\":\"not found\"}";
            }
            std::ostringstream out;
            out << "HTTP/1.1 " << response.status << " " << statusText(response.status) << "\r\n";
            out << "Content-Type: " << response.contentType << "\r\n";
            out << "Content-Length: " << response.body.size() << "\r\n";
            out << "Connection: close\r\n\r\n";
            auto const header = out.str();
            writeAll(fd, header.data(), header.size());
            if (!response.body.empty())
            {
                writeAll(fd, response.body.data(), response.body.size());
            }
            break;
        }
        else
        {
            if (!readSome(fd, buffer))
            {
                break;
            }
            if (buffer.size() > 65536)
            {
                buffer.clear();
            }
        }
    }
    if (websocket)
    {
        std::lock_guard const lock{clientsMu_};
        clients_.erase(std::remove(clients_.begin(), clients_.end(), fd), clients_.end());
    }
    ::close(fd);
}
} // namespace mwm
