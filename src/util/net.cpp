#include "util/net.hpp"

#include <ifaddrs.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

#include <cctype>

namespace mwm
{
std::string hostname()
{
    char buf[256] = {};
    if (::gethostname(buf, sizeof(buf) - 1) != 0)
    {
        return "localhost";
    }
    buf[sizeof(buf) - 1] = 0;
    return buf;
}

std::string firstNonLoopbackIpv4()
{
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0)
    {
        return "127.0.0.1";
    }
    std::string found = "127.0.0.1";
    for (auto* it = list; it != nullptr; it = it->ifa_next)
    {
        if (it->ifa_addr == nullptr || it->ifa_addr->sa_family != AF_INET)
        {
            continue;
        }
        auto const* addr = reinterpret_cast<sockaddr_in const*>(it->ifa_addr);
        char text[INET_ADDRSTRLEN] = {};
        if (::inet_ntop(AF_INET, &addr->sin_addr, text, sizeof(text)) == nullptr)
        {
            continue;
        }
        std::string const ip = text;
        if (ip.rfind("127.", 0) == 0)
        {
            continue;
        }
        found = ip;
        break;
    }
    ::freeifaddrs(list);
    return found;
}

std::optional<std::pair<std::string, int>> splitHostPort(std::string text)
{
    auto const scheme = text.find("://");
    if (scheme != std::string::npos)
    {
        text = text.substr(scheme + 3);
    }
    auto const slash = text.find('/');
    if (slash != std::string::npos)
    {
        text = text.substr(0, slash);
    }
    if (text.empty())
    {
        return std::nullopt;
    }
    std::string host = text;
    int port = 0;
    auto const colon = text.rfind(':');
    if (colon != std::string::npos && text.find(':') == colon)
    {
        host = text.substr(0, colon);
        try
        {
            port = std::stoi(text.substr(colon + 1));
        }
        catch (...)
        {
            return std::nullopt;
        }
    }
    if (host.empty())
    {
        host = "0.0.0.0";
    }
    return std::make_pair(host, port);
}
} // namespace mwm
