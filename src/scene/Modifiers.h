#pragma once

#include "scene/Node.h"

#include <functional>

namespace lila::scene
{
class Graph;

/**
 * A non-destructive mesh modifier: inMesh in, outMesh out, keyable parameters. Modifiers chain between a mesh's
 * source (a generator or a stored base mesh) and the mesh node, so the stack is just DG history:
 *
 *   polyCube -> bevelModifier -> mirrorModifier -> Cube.inMesh
 */
class ModifierNode : public Node
{
public:
    using Apply = std::function<geo::Mesh(const geo::Mesh&, const Node&)>;

    void compute(const EvalContext& ctx) override;

    /** True while this modifier shows its input for editing: the cage becomes its input and it and later modifiers pass through. */
    virtual bool editingInput() const { return false; }

    /** Why the last evaluation failed (the input passed through), empty when it succeeded. Read after Graph::evaluate. */
    const std::string& error() const { return mError; }

    Apply apply;
    int inAttr = 0;
    int enabledAttr = 1;
    int outAttr = -1;

protected:
    std::string mError;
};

/** Holds a base mesh upstream of modifiers, so direct edits never disturb the stack. */
class MeshSourceNode : public Node
{
public:
    enum Attr
    {
        kMesh,
        kOutMesh,
    };
    void compute(const EvalContext& ctx) override;
};

namespace modifiers
{
/** Point layer marking a mesh that passes through the rest of its stack untouched (a modifier upstream is in edit mode). */
inline constexpr const char* kBypassLayer = "lila:editBypass";
bool bypassed(const MeshPtr& mesh);

bool isModifier(const Node& n);

/** Modifiers feeding a mesh node, first applied first. */
std::vector<NodeId> stack(Graph& g, NodeId mesh);

/** The storable plug holding the base mesh direct edits change. Bakes a procedural source into a stored one. */
PlugRef editPlug(Graph& g, NodeId mesh);

/** The base mesh without modifiers (the cage), as components are picked and edited on it. Doesn't change the graph. */
MeshPtr cage(Graph& g, NodeId mesh);

/** Kinds addModifier can create ("bevel" for bevelModifier), from the type registry, so extension modifiers appear too. Sorted. */
std::vector<std::string> kinds();

/** What a kind ("bevel") does, from its node type's doc. Empty for an unknown or undocumented kind. */
std::string kindDoc(std::string_view kind);

/** What feeds the bottom of the stack: a generator, a meshSource, or kNoNode when the mesh stores its own base. */
NodeId stackSource(Graph& g, NodeId mesh);

/** The mesh node at the top of a modifier's chain, kNoNode if it feeds none. */
NodeId meshOf(Graph& g, NodeId modifier);

/** Whether a modifier is applied on export (its exportEnabled attr; true when the type has none). */
bool exportEnabled(const Node& modifier);

/** The mesh as exporters should write it: outMesh, or the stack re-run without the modifiers turned off for export. */
MeshPtr exportMesh(Graph& g, NodeId mesh);

void registerTypes();
void registerCommands();
/** duplicateModifier, applyAllModifiers, collapseStack, modifierStack (ModifierCommands.cpp). */
void registerStackCommands();
} // namespace modifiers
} // namespace lila::scene
