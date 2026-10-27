#include "core/Version.h"

#include "LilaBuildInfo.h"

namespace lila::version
{
const char* number()
{
    return LILA_VERSION;
}

const char* gitSha()
{
    return LILA_GIT_SHA;
}

const char* buildId()
{
    return LILA_BUILD_ID;
}

std::string full()
{
    return std::string(LILA_VERSION) + "+" + LILA_GIT_SHA + " (build " + LILA_BUILD_ID + ")";
}
} // namespace lila::version
