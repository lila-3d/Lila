#include "ui/ViewportContext.h"

#include "scene/Graph.h"
#include "scene/Materials.h"

#include <QCoreApplication>
#include <QTimer>

#include <map>

namespace lila::ui::viewportContext
{
namespace
{
struct State
{
    uint32_t value = 0;
    uint32_t notified = 0;
    bool dirty = true;
    bool pending = false;
    bool installed = false;
    int nextId = 1;
    std::map<int, std::function<void()>> listeners;
};

State& state()
{
    static State s;
    return s;
}

void notifyIfChanged()
{
    State& s = state();
    s.pending = false;
    const uint32_t v = present();
    if (v == s.notified)
        return;
    s.notified = v;
    // Copied: a listener may add or remove listeners.
    auto listeners = s.listeners;
    for (auto& [id, fn] : listeners)
        fn();
}

void schedule()
{
    State& s = state();
    s.dirty = true;
    if (s.pending || s.listeners.empty() || !QCoreApplication::instance())
        return;
    s.pending = true;
    QTimer::singleShot(50, QCoreApplication::instance(), notifyIfChanged);
}

void install()
{
    State& s = state();
    if (std::exchange(s.installed, true))
        return;
    scene::Graph& g = scene::Graph::current();
    g.addListener([&g](scene::Change c, scene::NodeId id) {
        switch (c)
        {
        case scene::Change::Created:
        case scene::Change::Deleted:
        case scene::Change::Cleared:
        case scene::Change::Reset: schedule(); break;
        // Value edits are frequent (every drag step), so only a material's can change what the scene holds.
        case scene::Change::Value:
        case scene::Change::Connection:
            if (const scene::Node* n = g.node(id); n && n->type().isA("material"))
                schedule();
            break;
        default: break;
        }
    });
    s.notified = present();
}
} // namespace

uint32_t scan(const scene::Graph& g)
{
    uint32_t out = 0;
    for (scene::NodeId id : g.nodes())
    {
        const scene::Node* n = g.node(id);
        if (!n)
            continue;
        const scene::NodeType& t = n->type();
        if (t.isA("joint"))
            out |= Joints;
        else if (t.isA("material"))
        {
            out |= Materials;
            if (n->as<int64_t>(scene::material::kShadingModel) != int64_t(scene::material::ShadingModel::DefaultLit))
                out |= StylisedMaterials;
        }
    }
    return out;
}

uint32_t present()
{
    State& s = state();
    install();
    if (std::exchange(s.dirty, false))
        s.value = scan(scene::Graph::current());
    return s.value;
}

int addListener(std::function<void()> fn)
{
    install();
    State& s = state();
    int id = s.nextId++;
    s.listeners[id] = std::move(fn);
    return id;
}

void removeListener(int id)
{
    state().listeners.erase(id);
}
} // namespace lila::ui::viewportContext
