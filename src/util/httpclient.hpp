#pragma once

#include <string>

namespace mwm
{
struct HttpGetResult
{
    int status = 0;
    std::string body;
    std::string error;
};

HttpGetResult httpGet(std::string const& url, int timeoutMs = 1000);
} // namespace mwm
