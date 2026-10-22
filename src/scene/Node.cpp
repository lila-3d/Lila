#include "scene/Node.h"

#include "scene/AnimCurve.h"
#include "scene/Camera.h"
#include "scene/Deformers.h"
#include "scene/DisplayLayers.h"
#include "scene/ImagePlane.h"
#include "scene/Materials.h"
#include "scene/Measure.h"
#include "scene/MeshNodes.h"
#include "scene/Rig.h"
#include "scene/Modifiers.h"
#include "scene/Transform.h"

#include <map>
#include <stdexcept>

namespace lila::scene
{
int NodeType::attrIndex(std::string_view n) const
{
    for (int i = 0; i < int(attrs.size()); ++i)
        if (attrs[i].name == n || attrs[i].shortName == n)
            return i;
    return -1;
}

bool NodeType::isA(std::string_view typeName) const
{
    for (const NodeType* t = this; t; t = t->base)
        if (t->name == typeName)
            return true;
    return false;
}

namespace types
{
namespace
{
// Pointers must stay stable: nodes and derived types hold them.
std::map<std::string, std::unique_ptr<NodeType>, std::less<>>& registry()
{
    static std::map<std::string, std::unique_ptr<NodeType>, std::less<>> r;
    return r;
}
} // namespace

const NodeType& registerType(NodeType type)
{
    if (registry().contains(type.name))
        throw std::invalid_argument("node type '" + type.name + "' already registered");
    type.attrs = type.base ? type.base->attrs : std::vector<AttrDef>{};
    type.attrs.insert(type.attrs.end(), type.ownAttrs.begin(), type.ownAttrs.end());
    std::string name = type.name;
    auto& slot = registry()[name];
    slot = std::make_unique<NodeType>(std::move(type));
    return *slot;
}

void unregisterType(std::string_view name)
{
    static std::vector<std::unique_ptr<NodeType>> retired;
    auto it = registry().find(name);
    if (it == registry().end())
        return;
    // create may point into an extension library that is about to be unloaded.
    it->second->create = nullptr;
    retired.push_back(std::move(it->second));
    registry().erase(it);
}

const NodeType* find(std::string_view name)
{
    auto it = registry().find(name);
    return it == registry().end() ? nullptr : it->second.get();
}

const std::string& iconFor(const NodeType& type, const std::string& fallback)
{
    for (const NodeType* t = &type; t; t = t->base)
        if (!t->icon.empty())
            return t->icon;
    return fallback;
}

std::vector<const NodeType*> all()
{
    std::vector<const NodeType*> out;
    for (auto& [n, t] : registry())
        out.push_back(t.get());
    return out;
}

void registerBuiltins()
{
    if (find("transform"))
        return;
    const NodeType& transform = registerType(Transform::makeType());
    registerType(Locator::makeType(transform));
    registerMeshTypes();
    modifiers::registerTypes();
    materials::registerTypes();
    registerRigTypes();
    deformers::registerTypes();
    registerMeasureTypes();
    registerImagePlaneTypes();
    registerCameraTypes();
    anim::registerTypes();
    displayLayers::registerTypes();
}
} // namespace types
} // namespace lila::scene
