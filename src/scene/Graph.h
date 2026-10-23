#pragma once

#include "core/Export.h"

#include "scene/Naming.h"
#include "scene/Node.h"

#include <atomic>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>

namespace lila::scene
{
enum class Change : uint8_t
{
    Created,
    Deleted,
    Renamed,
    Reparented,
    Value,
    Connection,
    Selection,
    Cleared,
    /** Many changes at once (a Graph::Batch ended): anything may have changed. Sent only to batched listeners. */
    Reset,
};

enum class SelectMode : uint8_t
{
    Replace,
    Add,
    Remove,
    Toggle,
};

/**
 * The scene's dependency graph plus its transform hierarchy. Main thread only, except evaluate() which fans
 * compute out over the TBB pool. Every mutation records undo.
 */
class LILA_SCENE_API Graph
{
public:
    static Graph& current();

    Graph();
    ~Graph();

    /** Not undoable. */
    void clear();

    // Nodes
    /** applyNaming=false keeps the name as given (file loads); it is still made unique. */
    Node* createNode(const NodeType& type, std::string_view name = {}, NodeId parent = kNoNode, bool applyNaming = true);
    void deleteNode(NodeId id);
    Node* node(NodeId id) const;
    Node* find(std::string_view name) const;
    /** Returns the name actually given, which is made unique. */
    std::string rename(NodeId id, std::string_view name);
    std::vector<NodeId> nodes() const;
    /** Marks a node as loaded from a file reference (0 = owned by the scene). Not undoable; references manage it. */
    void setReference(NodeId id, uint32_t reference);

    // Hierarchy
    /**
     * index is the position the child takes among the parent's children (SIZE_MAX appends). With the same parent and
     * an index it only reorders.
     */
    void setParent(NodeId child, NodeId parent, bool keepWorld = true, size_t index = SIZE_MAX);
    NodeId parent(NodeId id) const;
    const std::vector<NodeId>& children(NodeId id) const;
    const std::vector<NodeId>& roots() const { return mRoots; }
    std::vector<NodeId> descendants(NodeId id) const;

    // Plugs
    /** "node.attr" or "node.tx". Throws std::invalid_argument. */
    PlugRef plug(std::string_view path) const;
    std::string plugName(const PlugRef& p) const;
    const AttrDef& attr(const PlugRef& p) const;

    /**
     * Throws std::invalid_argument when the plug is driven by a connection, unless the driven-set hook takes the
     * edit. Setting a whole Vec3 whose components are driven stores it and offers each changed driven component to
     * the hook.
     */
    void setValue(const PlugRef& p, PlugValue v);

    /**
     * Maya's attribute lock: a locked plug rejects setValue (the channel box, manipulators and setAttr all go through
     * it). A whole Vec3 counts as locked when all three components are; setting it fails only when a locked
     * component would change. Undoable.
     */
    void setLocked(const PlugRef& p, bool locked);
    bool isLocked(const PlugRef& p) const;
    /** True when the plug or any of its components is locked. */
    bool anyLocked(const PlugRef& p) const;
    /** v with the plug's locked parts put back to their stored values, for tools that move only what they may. */
    PlugValue keepLocked(const PlugRef& p, PlugValue v) const;

    enum class DrivenEdit : uint8_t
    {
        Reject, // the connection wins
        Handled, // the hook changed the source (set a key, say)
        Hold, // keep the given value until the time or the source changes
    };
    /** Decides what a set on a driven scalar plug does. src is the plug driving dst. */
    using DrivenSetHook = std::function<DrivenEdit(Graph& g, const PlugRef& dst, const PlugRef& src, double value)>;
    void setDrivenSetHook(DrivenSetHook fn) { mDrivenSet = std::move(fn); }
    /**
     * The same for a whole Vec3 driven by a Vec3 source (a rotation curve). Setting one component of such a plug
     * offers the whole value with that component replaced.
     */
    using DrivenVecSetHook = std::function<DrivenEdit(Graph& g, const PlugRef& dst, const PlugRef& src, const glm::dvec3& value)>;
    void setDrivenVecSetHook(DrivenVecSetHook fn) { mDrivenVecSet = std::move(fn); }
    /** True while a driven plug shows a held value instead of its source's. */
    bool isHeld(const PlugRef& dst) const { return mHeld.contains(dst); }
    /** Raw stored value, no evaluation. */
    const PlugValue& storedValue(const PlugRef& p) const { return node(p.node)->value(p.attr); }
    std::vector<std::pair<PlugRef, PlugRef>> connections() const { return {mInputs.begin(), mInputs.end()}; }
    /** Evaluates first if the node is dirty. Component plugs return a double. */
    PlugValue value(const PlugRef& p);

    /**
     * Either end may be one component of a Vec3 (node.tx), which carries a float. A component destination and its
     * whole Vec3 can't both be driven; force replaces whichever is there.
     */
    void connect(const PlugRef& src, const PlugRef& dst, bool force = false);
    void disconnect(const PlugRef& dst);
    std::optional<PlugRef> source(const PlugRef& dst) const;
    std::vector<PlugRef> destinations(const PlugRef& src) const;

    // Evaluation
    void evaluate();
    double time() const { return mCtx.time; }
    void setTime(double seconds);
    /** Bumped on every change; the renderer uses it to skip unchanged frames. */
    uint64_t version() const { return mVersion.load(std::memory_order_relaxed); }

    // Selection
    const std::vector<NodeId>& selection() const { return mSelection; }
    void select(const std::vector<NodeId>& ids, SelectMode mode = SelectMode::Replace);
    bool isSelected(NodeId id) const;

    using Listener = std::function<void(Change, NodeId)>;
    /**
     * immediate: called for every change even inside a Batch (model bookkeeping that later edits depend on). Others,
     * typically UI, get one Change::Reset when the outermost Batch ends instead.
     */
    int addListener(Listener fn, bool immediate = false);
    void removeListener(int id);

    /** Scene loads and imports: batched listeners hear nothing until the end, then one Reset. Nests. Main thread. */
    class LILA_SCENE_API Batch
    {
    public:
        explicit Batch(Graph& g = Graph::current());
        ~Batch();
        Batch(const Batch&) = delete;
        Batch& operator=(const Batch&) = delete;

    private:
        Graph& mGraph;
    };
    bool batching() const { return mBatchDepth > 0; }

    static bool validName(std::string_view name);

    void setNamingPolicy(NamingPolicy policy) { mNaming = std::move(policy); }
    const NamingPolicy& namingPolicy() const { return mNaming; }

private:
    friend struct GraphAccess;

    struct Relations
    {
        NodeId parent = kNoNode;
        std::vector<NodeId> children;
    };

    std::string uniqueName(std::string_view wanted, NodeId self = kNoNode) const;
    /** Applies the naming policy; throws in block mode when a user-given name doesn't conform. */
    std::string conformName(const NodeType& type, std::string_view wanted, bool userGiven) const;
    void attach(std::unique_ptr<Node> node, NodeId parent, size_t index);
    std::unique_ptr<Node> detach(NodeId id, size_t* indexOut);
    void reparentRaw(NodeId child, NodeId parent, size_t index);
    void renameRaw(NodeId id, const std::string& name);
    void setRaw(const PlugRef& p, const PlugValue& v);
    void setLockRaw(NodeId id, int attr, uint8_t mask);
    void connectRaw(const PlugRef& src, const PlugRef& dst);
    void disconnectRaw(const PlugRef& dst);
    void selectRaw(std::vector<NodeId> ids);
    void holdRaw(const PlugRef& dst, const PlugValue& v);
    bool setDrivenVec(const PlugRef& dst, const PlugRef& src, const glm::dvec3& v);
    void releaseRaw(const PlugRef& dst);
    /** Routes a set on a driven scalar through the hook. Returns false when it is rejected. */
    bool setDriven(const PlugRef& dst, const PlugRef& src, double v);
    void markDirty(NodeId id);
    bool dependsOn(NodeId node, NodeId upstream) const;
    void changed(Change c, NodeId id);
    /** Node and selection changes of the current graph, to the event bus. */
    void forwardEvent(Change c, NodeId id);
    std::vector<NodeId>& siblings(NodeId parent);

    std::unordered_map<NodeId, std::unique_ptr<Node>> mNodes;
    std::unordered_map<std::string, NodeId> mNames;
    std::unordered_map<NodeId, Relations> mRel;
    std::vector<NodeId> mRoots;
    std::map<PlugRef, PlugRef> mInputs; // dst -> src
    std::unordered_map<NodeId, std::vector<PlugRef>> mOutgoingDst; // src node -> dst plugs it drives
    std::vector<NodeId> mSelection;
    std::set<PlugRef> mHeld; // driven plugs keeping a user-set value
    DrivenSetHook mDrivenSet;
    DrivenVecSetHook mDrivenVecSet;
    struct ListenerEntry
    {
        Listener fn;
        bool immediate = false;
    };
    std::map<int, ListenerEntry> mListeners;
    int mNextListener = 1;
    int mBatchDepth = 0;
    bool mBatchPending = false;
    NodeId mNextId = 1;
    EvalContext mCtx;
    NamingPolicy mNaming;
    std::atomic<uint64_t> mVersion{1};
    bool mAnyDirty = false;
};
} // namespace lila::scene
