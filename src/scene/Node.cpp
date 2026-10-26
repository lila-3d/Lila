#include "scene/Node.h"

#include "core/ExtensionContext.h"
#include "core/Log.h"
#include "scene/AnimCurve.h"
#include "scene/AnimLayer.h"
#include "scene/Take.h"
#include "scene/Camera.h"
#include "scene/Light.h"
#include "scene/Deformers.h"
#include "scene/Drivers.h"
#include "scene/DisplayLayers.h"
#include "scene/ObjectSets.h"
#include "scene/Graph.h"
#include "scene/ImagePlane.h"
#include "scene/ArrayGroup.h"
#include "scene/LodGroup.h"
#include "scene/Materials.h"
#include "scene/Measure.h"
#include "scene/MeshNodes.h"
#include "scene/Rig.h"
#include "scene/Modifiers.h"
#include "scene/Spaces.h"
#include "scene/Transform.h"

#include "core/Log.h"

#include <algorithm>
#include <map>
#include <stdexcept>

namespace lila::scene
{
std::string Node::path() const { return mGraph ? mGraph->path(mId) : mName; }

std::string Node::fullPath() const { return mGraph ? mGraph->fullPath(mId) : "|" + mName; }

int NodeType::attrIndex(std::string_view n) const
{
    for (int i = 0; i < int(attrs.size()); ++i)
        if (attrs[i].name == n || attrs[i].shortName == n)
            return i;
    return -1;
}

int NodeType::fileAttrIndex(std::string_view n) const
{
    if (int i = attrIndex(n); i >= 0)
        return i;
    for (int i = 0; i < int(attrs.size()); ++i)
        if (!attrs[i].fileAlias.empty() && attrs[i].fileAlias == n)
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
Registry<NodeType>& registry()
{
    static Registry<NodeType> r;
    return r;
}

namespace
{
/** Unregistered types stay allocated: nodes and derived types hold pointers to them. */
std::vector<std::unique_ptr<NodeType>>& retired()
{
    static std::vector<std::unique_ptr<NodeType>> r;
    return r;
}
} // namespace

const NodeType& registerType(NodeType type)
{
    if (registry().contains(type.name))
        throw std::invalid_argument("node type '" + type.name + "' already registered");
    type.attrs = type.base ? type.base->attrs : std::vector<AttrDef>{};
    for (const AttrDef& own : type.ownAttrs)
    {
        // An own attr named like a base one overrides it in place (a different default, say), keeping indices.
        auto same = std::find_if(type.attrs.begin(), type.attrs.end(), [&](const AttrDef& a) { return a.name == own.name; });
        if (type.base && same != type.attrs.end() && size_t(same - type.attrs.begin()) < type.base->attrs.size())
            *same = own;
        else
            type.attrs.push_back(own);
    }
    std::string name = type.name;
    std::string owner = type.owner;
    return registry().add(std::move(name), std::move(type), std::move(owner));
}

const NodeType* registerType(ext::Context& ctx, NodeType type)
{
    type.owner = ctx.owner();
    if (type.doc.empty())
        log::warn("extension {}: node type '{}' has no doc, so menus and tooltips can't say what it does", ctx.owner(), type.name);
    std::string name = type.name;
    try
    {
        const NodeType& t = registerType(std::move(type));
        std::string owner = ctx.owner();
        ctx.track("nodeType:" + name, [name, owner] {
            if (const std::string* o = registry().ownerOf(name); o && *o == owner)
                unregisterType(name);
        });
        return &t;
    }
    catch (const std::exception& e)
    {
        log::error("extension {}: {}", ctx.owner(), e.what());
        return nullptr;
    }
}

void unregisterType(std::string_view name)
{
    std::unique_ptr<NodeType> type = registry().take(name);
    if (!type)
        return;
    // create may point into an extension library that is about to be unloaded.
    type->create = nullptr;
    retired().push_back(std::move(type));
}

namespace
{
std::map<std::string, std::string, std::less<>>& fileAliases()
{
    static std::map<std::string, std::string, std::less<>> m;
    return m;
}
} // namespace

const NodeType* find(std::string_view name)
{
    if (const NodeType* t = registry().find(name))
        return t;
    auto& aliases = fileAliases();
    auto it = aliases.find(name);
    return it == aliases.end() ? nullptr : registry().find(it->second);
}

void addFileAlias(std::string_view oldName, std::string_view type)
{
    fileAliases()[std::string(oldName)] = std::string(type);
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
    std::vector<NodeType*> types = registry().all();
    return {types.begin(), types.end()};
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
    lod::registerTypes();
    arrays::registerTypes();
    registerCameraTypes();
    registerLightTypes();
    anim::registerTypes();
    drivers::registerTypes();
    anim::registerLayerTypes();
    anim::registerTakeTypes();
    spaces::registerTypes();
    displayLayers::registerTypes();
    objectSets::registerTypes();
}
} // namespace types
} // namespace lila::scene
