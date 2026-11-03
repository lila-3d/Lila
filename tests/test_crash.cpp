#include <doctest/doctest.h>

#include "TestUtil.h"

#include "core/CrashHandler.h"
#include "core/CrashReport.h"
#include "core/Paths.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace lila;
using namespace testutil;
namespace fs = std::filesystem;

namespace
{
fs::path crashDir(const std::string& name)
{
    // Per process: other test runs may share the temp folder.
    static const std::string run = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::path dir = fs::temp_directory_path() / "lila_tests" / ("crash_" + run) / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

crash::Report fakeReport()
{
    crash::Report r;
    crash::Section& c = r.section("crash");
    c.set("exception.code", "0xC0000374");
    c.set("exception.name", "STATUS_HEAP_CORRUPTION");
    c.set("exception.description", "heap corruption");
    c.set("module", "ntdll.dll");
    c.set("module.offset", "0x1037F1");
    r.section("log").line("12:00:00.000 info: hello");
    r.section("log").line("12:00:01.000 error: Traceback\n  File \"x.py\", line 1");
    r.section("stack").line("#00 ntdll.dll!RtlReportFatalFailure+0x9  ");
    r.section("stack").line("#01 KERNELBASE.dll!RaiseFailFastException+0x12");
    r.section("stack").line("#02 lila.exe!lila::geo::Mesh::weld+0x1a  X:\\Lila\\src\\geo\\Mesh.cpp:120");
    r.section("app").set("version", "0.1.0");
    return r;
}
} // namespace

TEST_CASE("crash report renders and parses back")
{
    crash::Report r = fakeReport();
    r.summary = crash::summarize(r);
    CHECK(r.summary == "Lila crashed: heap corruption (0xC0000374 STATUS_HEAP_CORRUPTION) in ntdll.dll+0x1037F1, at "
                       "lila.exe!lila::geo::Mesh::weld (X:\\Lila\\src\\geo\\Mesh.cpp:120)");
    crash::sortSections(r);
    REQUIRE(r.sections.size() == 4);
    CHECK(r.sections[0].name == "crash");
    CHECK(r.sections[1].name == "stack");
    CHECK(r.sections[2].name == "app");
    CHECK(r.sections[3].name == "log");

    std::string text = crash::renderReport(r);
    CHECK(text.starts_with("Lila crash report\nsummary: Lila crashed: heap corruption"));
    CHECK(contains(text, "\n[crash]\nexception.code: 0xC0000374\n"));
    // A multi-line entry becomes several indented lines rather than breaking the format.
    CHECK(contains(text, "  12:00:01.000 error: Traceback\n    File \"x.py\", line 1\n"));

    crash::Report back = crash::parseReport(text);
    CHECK(back.summary == r.summary);
    REQUIRE(back.find("crash"));
    CHECK(back.find("crash")->get("module") == "ntdll.dll");
    CHECK(back.find("app")->get("version") == "0.1.0");
    CHECK(back.find("log")->lines.size() == 3);
    CHECK(back.find("stack")->lines.size() == 3);
}

TEST_CASE("crash report merge replaces fields and line blocks")
{
    crash::Report a = fakeReport();
    crash::Report b;
    b.section("crash").set("module", "lila.exe");
    b.section("crash").set("detected.by", "watchdog");
    b.section("log").line("only line");
    a.merge(b);
    CHECK(a.find("crash")->get("module") == "lila.exe");
    CHECK(a.find("crash")->get("exception.code") == "0xC0000374");
    CHECK(a.find("crash")->get("detected.by") == "watchdog");
    CHECK(a.find("log")->lines == std::vector<std::string>{"only line"});
    CHECK(a.find("stack")->lines.size() == 3);
}

TEST_CASE("crash summary without a stack or a known exception")
{
    crash::Report r;
    CHECK(crash::summarize(r) == "Lila crashed: unknown cause");
    r.section("crash").set("exception.code", "0xC0000409");
    r.section("crash").set("exception.description", crash::exceptionDescription(0xC0000409));
    r.section("crash").set("exception.name", crash::exceptionName(0xC0000409));
    CHECK(crash::summarize(r) == "Lila crashed: fail-fast (stack buffer overrun or __fastfail) (0xC0000409 STATUS_STACK_BUFFER_OVERRUN)");
}

TEST_CASE("crash line ring keeps the newest lines in order")
{
    crash::LineRing ring(3);
    CHECK(ring.lines().empty());
    ring.push("a");
    ring.push("b");
    CHECK(ring.lines() == std::vector<std::string>{"a", "b"});
    ring.push("c");
    ring.push("d");
    ring.push("e");
    CHECK(ring.lines() == std::vector<std::string>{"c", "d", "e"});
    CHECK(ring.size() == 3);
    CHECK(ring.pushed() == 5);
    std::vector<std::string> out;
    CHECK(ring.tryLines(out));
    CHECK(out == ring.lines());

    crash::LineRing big(500);
    std::vector<std::thread> writers;
    for (int t = 0; t < 4; ++t)
        writers.emplace_back([&big, t] {
            for (int i = 0; i < 1000; ++i)
                big.push(std::to_string(t) + ":" + std::to_string(i));
        });
    for (auto& w : writers)
        w.join();
    CHECK(big.size() == 500);
    CHECK(big.pushed() == 4000);
}

TEST_CASE("crash reports never carry secrets")
{
    std::vector<std::string> secrets{"s3cr3tTokenValue42"};
    CHECK(crash::redact("token s3cr3tTokenValue42 here", secrets) == "token <redacted> here");
    CHECK(crash::redact(R"({"token": "abcdef0123456789abcdef"})", {}) == R"({"token": "<redacted>"})");
    CHECK(crash::redact("Authorization: Bearer abcdefghijklmnopqrstuvwxyz", {}) == "Authorization: Bearer <redacted>");
    CHECK(crash::redact("/mcp/token=0123456789abcdef0123", {}) == "/mcp/token=<redacted>");
    // Ordinary prose mentioning tokens stays readable.
    CHECK(crash::redact("the token parser failed", {}) == "the token parser failed");
}

TEST_CASE("crash report retention keeps the newest folders")
{
    fs::path root = crashDir("retention");
    for (int i = 0; i < 25; ++i)
    {
        fs::path dir = crash::createReportDir(root, 1700000000 + i * 60, 1234);
        std::ofstream(dir / "report.txt") << "Lila crash report\n";
    }
    // A folder without a report is not ours to delete.
    fs::create_directories(root / "notes");
    CHECK(crash::listReports(root).size() == 25);
    CHECK(crash::pruneReports(root, 20) == 5);
    std::vector<fs::path> left = crash::listReports(root);
    REQUIRE(left.size() == 20);
    CHECK(left.front().filename().string() == crash::folderTimestamp(1700000000 + 5 * 60) + "_1234");
    CHECK(fs::is_directory(root / "notes"));

    fs::path a = crash::createReportDir(root, 1800000000, 7);
    fs::path b = crash::createReportDir(root, 1800000000, 7);
    CHECK(a != b);
    std::error_code ec;
    fs::remove_all(root.parent_path(), ec);
}

TEST_CASE("crash exit codes")
{
    CHECK(crash::isCrashExitCode(0xC0000374));
    CHECK(crash::isCrashExitCode(0xC0000409));
    CHECK(crash::isCrashExitCode(0xC00000FD));
    CHECK(crash::isCrashExitCode(3));
    CHECK_FALSE(crash::isCrashExitCode(0));
    CHECK_FALSE(crash::isCrashExitCode(1));
    CHECK_FALSE(crash::isCrashExitCode(0xFFFFFFFF));
    CHECK_FALSE(crash::isCrashExitCode(0xC000013A));
    CHECK(std::string(crash::exceptionName(0xC0000374)) == "STATUS_HEAP_CORRUPTION");
}
