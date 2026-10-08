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

// A response of `Transfer-Encoding: chunked` has its body joined from the chunks.
HttpGetResult httpGet(std::string const& url, int timeoutMs = 1000);
// The data of a chunked body ("<hex size>\r\n<data>\r\n" ... "0\r\n\r\n"); a malformed rest is dropped.
std::string decodeChunked(std::string const& body);
} // namespace mwm
