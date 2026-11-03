#pragma once

#include "core/Export.h"

#include <cctype>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace lila::vcs
{
struct ProcessSpec
{
    std::vector<std::string> argv; // argv[0] is looked up on PATH
    std::filesystem::path cwd;
    /** Added to (or replacing in) the inherited environment. */
    std::vector<std::pair<std::string, std::string>> env;
    /** Written to stdin, which is then closed. The child never gets an interactive stdin. */
    std::string input;
    int timeoutMs = 0; // 0 = wait forever
};

struct ProcessResult
{
    bool started = false;
    bool timedOut = false;
    int exitCode = -1;
    std::string out;
    std::string err;
};

/** Runs external tools. Injected so providers can be tested against canned output. Thread-safe. */
class ProcessRunner
{
public:
    virtual ~ProcessRunner() = default;
    virtual ProcessResult run(const ProcessSpec& spec) = 0;
};

/** Spawns real processes without a console window. */
LILA_SCENE_API std::shared_ptr<ProcessRunner> systemRunner();

/** Full path of an executable found on PATH, or empty. */
LILA_SCENE_API std::filesystem::path findExecutable(const std::string& name);

/** s without leading and trailing whitespace, for one line of a tool's output. */
inline std::string trim(std::string s)
{
    while (!s.empty() && std::isspace((unsigned char)s.back()))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace((unsigned char)s[i]))
        ++i;
    return s.substr(i);
}
} // namespace lila::vcs
