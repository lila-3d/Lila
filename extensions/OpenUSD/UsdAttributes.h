#pragma once

#include <lila/Graph.h>
#include <lila/Plug.h>

#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/valueTypeName.h>
#include <pxr/usd/usd/prim.h>

#include <optional>
#include <set>
#include <string>
#include <vector>

/**
 * Components: applied API schemas Lila has no feature for (a studio's HealthAPI, PhysicsRigidBodyAPI) and custom
 * attributes. Their attributes become addAttr attributes on the prim's node, headed by the schema's name in the Attribute
 * Editor, and the schemas themselves are listed in the node's usdApiSchemas string array. Export and the live stage write
 * both back. Values are carried as authored: no axis or unit conversion.
 */
namespace lila::usd
{
/** The string array on a node naming its components' schemas, as apiSchemas names them ("CollectionAPI:foo" for multi-apply). */
inline constexpr const char* kSchemasAttr = "usdApiSchemas";

/** An applied schema Lila reads or writes as part of a feature (lights, bindings, skins), so not a component. */
bool lilaSchema(const pxr::TfToken& applied);

/** The component schemas among applied ones. */
pxr::TfTokenVector componentSchemas(const pxr::TfTokenVector& applied);

/** The attribute definition of name in one applied schema (instance names filled in), or empty when it has none. */
pxr::UsdPrimDefinition::Attribute schemaAttribute(const pxr::TfToken& applied, const pxr::TfToken& name);

/** Names of the attributes the schemas define, instance names filled in. */
std::set<pxr::TfToken> schemaAttributeNames(const pxr::TfTokenVector& schemas);

/** The attribute a USD value type and its allowed tokens become in Lila, or empty for a type Lila has no attribute for. */
std::optional<scene::AttrDef> attrDefFor(const std::string& name, const pxr::SdfValueTypeName& type, const pxr::VtTokenArray& allowed);

/** A USD value as the elements of a Lila attribute (one for a scalar), or empty when it doesn't convert. */
std::optional<std::vector<scene::PlugValue>> plugValues(const pxr::VtValue& v, const scene::AttrDef& def);

/** A Lila attribute's elements as a value of a USD type, or empty when they don't convert. */
pxr::VtValue usdValue(const std::vector<scene::PlugValue>& values, const scene::AttrDef& def, const pxr::SdfValueTypeName& type);

/** The USD type a Lila attribute is written as when no schema names one. */
pxr::SdfValueTypeName usdTypeFor(const scene::AttrDef& def);

/** A node attribute's value, as elements. */
std::vector<scene::PlugValue> nodeValues(const scene::Node& n, int attr);

/**
 * The prim's components on its node at time t: the attributes added (or updated, on a node imported again) and
 * usdApiSchemas set. Attributes Lila can't hold are listed in skipped as "path (type)".
 */
void importComponents(scene::Graph& g, scene::NodeId id, const pxr::UsdPrim& prim, pxr::UsdTimeCode t, std::vector<std::string>& skipped);

/** A node's components onto its prim: the schemas applied, then every addAttr attribute written. */
void exportComponents(const scene::Graph& g, scene::NodeId id, const pxr::UsdPrim& prim);

/** Names of a written prim spec's component attributes: custom ones and those of its component schemas. */
std::set<std::string> componentAttributes(const pxr::SdfPrimSpecHandle& spec);

/** The component schemas a prim spec applies, from its apiSchemas list op. */
pxr::TfTokenVector specComponentSchemas(const pxr::SdfPrimSpecHandle& spec);
} // namespace lila::usd
