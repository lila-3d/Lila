#pragma once

#include "core/Export.h"

#include <cstddef>

namespace lila
{
/** The machine's physical memory in bytes; 0 when it can't be read. */
LILA_FOUNDATION_API size_t physicalMemoryBytes();
} // namespace lila
