#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace lila::python
{
/** Main thread. Leaves the GIL released; use Gil to run anything. */
bool init(const std::vector<std::string>& argv);
void shutdown();
bool initialized();

struct Result
{
    bool ok = false;
    std::string value; // repr of the last expression, if any
};

/** Runs like the script editor: a lone expression is evaluated and its repr returned. Output goes to the log. */
Result run(std::string_view code, std::string_view filename = "<script>");
Result runFile(const std::filesystem::path& path);

/** The UI installs these so lila.ui can reach its action registry; headless they stay empty. */
struct UiHooks
{
    std::function<bool(const std::string&)> runAction;
    std::function<std::vector<std::array<std::string, 3>>()> listActions;
};
UiHooks& uiHooks();

/** RAII GIL acquire, usable from any thread. */
class Gil
{
public:
    Gil();
    ~Gil();
    Gil(const Gil&) = delete;
    Gil& operator=(const Gil&) = delete;

private:
    int mState;
};
} // namespace lila::python
