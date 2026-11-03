#pragma once

#include "ops/Command.h"

#include <string>
#include <utility>

namespace lila::gn
{
/** "node.socket" split at the first dot; a CommandError naming cmd when either part is missing. */
inline std::pair<std::string, std::string> splitPlug(const std::string& s, const char* cmd)
{
    auto dot = s.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == s.size())
        throw CommandError(std::string(cmd) + ": expected node.socket, got '" + s + "'");
    return {s.substr(0, dot), s.substr(dot + 1)};
}
} // namespace lila::gn
