#include "core/MainQueue.h"
#include "vcs/GitProvider.h"
#include "vcs/SourceControl.h"

#include <doctest/doctest.h>

#include <chrono>
#include <fstream>
#include <random>

using namespace lila::vcs;

namespace
{
/** A throwaway repo under %TEMP%, removed afterwards. Never the Lila tree. */
struct TempRepo
{
    fs::path root;
    std::shared_ptr<GitProvider> git;

    TempRepo()
    {
        std::random_device rd;
        root = fs::temp_directory_path() / ("lila_vcs_test_" + std::to_string(rd()));
        fs::create_directories(root);
        root = fs::canonical(root);
        git = std::make_shared<GitProvider>(root, GitSettings{}, systemRunner());
        git->run({"init", "-q"});
        git->run({"config", "user.name", "Lila Test"});
        git->run({"config", "user.email", "test@example.invalid"});
        git->run({"config", "commit.gpgsign", "false"});
        git->run({"config", "core.autocrlf", "false"});
    }

    ~TempRepo()
    {
        std::error_code ec;
        // git marks its object files read-only.
        for (auto& e : fs::recursive_directory_iterator(root, ec))
            fs::permissions(e.path(), fs::perms::owner_write, fs::perm_options::add, ec);
        fs::remove_all(root, ec);
    }

    fs::path write(const std::string& name, const std::string& text)
    {
        fs::path p = root / name;
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << text;
        return p;
    }

    static std::string read(const fs::path& p)
    {
        std::ifstream f(p, std::ios::binary);
        return {std::istreambuf_iterator<char>(f), {}};
    }

    FileState state(const fs::path& p) { return git->status({p}).at(0).state; }
};
} // namespace

TEST_CASE("git: detect the work tree")
{
    TempRepo repo;
    fs::create_directories(repo.root / "sub");
    auto found = GitProvider::findRoot(repo.root / "sub" / "not_yet.txt", {}, *systemRunner());
    REQUIRE(found);
    CHECK(fs::equivalent(*found, repo.root));
    CHECK(repo.git->manages(repo.root / "sub"));
    CHECK_FALSE(repo.git->manages(repo.root.parent_path()));
}

TEST_CASE("git: add, submit, modify, revert")
{
    TempRepo repo;
    fs::path a = repo.write("a.txt", "one\n");
    fs::path spaced = repo.write("dir with space/b @2.txt", "b\n");

    CHECK(repo.state(a) == FileState::NotTracked);

    OpResult added = repo.git->add({a, spaced});
    CHECK(added.files.size() == 2);
    CHECK(repo.state(a) == FileState::Added);
    CHECK(repo.state(spaced) == FileState::Added);

    std::vector<Changelist> cls = repo.git->changelists(repo.root);
    REQUIRE(cls.size() == 1);
    CHECK(cls[0].files.size() == 2);

    CHECK_THROWS_AS(repo.git->submit("", ""), VcsError);
    OpResult sub = repo.git->submit("", "first commit");
    CHECK(sub.change.size() == 40);
    CHECK(sub.files.size() == 2);
    CHECK(repo.state(a) == FileState::Synced);

    repo.write("a.txt", "two\n");
    FileStatus st = repo.git->status({a}).at(0);
    CHECK(st.state == FileState::CheckedOut);
    CHECK(st.checkedOutByMe);

    repo.git->revert({a});
    CHECK(repo.state(a) == FileState::Synced);
    CHECK(TempRepo::read(a) == "one\n");

    fs::path n = repo.write("new.txt", "n\n");
    repo.git->add({n});
    repo.git->revert({n});
    CHECK(repo.state(n) == FileState::NotTracked);
    CHECK(fs::exists(n));

    repo.write("a.txt", "three\n");
    OpResult partial = repo.git->submit("", "second", {a});
    CHECK(partial.files.size() == 1);

    std::vector<Revision> hist = repo.git->history(a);
    REQUIRE(hist.size() == 2);
    CHECK(hist[0].description == "second");
    CHECK(hist[1].description == "first commit");
    CHECK(hist[0].user == "Lila Test");
    CHECK(hist[0].time > 0);

    fs::path fetched = repo.root.parent_path() / (repo.root.filename().string() + "_fetched.txt");
    repo.git->fetchRevision(a, hist[0].revision, fetched);
    CHECK(TempRepo::read(fetched) == "three\n");
    fs::remove(fetched);

    repo.git->remove({spaced});
    CHECK(repo.state(spaced) == FileState::Deleted);
    CHECK_FALSE(fs::exists(spaced));
}

TEST_CASE("git: checkout of a plain file is a no-op success")
{
    TempRepo repo;
    fs::path a = repo.write("a.txt", "x");
    repo.git->add({a});
    repo.git->submit("", "c");
    OpResult r = repo.git->checkout({a});
    CHECK(r.files.size() == 1);
    CHECK(r.warnings.empty());
    CHECK_THROWS_AS(repo.git->shelve("1", {}), VcsError);
    CHECK_THROWS_AS(repo.git->createChangelist("x", repo.root), VcsError);
}

TEST_CASE("git: files outside the repo are rejected")
{
    TempRepo repo;
    CHECK_THROWS_AS(repo.git->add({repo.root.parent_path() / "elsewhere.txt"}), VcsError);
}

TEST_CASE("SourceControl: auto-detects git, async delivers on the main thread, save hook checks out")
{
    lila::mainq::init();
    TempRepo repo;
    fs::path a = repo.write("a.txt", "x");
    repo.git->add({a});
    repo.git->submit("", "c");

    SourceControl& sc = SourceControl::get();
    sc.refresh();
    std::shared_ptr<Provider> p = sc.providerFor(a);
    REQUIRE(p);
    CHECK(p->name() == "git");

    bool delivered = false;
    std::vector<FileStatus> got;
    sc.statusAsync({a, repo.root / "untracked.txt"}, [&](std::vector<FileStatus> st, std::string err) {
        CHECK(lila::mainq::isMainThread());
        CHECK(err.empty());
        got = std::move(st);
        delivered = true;
    });
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!delivered && std::chrono::steady_clock::now() < deadline)
    {
        lila::mainq::drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(delivered);
    REQUIRE(got.size() == 2);
    CHECK(got[0].state == FileState::Synced);
    CHECK(got[1].state == FileState::NotTracked);

    // A read-only tracked file: approved checkout makes it writable.
    fs::permissions(a, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write, fs::perm_options::remove);
    std::string err;
    WriteAccess w = sc.requestWrite(a, &err, true);
    CHECK_MESSAGE(w == WriteAccess::Writable, err);
    CHECK((fs::status(a).permissions() & fs::perms::owner_write) != fs::perms::none);

    CHECK(sc.requestWrite(repo.root / "missing.txt", &err) == WriteAccess::Writable);
    sc.shutdown();
}
