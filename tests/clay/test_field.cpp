#include "clay/Clay.h"

#include <doctest/doctest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <random>

using namespace lila::clay;

namespace
{
Item prim(Shape shape, glm::dvec3 at, Op op = Op::Add, double blend = 0.0)
{
    Item it{.op = op, .blend = blend, .matrix = glm::translate(glm::dmat4(1.0), at), .shape = Primitive{.shape = shape}};
    return it;
}

Item sphere(glm::dvec3 at, double r, Op op = Op::Add, double blend = 0.0)
{
    Item it = prim(Shape::Sphere, at, op, blend);
    it.shape->radius = r;
    return it;
}

Item root(std::vector<Item> children) { return Item{.children = std::move(children)}; }
} // namespace

TEST_CASE("clay field: each shape is zero on its surface and negative inside")
{
    Primitive p;
    p.shape = Shape::Sphere;
    CHECK(distance(p, {10, 0, 0}) == doctest::Approx(0.0));
    CHECK(distance(p, {0, 0, 0}) == doctest::Approx(-10.0));

    p.shape = Shape::Box;
    p.extents = {5, 6, 7};
    CHECK(distance(p, {5, 0, 0}) == doctest::Approx(0.0));
    CHECK(distance(p, {0, 0, 7}) == doctest::Approx(0.0));
    CHECK(distance(p, {0, 0, 0}) == doctest::Approx(-5.0));

    p.shape = Shape::Capsule;
    p.radius = 2;
    p.endRadius = 2;
    p.length = 10;
    CHECK(distance(p, {0, 7, 0}) == doctest::Approx(0.0));
    CHECK(distance(p, {2, 3, 0}) == doctest::Approx(0.0));
    p.endRadius = 1;
    CHECK(distance(p, {0, 6, 0}) == doctest::Approx(0.0));
    CHECK(distance(p, {0, -7, 0}) == doctest::Approx(0.0));

    p.shape = Shape::Cylinder;
    p.radius = 3;
    p.length = 8;
    CHECK(distance(p, {3, 0, 0}) == doctest::Approx(0.0));
    CHECK(distance(p, {0, 4, 0}) == doctest::Approx(0.0));

    p.shape = Shape::Cone;
    p.radius = 4;
    p.endRadius = 0;
    CHECK(distance(p, {0, -4, 0}) == doctest::Approx(0.0));
    CHECK(distance(p, {2, 0, 0}) == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(distance(p, {0, 0, 0}) < 0.0);

    p.shape = Shape::Torus;
    p.radius = 10;
    p.endRadius = 2;
    CHECK(distance(p, {12, 0, 0}) == doctest::Approx(0.0));
    CHECK(distance(p, {10, 0, 0}) == doctest::Approx(-2.0));
    CHECK(distance(p, {0, 0, 0}) > 0.0);

    p.shape = Shape::Ellipsoid;
    p.extents = {10, 5, 2};
    CHECK(std::abs(distance(p, {10, 0, 0})) < 1e-9);
    CHECK(std::abs(distance(p, {0, 0, 2})) < 1e-9);
    CHECK(distance(p, {0, 0, 0}) < 0.0);
}

TEST_CASE("clay field: scale carries into distance, groups scope their cuts, symmetry mirrors")
{
    Item big = sphere({0, 0, 0}, 5);
    big.matrix = glm::scale(glm::dmat4(1.0), glm::dvec3(2.0));
    CHECK(Field(root({big})).eval({10, 0, 0}).distance == doctest::Approx(0.0));
    CHECK(Field(root({big})).eval({0, 0, 0}).distance == doctest::Approx(-10.0));

    // A cut inside a group reaches only the group.
    Item group{.children = {sphere({20, 0, 0}, 5), sphere({0, 0, 0}, 50, Op::Subtract)}};
    Field f(root({sphere({-20, 0, 0}, 5), group}));
    CHECK(f.eval({-20, 0, 0}).distance < 0.0);
    CHECK(f.eval({20, 0, 0}).distance > 0.0);

    // At the top level the same cut takes everything before it.
    Field flat(root({sphere({-20, 0, 0}, 5), sphere({20, 0, 0}, 5), sphere({0, 0, 0}, 50, Op::Subtract)}));
    CHECK(flat.eval({-20, 0, 0}).distance > 0.0);

    Field inter(root({sphere({0, 0, 0}, 10), sphere({8, 0, 0}, 5, Op::Intersect)}));
    CHECK(inter.eval({6, 0, 0}).distance < 0.0);
    CHECK(inter.eval({-6, 0, 0}).distance > 0.0);

    Field mirror(root({sphere({10, 0, 0}, 3)}), true);
    CHECK(mirror.eval({-10, 0, 0}).distance == doctest::Approx(-3.0));
    CHECK(mirror.eval({10, 0, 0}).distance == doctest::Approx(-3.0));
    // A shape on the plane is not doubled up.
    Field centred(root({sphere({0, 0, 0}, 3)}), true);
    CHECK(centred.eval({0, 3, 0}).distance == doctest::Approx(0.0));
}

TEST_CASE("clay field: smooth add fills the crease, colours blend across it")
{
    Item a = sphere({-5, 0, 0}, 5, Op::Add, 0.0);
    Item b = sphere({5, 0, 0}, 5, Op::Add, 4.0);
    a.shape->color = {1, 0, 0};
    b.shape->color = {0, 0, 1};
    Field hard(root({a, sphere({5, 0, 0}, 5)}));
    Field soft(root({a, b}));
    // Where both distances are equal the join sinks by a quarter of the blend radius.
    CHECK(hard.eval({0, 5, 0}).distance == doctest::Approx(std::sqrt(50.0) - 5.0));
    CHECK(soft.eval({0, 5, 0}).distance == doctest::Approx(std::sqrt(50.0) - 5.0 - 1.0));
    const glm::vec3 mid = soft.eval({0, 5, 0}).color;
    CHECK(mid.x == doctest::Approx(0.5));
    CHECK(mid.z == doctest::Approx(0.5));
    CHECK(soft.eval({-10, 0, 0}).color.x == doctest::Approx(1.0));
}

TEST_CASE("clay field: a local program matches the whole field near its region")
{
    std::vector<Item> items;
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> u(-40.0, 40.0);
    for (int i = 0; i < 40; ++i)
        items.push_back(sphere({u(rng), u(rng), u(rng)}, 4.0 + std::abs(u(rng)) * 0.1, i % 5 == 4 ? Op::Subtract : Op::Add, 2.0));
    Item g{.blend = 1.0, .children = {sphere({0, 0, 0}, 10), sphere({0, 0, 10}, 6, Op::Intersect, 1.0)}};
    items.push_back(g);
    const Field f(root(items), true);

    for (int trial = 0; trial < 200; ++trial)
    {
        const glm::dvec3 c(u(rng), u(rng), u(rng));
        const Bounds region{c - 2.0, c + 2.0};
        const Field::Local local = f.local(region, 1.0);
        for (int k = 0; k < 8; ++k)
        {
            const glm::dvec3 p = c + glm::dvec3((k & 1) ? 1.5 : -1.5, (k & 2) ? 1.5 : -1.5, (k & 4) ? 1.5 : -1.5);
            const double whole = f.eval(p).distance;
            if (std::abs(whole) < 0.5)
                CHECK(local.eval(p).distance == doctest::Approx(whole).epsilon(1e-12));
        }
    }
}

TEST_CASE("clay field: changes reports where an edit can reach")
{
    const Field a(root({sphere({0, 0, 0}, 5), sphere({50, 0, 0}, 5)}));
    const Field moved(root({sphere({0, 0, 0}, 5), sphere({60, 0, 0}, 5)}));
    auto r = Field::changes(a, moved);
    REQUIRE(r);
    REQUIRE(r->size() == 2);
    CHECK((*r)[0].min.x == doctest::Approx(45.0));
    CHECK((*r)[1].max.x == doctest::Approx(65.0));

    CHECK(Field::changes(a, a)->empty());
    CHECK_FALSE(Field::changes(a, Field(root({sphere({0, 0, 0}, 5)}))));
    CHECK_FALSE(Field::changes(a, Field(root({sphere({0, 0, 0}, 5), sphere({50, 0, 0}, 5)}), true)));
}
