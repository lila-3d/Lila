#include <doctest/doctest.h>

#include "ops/Command.h"
#include "scene/Graph.h"

using namespace lila;

namespace componentValueTests
{
json cvRun(const char* name, json call = json::object()) { return ops::execute(name, call); }

/** A 100 cm tube skinned to a chain of three joints, vertices 0 and 1 at 0.7/0.3 on bone_a/bone_b, vertex 2 all bone_c. */
std::string cvSkinnedTube()
{
    cvRun("newScene");
    std::string mesh = cvRun("polyCylinder", {{"name", "Tube"}, {"radius", 10}, {"height", 100}, {"subdivisionsHeight", 4}})[0];
    cvRun("select", {{"args", json::array()}});
    cvRun("joint", {{"position", json::array({0, -50, 0})}, {"name", "bone_a"}});
    cvRun("joint", {{"position", json::array({0, 0, 0})}, {"name", "bone_b"}});
    cvRun("joint", {{"position", json::array({0, 50, 0})}, {"name", "bone_c"}});
    cvRun("skinCluster", {{"args", {"bone_a", mesh}}});
    cvRun("skinPercent", {{"args", {mesh}}, {"vertices", {0, 1}}, {"transformValue", {{"bone_a", 0.7}, {"bone_b", 0.3}}}});
    cvRun("skinPercent", {{"args", {mesh}}, {"vertices", {2}}, {"transformValue", {{"bone_c", 1.0}}}});
    return mesh;
}

double cvValue(const std::string& mesh, int vertex, const std::string& column)
{
    json r = cvRun("componentValues", {{"args", {mesh}}, {"vertices", {vertex}}, {"columns", {column}}});
    return r["values"][0][0].get<double>();
}

json cvColumn(const json& result, const std::string& label)
{
    for (const json& c : result["columns"])
        if (c["label"] == label)
            return c;
    return nullptr;
}
} // namespace componentValueTests
using namespace componentValueTests;

TEST_CASE("componentValues lists the influences touching the selection, heaviest first, with positions")
{
    std::string mesh = cvSkinnedTube();
    cvRun("selectComponents", {{"args", {mesh}}, {"vertices", {0, 1}}});
    json r = cvRun("componentValues");
    REQUIRE(r["rows"].size() == 2);
    CHECK(r["rows"][0][1] == 0);
    REQUIRE(r["columns"].size() == 5);
    CHECK(r["columns"][0]["label"] == "bone_a");
    CHECK(r["columns"][0]["total"].get<double>() == doctest::Approx(1.4));
    CHECK(r["columns"][1]["label"] == "bone_b");
    CHECK(r["columns"][2]["id"] == "position.x");
    CHECK(r["values"][1][0].get<double>() == doctest::Approx(0.7));
    CHECK(r["values"][1][1].get<double>() == doctest::Approx(0.3));
    CHECK(cvColumn(r, "bone_c").is_null());

    json all = cvRun("componentValues", {{"columns", {"allWeights"}}});
    REQUIRE(all["columns"].size() == 3);
    CHECK(all["columns"][2]["label"] == "bone_c");
    CHECK(all["columns"][2]["total"].get<double>() == doctest::Approx(0.0));
    CHECK_FALSE(all["columns"][2]["held"].get<bool>());
    CHECK(all["columns"][2]["links"].size() == 1);

    CHECK_THROWS_AS(cvRun("componentValues", {{"columns", {"nope"}}}), CommandError);
}

TEST_CASE("setComponentValues renormalises unheld influences, refuses held ones, and undoes")
{
    std::string mesh = cvSkinnedTube();
    cvRun("setComponentValues", {{"args", {mesh}}, {"vertices", {0}}, {"column", "bone_a"}, {"values", 0.4}});
    CHECK(cvValue(mesh, 0, "bone_a") == doctest::Approx(0.4));
    CHECK(cvValue(mesh, 0, "bone_b") == doctest::Approx(0.6));
    cvRun("undo");
    CHECK(cvValue(mesh, 0, "bone_a") == doctest::Approx(0.7));
    CHECK(cvValue(mesh, 0, "bone_b") == doctest::Approx(0.3));

    // Hold bone_b: it can't be set, and setting bone_a takes the room from bone_c only.
    json col = cvColumn(cvRun("componentValues", {{"args", {mesh}}, {"columns", {"allWeights"}}}), "bone_b");
    REQUIRE(col.is_object());
    cvRun("setAttr", {{"args", {col["links"][0].get<std::string>() + ".lockWeights", true}}});
    CHECK(cvColumn(cvRun("componentValues", {{"args", {mesh}}, {"columns", {"allWeights"}}}), "bone_b")["held"].get<bool>());
    CHECK_THROWS_AS(cvRun("setComponentValues", {{"args", {mesh}}, {"vertices", {2}}, {"column", "bone_b"}, {"values", 0.5}}), CommandError);
    cvRun("setComponentValues", {{"args", {mesh}}, {"vertices", {2}}, {"column", "bone_a"}, {"values", 0.25}});
    CHECK(cvValue(mesh, 2, "bone_a") == doctest::Approx(0.25));
    CHECK(cvValue(mesh, 2, "bone_b") == doctest::Approx(0.0));
    CHECK(cvValue(mesh, 2, "bone_c") == doctest::Approx(0.75));

    // normalize=False leaves the rest alone; setting the same value again with it on normalizes the point.
    cvRun("setComponentValues", {{"args", {mesh}}, {"vertices", {2}}, {"column", "bone_a"}, {"values", 0.5}, {"normalize", false}});
    CHECK(cvValue(mesh, 2, "bone_c") == doctest::Approx(0.75));
    cvRun("setComponentValues", {{"args", {mesh}}, {"vertices", {2}}, {"column", "bone_a"}, {"values", 0.5}});
    CHECK(cvValue(mesh, 2, "bone_c") == doctest::Approx(0.5));

    // Several columns per vertex in one call.
    cvRun("setAttr", {{"args", {col["links"][0].get<std::string>() + ".lockWeights", false}}});
    cvRun("setComponentValues", {{"args", {mesh}}, {"vertices", {0, 1}}, {"columns", {"bone_a", "bone_b"}}, {"values", {{0.5, 0.5}, {0.1, 0.9}}}});
    CHECK(cvValue(mesh, 0, "bone_b") == doctest::Approx(0.5));
    CHECK(cvValue(mesh, 1, "bone_a") == doctest::Approx(0.1));
    CHECK(cvValue(mesh, 1, "bone_b") == doctest::Approx(0.9));
    CHECK_THROWS_AS(cvRun("setComponentValues", {{"args", {mesh}}, {"vertices", {0, 1}}, {"column", "bone_a"}, {"values", {0.5}}}), CommandError);
}

TEST_CASE("setComponentValues moves positions on the base mesh, relative or absolute, undoably")
{
    std::string mesh = cvSkinnedTube();
    double y = cvValue(mesh, 3, "position.y");
    cvRun("setComponentValues", {{"args", {mesh}}, {"vertices", {3}}, {"column", "position.y"}, {"values", 10.0}, {"relative", true}});
    CHECK(cvValue(mesh, 3, "position.y") == doctest::Approx(y + 10.0));
    cvRun("setComponentValues", {{"args", {mesh}}, {"vertices", {3}}, {"column", "py"}, {"values", 2.0}});
    CHECK(cvValue(mesh, 3, "position.y") == doctest::Approx(2.0));
    cvRun("undo");
    cvRun("undo");
    CHECK(cvValue(mesh, 3, "position.y") == doctest::Approx(y));
}
