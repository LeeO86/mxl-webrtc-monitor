#include "channel/state.hpp"

#include <string>

namespace mwm
{
char const* stateName(RunState state)
{
    switch (state)
    {
    case RunState::NotRouted:
        return "not_routed";
    case RunState::Waiting:
        return "waiting";
    case RunState::NoSignal:
        return "no_signal";
    case RunState::Running:
        return "running";
    }
    return "not_routed";
}

LegEval evaluateLeg(LegInput const& input)
{
    LegEval out;
    if (!input.master_enable || input.flow_id.empty() || input.domain_id.empty())
    {
        out.state = RunState::NotRouted;
        return out;
    }
    if (!input.domain_resolved)
    {
        out.state = RunState::Waiting;
        out.reason = "domain_not_found";
        return out;
    }
    if (!input.flow_open)
    {
        out.state = RunState::Waiting;
        out.reason = "flow_not_found";
        return out;
    }
    if (!input.grains_flowing)
    {
        out.state = RunState::NoSignal;
        return out;
    }
    out.state = RunState::Running;
    return out;
}

int backoffMs(int attempt)
{
    if (attempt < 0)
    {
        attempt = 0;
    }
    int shift = attempt;
    if (shift > 5)
    {
        shift = 5;
    }
    int ms = 250 << shift;
    if (ms > 5000)
    {
        ms = 5000;
    }
    return ms;
}

std::string formatString(int height, bool interlaced, std::int64_t rateNum, std::int64_t rateDen)
{
    std::int64_t num = rateNum;
    std::int64_t den = rateDen == 0 ? 1 : rateDen;
    if (interlaced)
    {
        num *= 2;
    }
    std::string rate;
    if (den == 1001 && (num == 24000 || num == 30000 || num == 48000 || num == 60000 || num == 120000))
    {
        if (num == 24000)
        {
            rate = "23.98";
        }
        else if (num == 30000)
        {
            rate = "29.97";
        }
        else if (num == 48000)
        {
            rate = "47.95";
        }
        else if (num == 60000)
        {
            rate = "59.94";
        }
        else
        {
            rate = "119.88";
        }
    }
    else if (num % den == 0)
    {
        rate = std::to_string(num / den);
    }
    else
    {
        rate = std::to_string(num) + "/" + std::to_string(den);
    }
    std::string name = std::to_string(height);
    if (height == 1080 || height == 2160 || height == 720)
    {
        name = std::to_string(height);
    }
    name += interlaced ? "i" : "p";
    name += rate;
    return name;
}
} // namespace mwm
