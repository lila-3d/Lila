#pragma once

#include <filesystem>
#include <string>

namespace terrain
{
/** User agent for every request: names Lila and where to reach its maintainers, as Nominatim's policy asks. */
const char* httpUserAgent();

/**
 * GET url into dest (Qt Network, redirects followed, 120 s without progress times out). Returns the HTTP status, or 0
 * with why set when the server was never reached. Blocks, running a local event loop.
 */
int httpGet(const std::string& url, const std::filesystem::path& dest, std::string* why);

/** GET url as text; status as httpGet. */
int httpGetText(const std::string& url, std::string* body, std::string* why);
} // namespace terrain
