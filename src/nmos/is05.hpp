#pragma once

#include <optional>
#include <string>

namespace mwm
{
struct Is05Activation
{
    bool master_enable = false;
    std::string domain_id;
    std::string flow_id;
    std::string sender_id;
    bool has_sender = false;
};

struct Is05Error
{
    int status = 400;
    std::string message;
};

// Validates one IS-05 connection PATCH body. Malformed non-UUID transport
// parameters are rejected. A missing domain or flow is accepted.
std::optional<Is05Error> validateIs05(std::string const& body, Is05Activation* activation);
std::string is05ErrorBody(Is05Error const& error);
} // namespace mwm
