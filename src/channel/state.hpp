#pragma once

#include <cstdint>
#include <string>

namespace mwm
{
enum class LegKind
{
    Video,
    Audio,
};

enum class RunState
{
    NotRouted,
    Waiting,
    NoSignal,
    Running,
};

char const* stateName(RunState state);

struct LegInput
{
    bool master_enable = false;
    std::string domain_id;
    std::string flow_id;
    bool domain_resolved = false;
    bool flow_open = false;
    bool grains_flowing = false;
};

struct LegEval
{
    RunState state = RunState::NotRouted;
    std::string reason;
};

LegEval evaluateLeg(LegInput const& input);

// 250 ms, doubling, capped at 5 s.
int backoffMs(int attempt);

std::string formatString(int height, bool interlaced, std::int64_t rateNum, std::int64_t rateDen);
} // namespace mwm
