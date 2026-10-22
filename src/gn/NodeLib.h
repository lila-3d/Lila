#pragma once

#include "gn/Field.h"
#include "gn/Group.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace lila::gn
{
/** What an unconnected field input reads instead of its value. */
enum class Implicit : uint8_t
{
    None,
    Position,
    Normal,
    Index,
    Id,
};

struct SocketDecl
{
    std::string id;
    SocketType type = SocketType::Float;
    json defaultValue;
    /** Inputs: accepts a field. Outputs: produces one (drawn as a diamond). */
    bool field = false;
    /** Takes any number of links (Join Geometry). */
    bool multi = false;
    Implicit implicit = Implicit::None;
    std::string doc;
};

/** A non-linkable choice on a node, such as the Math operation. Free text when options is empty. */
struct PropDecl
{
    std::string id;
    std::vector<std::string> options;
    std::string defaultValue;
    std::string doc;
};

struct Sockets
{
    std::vector<SocketDecl> inputs;
    std::vector<SocketDecl> outputs;

    int inputIndex(std::string_view id) const;
    int outputIndex(std::string_view id) const;
};

class ExecContext;

struct NodeDecl
{
    std::string type;
    std::string label;
    std::string category;
    std::string doc;
    std::vector<PropDecl> props;
    Sockets sockets;
    /** For nodes whose sockets follow a property or the group interface. */
    std::function<Sockets(const Group&, const GNode&)> dynamicSockets;
    std::function<void(ExecContext&)> exec;

    Sockets socketsFor(const Group& g, const GNode& n) const;
    const PropDecl* prop(std::string_view id) const;
    /** The node's value of a property, or its default. */
    std::string propValue(const GNode& n, std::string_view id) const;
};

void registerDecl(NodeDecl d);
const NodeDecl* findDecl(std::string_view type);
std::vector<const NodeDecl*> allDecls();
/** Fills the registry with the built-in node library. Safe to call twice. */
void registerNodeLibrary();

/** Whether an output of one type may feed an input of another. */
bool linkable(SocketType from, SocketType to);
/**
 * The socket on a new node of this type that a dragged socket of `type` connects to: one of its inputs when
 * wantInput (the drag came from an output), else one of its outputs. Same type first, then the first linkable one.
 * Empty when nothing fits.
 */
std::string compatibleSocket(const Group& g, const NodeDecl& d, SocketType type, bool wantInput);

/** What a socket carries during evaluation. Which member is used follows the socket type. */
struct Value
{
    GeometryPtr geometry;
    FieldPtr field;
    std::string string;
};

/** Another scene object as seen by an Object Info node. */
struct ObjectInput
{
    std::shared_ptr<const geo::Mesh> mesh;
    glm::dmat4 matrix{1.0};
};

/** Everything an evaluation can read from outside the group. */
struct ExecEnv
{
    double time = 0.0; // seconds
    GeometryPtr input;
    std::map<std::string, Value> params;        // group inputs by name
    std::map<std::string, ObjectInput> objects; // by Object Info node name
};

/** Turns a JSON value (node value, exposed default) into a socket value. Rotations are given in degrees. */
Value valueFromJson(SocketType t, const json& j);
/** Converts a value along a link of another type; incompatible pairs give the default. */
Value convertValue(const Value& v, SocketType from, SocketType to);
Value defaultValue(SocketType t);

class ExecContext
{
public:
    ExecContext(const Group& g, const GNode& n, const NodeDecl& d, const Sockets& s, const ExecEnv& env)
        : group(g), node(n), decl(d), sockets(s), env(env)
    {
    }

    const Group& group;
    const GNode& node;
    const NodeDecl& decl;
    const Sockets& sockets;
    const ExecEnv& env;
    std::vector<std::vector<Value>> inputs; // per input socket; several only for multi inputs
    std::vector<Value> outputs;

    /** Never null; an empty geometry when unconnected. */
    GeometryPtr geometry(std::string_view id) const;
    std::vector<GeometryPtr> geometries(std::string_view id) const;
    FieldPtr field(std::string_view id) const;
    double f(std::string_view id) const;
    int64_t i(std::string_view id) const;
    bool b(std::string_view id) const;
    glm::dvec3 v(std::string_view id) const;
    std::string s(std::string_view id) const;
    std::string prop(std::string_view id) const { return decl.propValue(node, id); }

    void out(std::string_view id, GeometryPtr g);
    void out(std::string_view id, FieldPtr f);
    void outString(std::string_view id, std::string s);
    /** Attribute name for a field output backed by data the node stored on the geometry. */
    std::string anonymous(std::string_view id) const { return "__" + node.name + "_" + std::string(id); }

private:
    const Value& in(std::string_view id) const;
};

/** Thrown by nodes for a readable evaluation error. */
struct ExecError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};
} // namespace lila::gn
