#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <QString>

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

/**
 * The credits: corporate sponsors, supporters by tier, contributors and testers, from credits.json published by the
 * telemetry Worker. Releases bundle a snapshot without links; links only ever come from the live file, so pulling one
 * on the Worker reaches every copy of Lila.
 */
namespace lila::ui::credits
{
inline constexpr const char* kUrl = "https://lila-telemetry.lila3d.workers.dev/credits.json";
inline constexpr const char* kReportUrl = "https://lila-telemetry.lila3d.workers.dev/credits/report";

/** Named tiers, lowest first. Backer is a badge with no name, so it never appears in the credits. */
enum class Tier
{
    Supporter,
    Patron,
    Legendary,
    Mythic,
    Eternal,
};

struct Supporter
{
    std::string name;
    Tier tier = Tier::Supporter;
    /** Legendary and up: the permanent order they reached Legendary in, from 1. */
    int number = 0;
    /** Supported at Supporter tier or above before 1.0. */
    bool founder = false;
    std::string url;
};

struct Sponsor
{
    std::string name;
    /** Absolute: a relative path in the file resolves against the file's URL. */
    std::string logo;
    std::string url;
};

struct Contributor
{
    std::string name;
    std::string role;
    std::vector<std::string> areas;
    std::string url;
};

struct Tester
{
    std::string name;
    /** Tested before 1.0. */
    bool founding = false;
};

struct Credits
{
    std::vector<Sponsor> corporate;
    std::vector<Sponsor> pastSponsors;
    /** Highest tier first, then by number, then by name. */
    std::vector<Supporter> supporters;
    std::vector<Contributor> contributors;
    /** Founding testers first. */
    std::vector<Tester> testers;

    bool empty() const
    {
        return corporate.empty() && pastSponsors.empty() && supporters.empty() && contributors.empty() && testers.empty();
    }

    std::vector<Supporter> tier(Tier t) const
    {
        std::vector<Supporter> out;
        for (const Supporter& s : supporters)
            if (s.tier == t)
                out.push_back(s);
        return out;
    }
};

LILA_UI_API QString tierTitle(Tier t);

/** Whether a link may be shown: https only, and for anyone but a corporate sponsor a profile on an allowed platform. */
LILA_UI_API bool linkAllowed(std::string_view url, bool corporate);
/** The host a link opens, for its tooltip. */
LILA_UI_API std::string linkHost(std::string_view url);

/** Reads credits.json. withLinks false drops every link, for the bundled snapshot. Entries without a name are skipped. */
LILA_UI_API Credits parse(const nlohmann::json& file, bool withLinks, std::string_view fileUrl = kUrl);

/**
 * The names for the welcome screen's "Made possible by" line: every Eternal and Mythic supporter, then one Legendary,
 * chosen by rotation so each showing names the next.
 */
LILA_UI_API std::vector<std::string> madePossibleBy(const Credits& c, int rotation);

/** The credits shipped with this release (no links). */
LILA_UI_API std::filesystem::path bundledFile();
/** The live credits as last fetched, or the bundled snapshot when they never have been. */
LILA_UI_API Credits current();
/** The downloaded logo of a sponsor, or empty until it is. */
LILA_UI_API QString logoFile(const Sponsor& s);

/** Fetches credits.json and any logos not yet downloaded, then calls done on the main thread. */
LILA_UI_API void refresh(std::function<void(const Credits&)> done);
/** refresh, only when the cached copy is missing or more than an hour old. done is not called otherwise. */
LILA_UI_API void refreshIfStale(std::function<void(const Credits&)> done);

/** Sends a report about a link to the Worker; done gets whether it arrived. */
LILA_UI_API void report(const std::string& name, const std::string& url, std::function<void(bool)> done);

/** The third-party licence notices shipped beside Lila, or empty when they can't be found. */
LILA_UI_API std::filesystem::path licenceNotices();
} // namespace lila::ui::credits
