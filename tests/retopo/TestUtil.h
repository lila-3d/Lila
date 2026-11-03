#pragma once

#include <doctest/doctest.h>

#include "geo/Mesh.h"
#include "geo/Primitives.h"
#include "retopo/Retopo.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <string>

namespace retopotest
{
using namespace lila;

inline const geo::Mesh& outMesh(const std::string& name)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    return *g.find(name)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
}

inline scene::Node* makeMeshNode(const std::string& name, geo::Mesh mesh)
{
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = g.createNode(*scene::types::find("mesh"), name, scene::kNoNode, false);
    g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(mesh))));
    return n;
}

inline geo::Mesh sphere(int subdiv)
{
    geo::SphereOptions o;
    o.radius = 50.0;
    o.subdivAxis = subdiv;
    o.subdivHeight = subdiv;
    return geo::makeSphere(o);
}

/** Logs the remesh metrics and per-stage timings. */
inline void report(const std::string& what, const retopo::QuadRemeshResult& r)
{
    double total = 0.0;
    std::string stages;
    for (auto& [s, t] : r.timings)
    {
        total += t;
        stages += " " + s + "=" + std::to_string(t);
    }
    MESSAGE(what << ": " << r.metrics.summary() << " in " << total << " s:" << stages);
}
} // namespace retopotest
