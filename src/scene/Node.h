#pragma once

#include "scene/Plug.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace lila::scene
{
class Graph;

struct EvalContext
{
    double time = 0.0; // seconds
};

class Node;

struct NodeType
{
    std::string name;
    const NodeType* base = nullptr;
    bool dag = false; // lives in the transform hierarchy
    bool timeDependent = false; // recomputed whenever the graph's time changes
    std::string owner; // extension that registered it; empty = built in
    /** What the node does and when to use it, one or two sentences. Tooltips, menus and the API reference show it. */
    std::string doc;
    /** Tabler icon name for the outliner and other node lists; empty inherits the base type's. */
    std::string icon;
    std::vector<AttrDef> ownAttrs;
    std::function<std::unique_ptr<Node>()> create;

    /** Base attrs first, then own. Filled by registerType. */
    std::vector<AttrDef> attrs;

    int attrIndex(std::string_view name) const;
    bool isA(std::string_view typeName) const;
};

namespace types
{
const NodeType& registerType(NodeType type);
/** Drops a type from lookup. It stays allocated for anything still pointing at it, but can no longer create nodes. */
void unregisterType(std::string_view name);
const NodeType* find(std::string_view name);
std::vector<const NodeType*> all();
/** The first icon set on the type or one of its bases, else fallback. */
const std::string& iconFor(const NodeType& type, const std::string& fallback);
void registerBuiltins();
} // namespace types

/**
 * A DG node. Values live here; connections and hierarchy live in Graph. compute() runs on a worker thread and may
 * only read this node's values (inputs are already pulled from their sources) and write its outputs.
 */
class Node
{
public:
    virtual ~Node() = default;

    NodeId id() const { return mId; }
    const NodeType& type() const { return *mType; }
    const std::string& name() const { return mName; }

    /** "" for the root namespace. */
    const std::string& nameSpace() const { return mNamespace; }

    /** Reference this node was loaded from; 0 for nodes owned by the scene. */
    uint32_t referenceId() const { return mReference; }

    const PlugValue& value(int attr) const { return mValues[attr]; }
    /** Locked parts of an attr: bit 0 for a scalar, bits 0-2 for a Vec3's components. See Graph::setLocked. */
    uint8_t lockMask(int attr) const { return size_t(attr) < mLocks.size() ? mLocks[size_t(attr)] : uint8_t(0); }
    template <class T> const T& as(int attr) const { return std::get<T>(mValues[attr]); }

    virtual void compute(const EvalContext& ctx) { (void)ctx; }

protected:
    template <class T> void setOutput(int attr, T v) { mValues[attr] = std::move(v); }

private:
    friend class Graph;

    NodeId mId = kNoNode;
    const NodeType* mType = nullptr;
    std::string mName;
    std::string mNamespace;
    uint32_t mReference = 0;
    std::vector<PlugValue> mValues;
    std::vector<uint8_t> mLocks; // per attr, empty until something is locked
    bool mDirty = true;
};
} // namespace lila::scene
