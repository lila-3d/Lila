#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

class QWidget;

/**
 * Update checks against latest.json on the newest GitHub release, attached by Release.cmd or the release workflow:
 *   {"version": "0.2.0", "notes": "...", "page": "<release page>", "installer": "<Setup.exe url>", "sha256": "..."}
 * A newer version shows a notice. With an installer URL, Lila downloads it, checks the hash and runs it silently once
 * the last Lila has closed (after a crash, at the next clean exit); without one it only opens the release page. Off
 * for Steam and source builds (Steam updates itself), with /update/check off, or with LILA_NO_UPDATE_CHECK set.
 * LILA_UPDATE_URL points it at another manifest.
 */
namespace lila::ui::updater
{
inline constexpr const char* kCheckPref = "/update/check";
inline constexpr const char* kSkipPref = "/update/skipVersion";
/** The downloaded, verified installer waiting for Lila to close. */
inline constexpr const char* kPendingPref = "/update/pendingInstaller";

/** -1, 0 or 1 comparing dotted versions ("0.10.0" > "0.9.2"); missing parts count as 0. */
LILA_UI_API int compareVersions(std::string_view a, std::string_view b);

struct Release
{
    std::string version;
    std::string notes;
    std::string page;
    std::string installer;
    std::string sha256;
};

/** Empty version when the manifest is unusable. */
LILA_UI_API Release parseManifest(const nlohmann::json& j);

/** The startup check, a few seconds after launch. Quiet when there's nothing new or the check fails. */
LILA_UI_API void startAutoCheck(QWidget* window);
/** Help > Check for Updates: also says when Lila is up to date or the check failed. */
LILA_UI_API void checkNow(QWidget* window);
/** Runs a downloaded update's installer silently, detached, once no other Lila is running; call as Lila exits. */
LILA_UI_API void installPendingOnExit();
/**
 * True while an update is installing: Lila must not start on files being replaced. It asks the installer to start
 * Lila when it finishes; the caller exits.
 */
LILA_UI_API bool deferToRunningInstall();
} // namespace lila::ui::updater
