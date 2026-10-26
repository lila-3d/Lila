#include "ops/Command.h"
#include "geo/ColorSets.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "vcolor/VertexColors.h"

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

namespace nb = nanobind;

namespace lila::python
{
namespace
{
scene::Node* nodeOf(uint64_t id) { return scene::Graph::current().node(id); }
} // namespace

/** Id-level scene queries for the object API in lila.nodes; anything that edits goes through commands. */
void bindScene(nb::module_& m)
{
    // Vertex colors as numpy arrays, one (r, g, b, a) row per face-vertex in vcolor::cornerOrder.
    m.def(
        "color_set_array",
        [](const std::string& mesh, const std::string& set, bool base) {
            scene::Graph& g = scene::Graph::current();
            scene::Node* n = g.find(mesh);
            if (!n || !n->type().isA("mesh"))
                throw nb::value_error((mesh + " is not a mesh").c_str());
            g.evaluate();
            scene::MeshPtr m = base ? scene::modifiers::cage(g, n->id()) : n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
            std::string name = set.empty() && m ? geo::colors::current(*m) : set;
            if (!m || !geo::colors::has(*m, name))
                throw nb::value_error((mesh + " has no color set " + (name.empty() ? std::string("(none)") : name)).c_str());
            std::span<const glm::vec4> v = geo::colors::values(*m, name);
            float* data = nullptr;
            size_t rows = 0;
            {
                nb::gil_scoped_release nogil;
                std::vector<geo::Index> order = vcolor::cornerOrder(*m);
                rows = order.size();
                data = new float[rows * 4];
                tbb::parallel_for(tbb::blocked_range<size_t>(0, rows, 1 << 14), [&](const tbb::blocked_range<size_t>& r) {
                    for (size_t i = r.begin(); i < r.end(); ++i)
                        std::memcpy(data + i * 4, &v[order[i]], sizeof(glm::vec4));
                });
            }
            nb::capsule owner(data, [](void* p) noexcept { delete[] static_cast<float*>(p); });
            return nb::ndarray<nb::numpy, float, nb::ndim<2>>(data, {rows, size_t(4)}, owner);
        },
        nb::arg("mesh"), nb::arg("color_set") = "", nb::arg("base") = true);
    m.def(
        "set_color_set_array",
        [](const std::string& mesh, nb::ndarray<const float, nb::ndim<2>, nb::c_contig, nb::device::cpu> values, const std::string& set) {
            if (values.shape(1) != 4)
                throw nb::value_error("expected an (N, 4) float32 array");
            std::vector<glm::vec4> flat;
            {
                nb::gil_scoped_release nogil;
                flat.resize(values.shape(0));
                std::memcpy(flat.data(), values.data(), flat.size() * sizeof(glm::vec4));
            }
            uint64_t token = vcolor::stashValues(std::move(flat));
            ops::execute("setVertexColorData", {{"args", {mesh}}, {"colorSet", set}, {"buffer", int64_t(token)}});
        },
        nb::arg("mesh"), nb::arg("values"), nb::arg("color_set") = "");

    m.def("node_id", [](const std::string& name) -> nb::object {
        scene::Node* n = scene::Graph::current().find(name);
        return n ? nb::object(nb::int_(n->id())) : nb::none();
    });
    m.def("node_name", [](uint64_t id) -> nb::object {
        scene::Node* n = nodeOf(id);
        return n ? nb::object(nb::str(n->name().c_str())) : nb::none();
    });
    m.def("node_path", [](uint64_t id) -> nb::object {
        scene::Node* n = nodeOf(id);
        return n ? nb::object(nb::str(n->path().c_str())) : nb::none();
    });
    m.def("node_full_path", [](uint64_t id) -> nb::object {
        scene::Node* n = nodeOf(id);
        return n ? nb::object(nb::str(n->fullPath().c_str())) : nb::none();
    });
    m.def("node_type", [](uint64_t id) -> nb::object {
        scene::Node* n = nodeOf(id);
        return n ? nb::object(nb::str(n->type().name.c_str())) : nb::none();
    });
    m.def("node_is_a", [](uint64_t id, const std::string& type) {
        scene::Node* n = nodeOf(id);
        return n && n->type().isA(type);
    });
    m.def("node_parent", [](uint64_t id) -> nb::object {
        scene::NodeId p = scene::Graph::current().parent(id);
        return p != scene::kNoNode ? nb::object(nb::int_(p)) : nb::none();
    });
    m.def("node_children", [](uint64_t id) {
        auto& g = scene::Graph::current();
        return id == 0 ? std::vector<uint64_t>(g.roots().begin(), g.roots().end())
                       : std::vector<uint64_t>(g.children(id).begin(), g.children(id).end());
    });
    m.def("selection_ids", [] {
        const auto& sel = scene::Graph::current().selection();
        return std::vector<uint64_t>(sel.begin(), sel.end());
    });
    m.def("all_node_ids", [] { return scene::Graph::current().nodes(); });
    m.def("node_attrs", [](uint64_t id) {
        nb::list out;
        if (scene::Node* n = nodeOf(id))
            for (const scene::AttrDef& a : n->type().attrs)
            {
                nb::dict d;
                d["name"] = a.name;
                d["short"] = a.shortName;
                d["type"] = scene::typeName(a.type);
                d["keyable"] = bool(a.flags & scene::AttrKeyable);
                d["output"] = !(a.flags & scene::AttrInput);
                d["hidden"] = bool(a.flags & scene::AttrHidden);
                d["array"] = bool(a.flags & scene::AttrArray);
                d["doc"] = a.doc;
                nb::list children;
                if (a.type == scene::PlugType::Vec3)
                    for (const std::string& c : a.childNames)
                        children.append(nb::str(c.c_str()));
                d["children"] = children;
                nb::list enums;
                for (const std::string& e : a.enumNames)
                    enums.append(nb::str(e.c_str()));
                d["enum"] = enums;
                out.append(d);
            }
        return out;
    });
}
} // namespace lila::python
