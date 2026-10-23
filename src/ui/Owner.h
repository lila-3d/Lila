#pragma once

namespace lila::ext
{
class Context;
}

namespace lila::ui
{
/** The owner Lila's own UI registrations carry; an extension's carry its id, so unloading it removes them. */
inline constexpr const char* kLilaOwner = "lila";
} // namespace lila::ui
