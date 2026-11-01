#include "ui/NodeHandles.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/Transform.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportTool.h"

#include <QApplication>

#include <map>
#include <vector>

namespace lila::ui::nodeHandles
{
namespace
{
/** Past this many selected nodes their handles would only clutter the view. */
constexpr size_t kMaxNodes = 32;

std::map<std::string, const Provider*, std::less<>>& handlesByType()
{
    static std::map<std::string, const Provider*, std::less<>> m;
    return m;
}

std::vector<Driver*>& drivers()
{
    static std::vector<Driver*> d;
    return d;
}

/** Selected nodes with handles, as {node, provider, context world}. */
template <class Fn>
void forEachHandled(Fn&& fn)
{
    if (viewportTools::active() || scene::components::mode() != scene::ComponentMode::Object)
        return;
    scene::Graph& g = scene::Graph::current();
    size_t n = 0;
    for (scene::NodeId id : g.selection())
    {
        const scene::Node* node = g.node(id);
        if (!node || viewportHooks::hiddenInView(id))
            continue;
        const Provider* p = forType(node->type());
        if (!p)
            continue;
        if (++n > kMaxNodes)
            return;
        fn(*node, *p, node->as<glm::dmat4>(scene::Transform::kWorldMatrix));
    }
}
} // namespace

Registry<Provider>& registry()
{
    static Registry<Provider>* r = [] {
        auto* reg = new Registry<Provider>();
        reg->addListener([reg](RegistryChange change, const std::string& id, const std::string&) {
            auto& m = handlesByType();
            m.clear();
            for (const Provider* p : reg->all())
                m.emplace(p->nodeType, p);
            if (change == RegistryChange::Removed)
                for (Driver* d : drivers())
                    d->cancel();
            (void)id;
        });
        return reg;
    }();
    return *r;
}

const Provider& add(Provider p)
{
    std::string id = p.id;
    return registry().add(std::move(id), std::move(p));
}

const Provider* add(ext::Context& ctx, Provider p)
{
    std::string id = p.id;
    try
    {
        ctx.add(registry(), "nodeHandles", id, std::move(p));
        return registry().find(id);
    }
    catch (const std::exception& e)
    {
        log::error("extension {}: node handles: {}", ctx.owner(), e.what());
        return nullptr;
    }
}

bool remove(std::string_view id) { return registry().remove(id); }

const Provider* forType(const scene::NodeType& t)
{
    registry();
    const auto& m = handlesByType();
    if (m.empty())
        return nullptr;
    for (const scene::NodeType* type = &t; type; type = type->base)
        if (auto it = m.find(type->name); it != m.end())
            return it->second;
    return nullptr;
}

Driver::Driver() { drivers().push_back(this); }

Driver::~Driver()
{
    cancel();
    std::erase(drivers(), this);
}

std::optional<Driver::Hit> Driver::pick(QPointF px, const ViewInfo& view) const
{
    std::optional<Hit> hit;
    forEachHandled([&](const scene::Node& node, const Provider& p, const glm::dmat4& world) {
        if (hit || !p.pick)
            return;
        if (int h = p.pick(px, {view, node, world}); h >= 0)
            hit = Hit{node.id(), p.id, h};
    });
    return hit;
}

bool Driver::hover(QPointF px, const ViewInfo& view)
{
    std::optional<Hit> h = dragging() ? mHover : pick(px, view);
    const bool changed = h != mHover;
    mHover = h;
    return changed;
}

bool Driver::press(QPointF px, const ViewInfo& view)
{
    cancel();
    std::optional<Hit> h = pick(px, view);
    if (!h)
        return false;
    const Provider* p = registry().find(h->provider);
    const scene::Node* node = scene::Graph::current().node(h->node);
    if (!p || !node)
        return false;
    if (p->begin)
        mDrag = p->begin(h->handle, px, {view, *node, node->as<glm::dmat4>(scene::Transform::kWorldMatrix)});
    mActive = mDrag ? h : std::nullopt;
    mPress = mLast = px;
    mMoved = false;
    return true;
}

void Driver::drag(QPointF px, const ViewInfo& view, Qt::KeyboardModifiers mods)
{
    if (!mDrag)
        return;
    mLast = px;
    if (!mMoved && (px - mPress).manhattanLength() < QApplication::startDragDistance())
        return;
    mMoved = true;
    mDrag->move(px, view, mods);
}

void Driver::release()
{
    if (std::unique_ptr<Drag> d = std::move(mDrag))
        d->release(mMoved);
    mActive.reset();
}

void Driver::cancel()
{
    if (std::unique_ptr<Drag> d = std::move(mDrag))
        d->cancel();
    mActive.reset();
    mHover.reset();
}

void Driver::draw(OverlayPainter& p, const ViewInfo& view)
{
    LILA_ZONE();

    forEachHandled([&](const scene::Node& node, const Provider& prov, const glm::dmat4& world) {
        if (!prov.draw)
            return;
        auto handleOf = [&](const std::optional<Hit>& h) { return h && h->node == node.id() && h->provider == prov.id ? h->handle : -1; };
        prov.draw(p, {view, node, world}, handleOf(mHover), handleOf(mActive));
    });
    if (mDrag && mMoved)
        if (QString text = mDrag->readout(); !text.isEmpty())
            p.label(mLast + QPointF(18, -14), text);
}
} // namespace lila::ui::nodeHandles
