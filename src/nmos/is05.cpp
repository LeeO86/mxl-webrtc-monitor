#include "nmos/is05.hpp"

#include "util/jsonutil.hpp"
#include "util/uuid.hpp"

#include <string>

namespace mwm
{
namespace
{
std::optional<Is05Error> readUuidField(picojson::value const& object, char const* key, std::string* out, bool* present)
{
    if (!object.is<picojson::object>() || !object.get<picojson::object>().count(key))
    {
        return std::nullopt;
    }
    auto const& value = object.get<picojson::object>().at(key);
    if (value.is<picojson::null>())
    {
        *out = {};
        if (present != nullptr)
        {
            *present = true;
        }
        return std::nullopt;
    }
    if (!value.is<std::string>() || !isUuid(value.get<std::string>()))
    {
        return Is05Error{400, std::string(key) + " must be a UUID"};
    }
    *out = parseUuid(value.get<std::string>()).value();
    if (present != nullptr)
    {
        *present = true;
    }
    return std::nullopt;
}
} // namespace

std::optional<Is05Error> validateIs05(std::string const& body, Is05Activation* activation)
{
    std::string err;
    auto const root = json::parse(body, &err);
    if (!err.empty() || !root.is<picojson::object>())
    {
        return Is05Error{400, "connection patch must be a JSON object"};
    }
    Is05Activation parsed;
    auto const& obj = root.get<picojson::object>();
    if (obj.count("master_enable"))
    {
        if (!obj.at("master_enable").is<bool>())
        {
            return Is05Error{400, "master_enable must be a boolean"};
        }
        parsed.master_enable = obj.at("master_enable").get<bool>();
    }
    if (auto const sender = readUuidField(root, "sender_id", &parsed.sender_id, &parsed.has_sender))
    {
        return sender;
    }
    if (obj.count("activation") && !obj.at("activation").is<picojson::null>())
    {
        if (!obj.at("activation").is<picojson::object>())
        {
            return Is05Error{400, "activation must be an object"};
        }
        auto const mode = json::fieldString(obj.at("activation"), "mode");
        if (!mode.empty() && mode != "activate_immediate" && mode != "activate_scheduled_absolute" && mode != "activate_scheduled_relative")
        {
            return Is05Error{400, "unsupported activation mode"};
        }
    }
    if (obj.count("transport_params"))
    {
        auto const& params = obj.at("transport_params");
        if (!params.is<picojson::array>() || params.get<picojson::array>().empty() || !params.get<picojson::array>().front().is<picojson::object>())
        {
            return Is05Error{400, "transport_params must be a non-empty array"};
        }
        auto const& leg = params.get<picojson::array>().front();
        if (auto const bad = readUuidField(leg, "mxl_domain_id", &parsed.domain_id, nullptr))
        {
            return bad;
        }
        if (auto const bad = readUuidField(leg, "mxl_flow_id", &parsed.flow_id, nullptr))
        {
            return bad;
        }
    }
    if (activation != nullptr)
    {
        *activation = parsed;
    }
    return std::nullopt;
}

std::string is05ErrorBody(Is05Error const& error)
{
    return std::string("{\"code\":") + std::to_string(error.status) + ",\"error\":\"" + error.message + "\",\"debug\":\"" + error.message + "\"}";
}
} // namespace mwm
