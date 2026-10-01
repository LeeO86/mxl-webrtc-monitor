#pragma once

#include "picojson/picojson.h"

#include <map>
#include <string>

namespace mwm::json
{
inline picojson::value parse(std::string const& text, std::string* err)
{
    picojson::value value;
    std::string local;
    auto* target = err != nullptr ? err : &local;
    picojson::parse(value, text.begin(), text.end(), target);
    return value;
}

inline std::string escape(std::string const& text)
{
    return picojson::value(text).serialize();
}

inline std::string fieldString(picojson::value const& object, std::string const& key)
{
    if (!object.is<picojson::object>())
    {
        return {};
    }
    auto const& obj = object.get<picojson::object>();
    auto const it = obj.find(key);
    if (it == obj.end() || !it->second.is<std::string>())
    {
        return {};
    }
    return it->second.get<std::string>();
}
} // namespace mwm::json
