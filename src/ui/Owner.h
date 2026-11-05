#pragma once

namespace lila::ext
{
class Context;
}

namespace lila::ui
{
/** The owner Lila's own UI registrations carry; an extension's carry its id, so unloading it removes them. */
inline constexpr const char* kLilaOwner = "lila";
/** The owner of what the user's own menus register (see UserMenus.h). */
inline constexpr const char* kUserMenusOwner = "userMenus";
} // namespace lila::ui
