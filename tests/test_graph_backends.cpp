#include <doctest/doctest.h>

#include "core/ExtensionContext.h"
#include "ui/GraphBackend.h"

using namespace lila;
using namespace lila::ui;

namespace
{
class TestBackend final : public GraphBackend
{
public:
    explicit TestBackend(std::string id) : mId(std::move(id)) {}
    std::string id() const override { return mId; }
    QString title() const override { return "Test Graph"; }
    std::string commandPrefix() const override { return "tst"; }
    bool supports(std::string_view verb) const override { return verb != "ExportGroup"; }
    std::vector<std::string> groupNames(scene::Graph&) const override { return {"A", "B"}; }
    bool isGroupNode(const scene::Node&) const override { return false; }

private:
    std::string mId;
};
} // namespace

TEST_CASE("a graph backend names its commands by prefix and verb")
{
    TestBackend b("tstGraph");
    CHECK(b.command("AddNode") == "tstAddNode");
    CHECK(b.command("GroupInfo") == "tstGroupInfo");
    CHECK(b.isQuery("GroupInfo"));
    CHECK(b.isQuery("NodeTypes"));
    CHECK_FALSE(b.isQuery("AddNode"));
    CHECK_FALSE(b.supports("ExportGroup"));
    CHECK(b.viewerType().empty());
    CHECK_FALSE(b.canSend());
}

TEST_CASE("graph backends register by id and unload with their extension")
{
    size_t before = graphBackends::registry().size();
    {
        ext::Context ctx("tstExtension");
        CHECK(graphBackends::add(ctx, std::make_shared<TestBackend>("tstGraph")) != 0);
        GraphBackendPtr found = graphBackends::find("tstGraph");
        REQUIRE(found);
        CHECK(found->command("Connect") == "tstConnect");
        CHECK(*graphBackends::registry().ownerOf("tstGraph") == "tstExtension");
        CHECK(graphBackends::ids().back() == "tstGraph");

        // Another owner can't take the id.
        ext::Context other("otherExtension");
        CHECK(graphBackends::add(other, std::make_shared<TestBackend>("tstGraph")) == 0);
        CHECK(*graphBackends::registry().ownerOf("tstGraph") == "tstExtension");

        ctx.unloadAll();
        CHECK_FALSE(graphBackends::find("tstGraph"));
    }
    CHECK(graphBackends::registry().size() == before);

    graphBackends::add(std::make_shared<TestBackend>("tstLila"));
    CHECK(*graphBackends::registry().ownerOf("tstLila") == kLilaOwner);
    CHECK(graphBackends::remove("tstLila"));
    CHECK_FALSE(graphBackends::remove("tstLila"));
}

TEST_CASE("the geometry-nodes backend keeps the gn command names")
{
    registerGnGraphBackend();
    GraphBackendPtr gn = graphBackends::find(kGeometryNodesBackend);
    REQUIRE(gn);
    CHECK(gn->command("AddNode") == "gnAddNode");
    CHECK(gn->command("SetActiveViewer") == "gnSetActiveViewer");
    CHECK(gn->viewerType() == "Viewer");
    CHECK(gn->subgroupNodeType() == "Group");
    CHECK(gn->newGroupAction() == "gn.newGroup");
    CHECK(gn->canSend());
    CHECK(gn->valueless("geometry"));
    CHECK_FALSE(gn->valueless("float"));
    CHECK(gn->fileSuffixes().front() == "lilanodes");
}
