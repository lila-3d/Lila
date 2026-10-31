#include <doctest/doctest.h>

#include "gn/Exec.h"
#include "gn/NodesCommon.h"
#include "scene/ViewState.h"

#include <glm/gtc/matrix_transform.hpp>

using namespace lila;

namespace viewtests
{
using N = gn::GNode;
using L = gn::GLink;
using gn::SocketType;

std::vector<gn::Value> evalOutputs(std::vector<N> nodes, std::vector<L> links, std::vector<gn::GOutput> outputs)
{
    gn::Group g = gn::Group::makeDefault("T");
    g.links = std::move(links);
    g.outputs = std::move(outputs);
    g.nodes.insert(g.nodes.end(), nodes.begin(), nodes.end());
    gn::PlanPtr plan = gn::compile(g);
    REQUIRE_MESSAGE(plan->error.empty(), plan->error);
    CHECK(plan->usesView);
    std::vector<gn::Value> out = gn::executeOutputs(*plan, {});
    out.erase(out.begin());
    return out;
}

bool isViewportNow()
{
    auto out = evalOutputs({N{"V", "IsViewport", {}, {}}}, {L{"V", "isViewport", "GroupOutput", "v"}}, {{"v", SocketType::Bool, {}}});
    return gn::evaluateSingle<uint8_t>(out[0].field) != 0;
}

TEST_CASE("Is Viewport reads false while the scene evaluates for output")
{
    CHECK(isViewportNow());
    {
        scene::viewState::FinalScope outer;
        scene::viewState::FinalScope inner;
        CHECK_FALSE(isViewportNow());
    }
    CHECK(isViewportNow());
}

TEST_CASE("Active Camera reports the camera the viewport set")
{
    scene::viewState::ActiveCamera cam;
    cam.world = glm::translate(glm::dmat4(1.0), glm::dvec3(1, 2, 3));
    cam.horizontalFov = 1.2;
    cam.verticalFov = 0.8;
    scene::viewState::setActiveCamera(cam);
    auto out = evalOutputs({N{"C", "ActiveCamera", {}, {}}},
                           {L{"C", "location", "GroupOutput", "loc"}, L{"C", "forward", "GroupOutput", "fwd"}, L{"C", "horizontalFov", "GroupOutput", "fov"}},
                           {{"loc", SocketType::Vector, {}}, {"fwd", SocketType::Vector, {}}, {"fov", SocketType::Float, {}}});
    const glm::dvec3 loc = gn::evaluateSingle<glm::dvec3>(out[0].field);
    const glm::dvec3 fwd = gn::evaluateSingle<glm::dvec3>(out[1].field);
    CHECK(loc.x == doctest::Approx(1));
    CHECK(loc.z == doctest::Approx(3));
    CHECK(fwd.z == doctest::Approx(-1));
    CHECK(gn::evaluateSingle<double>(out[2].field) == doctest::Approx(1.2));
    scene::viewState::setActiveCamera({});
}

TEST_CASE("A group reading the view through a nested group is dynamic")
{
    gn::Group inner = gn::Group::makeDefault("Inner");
    inner.nodes.push_back(N{"V", "IsViewport", {}, {}});
    gn::Group outer = gn::Group::makeDefault("Outer");
    outer.subgroups["Inner"] = inner.toJson();
    CHECK(gn::usesViewState(outer));
    CHECK_FALSE(gn::usesViewState(gn::Group::makeDefault("Plain")));
}
} // namespace viewtests
