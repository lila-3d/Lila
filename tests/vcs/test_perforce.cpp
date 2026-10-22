#include "vcs/PerforceProvider.h"

#include <doctest/doctest.h>

#include <functional>
#include <mutex>

using namespace lila::vcs;

namespace
{
/** Answers p4 calls from a callback and remembers what was asked. */
class FakeRunner final : public ProcessRunner
{
public:
    std::function<ProcessResult(const ProcessSpec&)> answer;
    std::vector<ProcessSpec> calls;

    ProcessResult run(const ProcessSpec& spec) override
    {
        std::scoped_lock lock(mMutex);
        calls.push_back(spec);
        return answer(spec);
    }

    /** The p4 subcommand: first argv entry after the global options. */
    static std::string command(const ProcessSpec& s)
    {
        for (size_t i = 1; i < s.argv.size(); ++i)
        {
            const std::string& a = s.argv[i];
            if (a == "-x" || a == "-p" || a == "-u" || a == "-c" || a == "-C" || a == "--field")
            {
                ++i;
                continue;
            }
            if (!a.empty() && a[0] == '-')
                continue;
            return a;
        }
        return {};
    }

private:
    std::mutex mMutex;
};

ProcessResult ok(std::string out)
{
    return {.started = true, .exitCode = 0, .out = std::move(out)};
}

bool has(const std::vector<std::string>& v, const std::string& s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}

// Shapes taken from a real 2022.1 server.
const char* kFstatSynced =
    R"({"clientFile":"X:\\Proj\\Content\\Hero.uasset","depotFile":"//Proj/main/Content/Hero.uasset","haveRev":"3","headAction":"edit","headChange":"1059","headRev":"3","headType":"binary","isMapped":""})";
const char* kFstatMine =
    R"({"action":"edit","actionOwner":"programmer","change":"1741","clientFile":"X:\\Proj\\Content\\Edit.uasset","depotFile":"//Proj/main/Content/Edit.uasset","haveRev":"1","headAction":"add","headRev":"2","headType":"binary","isMapped":"","type":"binary","workRev":"1"})";
const char* kFstatOther =
    R"({"clientFile":"X:\\Proj\\Content\\Other.uasset","depotFile":"//Proj/main/Content/Other.uasset","haveRev":"2","headAction":"edit","headRev":"2","headType":"binary","otherOpen":"1","otherOpen0":"artist@ExHorse_Art","otherAction0":"edit","otherChange0":"1700","isMapped":""})";
const char* kFstatLocked =
    R"({"clientFile":"X:\\Proj\\Content\\Map.umap","depotFile":"//Proj/main/Content/Map.umap","haveRev":"5","headAction":"edit","headRev":"5","headType":"binary+l","otherOpen":"1","otherOpen0":"artist@ExHorse_Art","otherLock":"","otherLock0":"artist@ExHorse_Art","isMapped":""})";
const char* kFstatAdded =
    R"({"action":"add","actionOwner":"programmer","change":"default","clientFile":"X:\\Proj\\Content\\New.uasset","depotFile":"//Proj/main/Content/New.uasset","isMapped":"","type":"binary","workRev":"1"})";
const char* kFstatHeadDeleted =
    R"({"clientFile":"X:\\Proj\\Content\\Gone.uasset","depotFile":"//Proj/main/Content/Gone.uasset","headAction":"delete","headChange":"1059","headRev":"2","headType":"binary","isMapped":""})";
const char* kNoSuchFile = R"({"data":"X:\\Proj\\Content\\Nope.uasset - no such file(s).\n","generic":17,"severity":2})";
const char* kNotLoggedIn = R"({"data":"Perforce password (P4PASSWD) invalid or unset.\n","generic":36,"severity":3})";
const char* kNoClient = R"({"data":"Client 'Horse' unknown - use 'client' command to create it.\n","generic":2,"severity":3})";
} // namespace

TEST_CASE("p4 -Mj output splits into records and messages")
{
    std::string out = std::string(kFstatSynced) + "\r\n" + kNoSuchFile + "\n\n" + kFstatAdded + "\n";
    PerforceProvider::Output o = PerforceProvider::parse(out);
    REQUIRE(o.records.size() == 2);
    REQUIRE(o.messages.size() == 1);
    CHECK(o.messages[0].severity == 2);
    CHECK(o.messages[0].generic == 17);
    CHECK(o.messages[0].text == "X:\\Proj\\Content\\Nope.uasset - no such file(s).");
    CHECK(o.records[1]["action"] == "add");
}

TEST_CASE("fstat records map to file states")
{
    auto st = [](const char* line) { return PerforceProvider::statusFromFstat(nlohmann::json::parse(line)); };

    FileStatus synced = st(kFstatSynced);
    CHECK(synced.state == FileState::Synced);
    CHECK_FALSE(synced.outOfDate);
    CHECK(synced.depotPath == "//Proj/main/Content/Hero.uasset");

    FileStatus mine = st(kFstatMine);
    CHECK(mine.state == FileState::CheckedOut);
    CHECK(mine.checkedOutByMe);
    CHECK(mine.outOfDate);
    CHECK(mine.changelist == "1741");

    FileStatus other = st(kFstatOther);
    CHECK(other.state == FileState::CheckedOutOther);
    REQUIRE(other.otherUsers.size() == 1);
    CHECK(other.otherUsers[0] == "artist@ExHorse_Art");

    FileStatus locked = st(kFstatLocked);
    CHECK(locked.state == FileState::Locked);
    CHECK(locked.exclusive);
    CHECK(locked.lockedByOther);

    CHECK(st(kFstatAdded).state == FileState::Added);
    CHECK(st(kFstatHeadDeleted).state == FileState::NotTracked);
    CHECK(st(R"({"action":"move/delete","depotFile":"//a"})").state == FileState::Deleted);

    FileStatus behind = st(R"({"clientFile":"c:\\a","depotFile":"//a","haveRev":"1","headRev":"4","headAction":"edit"})");
    CHECK(behind.state == FileState::OutOfDate);
}

TEST_CASE("status sends files through -x - and matches records back by local path")
{
    auto runner = std::make_shared<FakeRunner>();
    runner->answer = [](const ProcessSpec&) {
        return ok(std::string(kFstatMine) + "\n" + kFstatSynced + "\n" + kNoSuchFile + "\n");
    };
    PerforceProvider p4({.port = "ssl:p4:1666", .user = "programmer", .client = "Ws"}, runner);

    // Different case and separators from what p4 hands back.
    std::vector<fs::path> files{"x:/proj/content/hero.uasset", "X:/Proj/Content/Nope.uasset", "X:/Proj/Content/Edit.uasset"};
    std::vector<FileStatus> st = p4.status(files);
    REQUIRE(st.size() == 3);
    CHECK(st[0].state == FileState::Synced);
    CHECK(st[0].path == files[0].string());
    CHECK(st[1].state == FileState::NotTracked);
    CHECK(st[2].state == FileState::CheckedOut);

    REQUIRE(runner->calls.size() == 1);
    const ProcessSpec& call = runner->calls[0];
    CHECK(has(call.argv, "-ztag"));
    CHECK(has(call.argv, "-Mj"));
    CHECK(has(call.argv, "fstat"));
    CHECK(has(call.argv, "Ws"));
    CHECK(has(call.argv, "ssl:p4:1666"));
    CHECK(FakeRunner::command(call) == "fstat");
    CHECK(call.input.find("hero.uasset\n") != std::string::npos);
}

TEST_CASE("file arguments escape p4 wildcards in local paths only")
{
    CHECK(PerforceProvider::fileArg("C:/a/b@2#x%y*.txt") == fs::path("C:/a/b%402%23x%25y%2A.txt").string());
    CHECK(PerforceProvider::fileArg("//depot/main/...") == "//depot/main/...");
}

TEST_CASE("login and client errors become clear messages")
{
    auto runner = std::make_shared<FakeRunner>();
    PerforceProvider p4({.user = "jared"}, runner);

    runner->answer = [](const ProcessSpec&) { return ProcessResult{.started = true, .exitCode = 1, .out = std::string(kNotLoggedIn) + "\n"}; };
    try
    {
        p4.status({"C:/x.txt"});
        FAIL("expected VcsError");
    }
    catch (const VcsError& e)
    {
        std::string msg = e.what();
        CHECK(msg.find("not logged in as 'jared'") != std::string::npos);
        CHECK(msg.find("p4 login") != std::string::npos);
    }

    runner->answer = [](const ProcessSpec&) { return ProcessResult{.started = true, .exitCode = 1, .out = std::string(kNoClient) + "\n"}; };
    CHECK_THROWS_WITH_AS(p4.checkout({"C:/x.txt"}), doctest::Contains("no such client workspace 'Horse'"), VcsError);

    runner->answer = [](const ProcessSpec&) {
        return ProcessResult{.started = true, .exitCode = 1, .err = "Perforce client error:\n\tConnect to server failed; check $P4PORT.\n"};
    };
    CHECK_THROWS_WITH_AS(p4.status({"C:/x.txt"}), doctest::Contains("can't reach the server"), VcsError);

    runner->answer = [](const ProcessSpec&) { return ProcessResult{.started = false, .err = "file not found"}; };
    CHECK_THROWS_WITH_AS(p4.status({"C:/x.txt"}), doctest::Contains("not installed"), VcsError);
}

TEST_CASE("per-file failures among successes are warnings, not errors")
{
    auto runner = std::make_shared<FakeRunner>();
    runner->answer = [](const ProcessSpec&) {
        return ok(R"({"action":"edit","clientFile":"X:\\Proj\\a.txt","depotFile":"//Proj/a.txt","workRev":"2"})"
                  "\n"
                  R"({"data":"//Proj/b.uasset - can't edit exclusive file already opened\n","generic":17,"severity":3})"
                  "\n");
    };
    PerforceProvider p4({}, runner);
    OpResult r = p4.checkout({"X:/Proj/a.txt", "X:/Proj/b.uasset"}, "1741");
    REQUIRE(r.files.size() == 1);
    REQUIRE(r.warnings.size() == 1);
    CHECK(r.warnings[0].find("exclusive") != std::string::npos);
    CHECK(FakeRunner::command(runner->calls[0]) == "edit");
    CHECK(has(runner->calls[0].argv, "1741"));
}

TEST_CASE("changelists: create, list, submit")
{
    auto runner = std::make_shared<FakeRunner>();
    runner->answer = [](const ProcessSpec& s) {
        std::string cmd = FakeRunner::command(s);
        if (cmd == "change")
            return ok(R"({"data":"Change 1802 created.\n","generic":0,"severity":1})"
                      "\n");
        if (cmd == "info")
            return ok(R"({"clientName":"Ws","userName":"programmer","serverVersion":"P4D/NTX64/2022.1"})"
                      "\n");
        if (cmd == "changes")
            return ok(R"({"change":"1802","changeType":"public","client":"Ws","desc":"Fix the hero\n","status":"pending","time":"1","user":"programmer","shelved":""})"
                      "\n");
        if (cmd == "opened")
            return ok(R"({"action":"edit","change":"1802","client":"Ws","clientFile":"//Ws/a.txt","depotFile":"//Proj/a.txt","rev":"2","type":"text","user":"programmer"})"
                      "\n"
                      R"({"action":"add","change":"default","client":"Ws","clientFile":"//Ws/b.txt","depotFile":"//Proj/b.txt","rev":"1","type":"text","user":"programmer"})"
                      "\n");
        if (cmd == "submit")
            return ok(R"({"change":"1802","locked":"1"})"
                      "\n"
                      R"({"action":"edit","depotFile":"//Proj/a.txt","rev":"3"})"
                      "\n"
                      R"({"submittedChange":"1803"})"
                      "\n");
        return ok("");
    };
    PerforceProvider p4({}, runner);

    CHECK(p4.createChangelist("Fix the hero\nsecond line", "X:/Proj") == "1802");
    const ProcessSpec& create = runner->calls.back();
    CHECK(create.input.find("Change:\tnew") != std::string::npos);
    CHECK(create.input.find("\tFix the hero\n\tsecond line\n") != std::string::npos);

    std::vector<Changelist> cls = p4.changelists("X:/Proj");
    REQUIRE(cls.size() == 2);
    CHECK(cls[0].id == "default");
    REQUIRE(cls[0].files.size() == 1);
    CHECK(cls[0].files[0] == "//Proj/b.txt");
    CHECK(cls[1].id == "1802");
    CHECK(cls[1].description == "Fix the hero");
    CHECK(cls[1].shelved);
    CHECK(cls[1].files == std::vector<std::string>{"//Proj/a.txt"});

    CHECK_THROWS_AS(p4.submit("default", "  "), VcsError);
    OpResult sub = p4.submit("1802", "");
    CHECK(sub.change == "1803");
    CHECK(sub.files == std::vector<std::string>{"//Proj/a.txt"});

    CHECK_THROWS_AS(p4.shelve("default", {}), VcsError);
}

TEST_CASE("filelog revisions come from indexed keys")
{
    // Real output of `filelog -l -m 2`.
    auto r = nlohmann::json::parse(
        R"({"action0":"delete","action1":"add","change0":"1059","change1":"1058","client0":"Vaei_Hollow","client1":"Vaei_Hollow","depotFile":"//Hollow/mainline/Content/Knight_Idle_L.uasset","desc0":"Editor tooling. Update knight. Stroll anim.\n","desc1":"Implement stroll etc\n","digest1":"417B07C3773E93240182329FF66298ED","fileSize1":"1038052","rev0":"2","rev1":"1","time0":"1758365185","time1":"1758358009","type0":"binary","type1":"binary","user0":"programmer","user1":"programmer"})");
    std::vector<Revision> revs = PerforceProvider::parseFilelog(r);
    REQUIRE(revs.size() == 2);
    CHECK(revs[0].revision == "2");
    CHECK(revs[0].action == "delete");
    CHECK(revs[0].change == "1059");
    CHECK(revs[0].time == 1758365185);
    CHECK(revs[0].description == "Editor tooling. Update knight. Stroll anim.");
    CHECK(revs[1].user == "programmer");
}

TEST_CASE("stdin is never left open for a prompt")
{
    auto runner = std::make_shared<FakeRunner>();
    runner->answer = [](const ProcessSpec&) { return ok(""); };
    PerforceProvider p4({}, runner);
    p4.revert({"C:/a.txt"});
    // The runner always closes stdin after writing input; here the input is only the file list.
    CHECK(runner->calls[0].input == fs::path("C:/a.txt").string() + "\n");
}

TEST_CASE("real server, read only" * doctest::skip(false))
{
    // Only `info` and `where`: nothing on the server is changed.
    PerforceProvider p4({}, systemRunner());
    nlohmann::json info = p4.info();
    if (!info.contains("server"))
    {
        MESSAGE("no reachable Perforce server: " << info.value("error", std::string("?")));
        return;
    }
    CHECK(info["server"].contains("serverVersion"));
    std::error_code ec;
    MESSAGE("temp dir mapped by p4: " << p4.manages(fs::temp_directory_path(ec)));
}
