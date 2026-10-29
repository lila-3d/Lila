#pragma once

#include "core/Export.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

/**
 * Sending crash reports to Sentry, from the lilacrash watchdog once Lila has died.
 *
 * Lila keeps <userDir>/telemetry/crash-consent.json up to date with the user's choice; the watchdog reads it at crash
 * time, so a choice made after startup still counts. The minidump goes to Sentry's minidump endpoint (Sentry
 * symbolicates it and keeps only the event) with report.txt attached after sanitize() has taken paths, the user and
 * machine names out of it.
 */
namespace lila::crash::upload
{
namespace fs = std::filesystem;

inline constexpr const char* kConsentFile = "crash-consent.json";

struct Consent
{
    bool send = false;   // crash reports are on and nothing in the environment opts out
    bool always = false; // send without asking
    fs::path prefsFile;  // the user's general.json, where "always" is kept
};

LILA_CRASHKIT_API Consent readConsent(const fs::path& file);
LILA_CRASHKIT_API void writeConsent(const fs::path& file, const Consent& c);
/** Records "send without asking" in both the consent file and the user's prefs. */
LILA_CRASHKIT_API void setAlways(const fs::path& consentFile, bool on);

/**
 * report.txt as it may leave the machine: identifying fields dropped, every literal in `secrets` (user and machine
 * names) replaced, and file paths cut to the file name for code files and to "<path>" for anything else.
 */
LILA_CRASHKIT_API std::string sanitize(std::string_view reportText, const std::vector<std::string>& secrets);

#ifdef _WIN32
/** POSTs reportDir's minidump and sanitized report. Writes reportDir/.sent with Sentry's event id on success. */
LILA_CRASHKIT_API bool send(const fs::path& reportDir, std::string* eventId = nullptr, std::string* error = nullptr);
LILA_CRASHKIT_API bool alreadySent(const fs::path& reportDir);
#endif
} // namespace lila::crash::upload
