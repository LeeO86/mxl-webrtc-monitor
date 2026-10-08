#include "util/httpclient.hpp"

#include "util/net.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

namespace mwm
{
std::string decodeChunked(std::string const& body)
{
    std::string out;
    std::size_t pos = 0;
    while (pos < body.size())
    {
        auto const lineEnd = body.find("\r\n", pos);
        if (lineEnd == std::string::npos)
        {
            break;
        }
        std::size_t size = 0;
        try
        {
            size = std::stoul(body.substr(pos, lineEnd - pos), nullptr, 16);
        }
        catch (...)
        {
            break;
        }
        pos = lineEnd + 2;
        if (size == 0 || pos + size > body.size())
        {
            break;
        }
        out.append(body, pos, size);
        pos += size + 2;
    }
    return out;
}

HttpGetResult httpGet(std::string const& url, int timeoutMs)
{
    HttpGetResult result;
    auto parsed = splitHostPort(url);
    if (!parsed || parsed->second <= 0)
    {
        result.error = "bad url";
        return result;
    }
    std::string path = "/";
    auto const scheme = url.find("://");
    auto const start = scheme == std::string::npos ? 0 : scheme + 3;
    auto const slash = url.find('/', start);
    if (slash != std::string::npos)
    {
        path = url.substr(slash);
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* info = nullptr;
    auto const port = std::to_string(parsed->second);
    if (::getaddrinfo(parsed->first.c_str(), port.c_str(), &hints, &info) != 0)
    {
        result.error = "resolve failed";
        return result;
    }
    int fd = -1;
    for (auto* it = info; it != nullptr; it = it->ai_next)
    {
        fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0)
        {
            continue;
        }
        timeval tv{};
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (::connect(fd, it->ai_addr, it->ai_addrlen) == 0)
        {
            break;
        }
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(info);
    if (fd < 0)
    {
        result.error = "connect failed";
        return result;
    }
    std::string request = "GET " + path + " HTTP/1.1\r\nHost: " + parsed->first + "\r\nConnection: close\r\n\r\n";
    if (::send(fd, request.data(), request.size(), MSG_NOSIGNAL) < 0)
    {
        ::close(fd);
        result.error = "send failed";
        return result;
    }
    std::string raw;
    char buf[2048];
    while (true)
    {
        auto const n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
        {
            break;
        }
        raw.append(buf, static_cast<std::size_t>(n));
        if (raw.size() > 2 * 1024 * 1024)
        {
            break;
        }
    }
    ::close(fd);
    auto const headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos)
    {
        result.error = "short response";
        return result;
    }
    std::istringstream line(raw);
    std::string version;
    line >> version >> result.status;
    result.body = raw.substr(headerEnd + 4);
    // MediaMTX sends larger answers (GET /v3/paths/list with two or more paths) chunked.
    auto headers = raw.substr(0, headerEnd);
    std::transform(headers.begin(), headers.end(), headers.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (headers.find("transfer-encoding: chunked") != std::string::npos)
    {
        result.body = decodeChunked(result.body);
    }
    return result;
}
} // namespace mwm
