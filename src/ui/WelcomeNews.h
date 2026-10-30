#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <QString>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

/** The news feed on the welcome dialog: announcements, releases and community spotlights from the Lila site. */
namespace lila::ui::news
{
inline constexpr const char* kEnabledPref = "/startup/news";
inline constexpr const char* kFeedUrl = "https://lila-3d.github.io/news.json";

struct Item
{
    std::string id;
    /** news, release, spotlight or tutorial. */
    std::string type;
    std::string date;
    std::string title;
    std::string body;
    /** Who made the work, for a spotlight or tutorial. */
    std::string author;
    /** Absolute: a relative path in the feed resolves against the feed's URL. */
    std::string image;
    std::string url;
};

/** What to show from a feed: newest first, at most limit, without items whose minVersion or maxVersion excludes version. */
LILA_UI_API std::vector<Item> parse(const nlohmann::json& feed, std::string_view feedUrl, std::string_view version, size_t limit = 8);

/** The feed as last fetched, or nothing when it never has been or the pref is off. */
LILA_UI_API std::vector<Item> cached();

/** The downloaded image of an item, or empty until it is. */
LILA_UI_API QString imageFile(const Item& item);

/** Fetches the feed and any images not yet downloaded, then calls done with the items on the main thread. Off with the pref. */
LILA_UI_API void refresh(std::function<void(const std::vector<Item>&)> done);
} // namespace lila::ui::news
