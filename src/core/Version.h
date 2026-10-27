#pragma once

#include "core/Export.h"

#include <string>

namespace lila::version
{
/** major.minor.patch, as in "0.1.0". */
LILA_FOUNDATION_API const char* number();

/** The source commit, 10 hex digits, or "unknown" for a build outside git. */
LILA_FOUNDATION_API const char* gitSha();

/** The commit's time (UTC yyyyMMddHHmm) and sha, "-dirty" with uncommitted changes: "202610281430-9641744ab1". */
LILA_FOUNDATION_API const char* buildId();

/** "0.1.0+9641744ab1 (build 202610281430-9641744ab1)", for logs, crash reports and --version. */
LILA_FOUNDATION_API std::string full();
} // namespace lila::version
