#pragma once

#include "core/Export.h"
#include "core/Registry.h"

#include "scene/Node.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * Info, warnings and errors that validators attach to objects, their components and their attributes. The outliner
 * badges the rows, the Attribute Editor marks the sections and fields, and the Validation panel lists them all. Not
 * saved with the scene and not undoable: validators report them again on their next run. Main thread only.
 *
 * A target is "<object>[.<component>[:<attribute>]]". The object is a Lila path ("|group|crate", "crate") or anything a
 * registered resolver takes (a USD prim path "/World/crate"). The part after the dot names an attribute when the node
 * has one by that name ("crate.translateX", "crate.health:max"); otherwise it is a component, matched against the
 * node's types, attribute categories, Attribute Editor sections and attribute namespaces, with ":<attribute>" naming
 * an attribute in it ("<component>:<attribute>" first, then "<attribute>"). An issue whose object resolves to no node
 * stays listed in the panel and attaches when a node for it appears.
 */
namespace lila::scene::diagnostics
{
enum class Severity : uint8_t
{
    Info,
    Warning,
    Error,
};

LILA_SCENE_API const char* severityName(Severity s);
LILA_SCENE_API std::optional<Severity> parseSeverity(std::string_view name);

struct Issue
{
    /** Assigned by report: unique for the session. */
    uint64_t id = 0;
    /** Who reported it, so it can clear its own: the validator's name while one runs, else "user". */
    std::string source;
    Severity severity = Severity::Warning;
    /** As reported. */
    std::string target;
    /** The target's object, its component and attribute parts. */
    std::string object, component, attr;
    /** kNoNode while the object resolves to nothing (or names the scene, when empty). */
    NodeId node = kNoNode;
    std::string message;
    /** Commands it refuses to let run while it stands ("exportFbx", "saveScene"). */
    std::vector<std::string> blocks;
    /** Python run by fixIssue. */
    std::string fix;

    bool blocking() const { return !blocks.empty(); }
};

/** The parts of a target string. Throws CommandError on an empty object after a dot. */
struct Target
{
    std::string object, component, attr;
};
LILA_SCENE_API Target parseTarget(std::string_view target);

/** Adds an issue and returns its id. Fills node, object, component and attr from target unless they are set. */
LILA_SCENE_API uint64_t report(Issue issue);
/** Matching issues: empty source, node kNoNode or no severity match any. Returns how many went. */
LILA_SCENE_API size_t clear(std::string_view source = {}, std::optional<NodeId> node = std::nullopt, std::optional<Severity> severity = std::nullopt);
LILA_SCENE_API bool remove(uint64_t id);
LILA_SCENE_API const std::vector<Issue>& issues();
LILA_SCENE_API const Issue* find(uint64_t id);
/** The node's own issues. */
LILA_SCENE_API std::vector<const Issue*> issuesOf(NodeId node);
/** Bumped on every change. */
LILA_SCENE_API uint64_t version();
/** Called after every change, coalesced by nothing: listeners schedule their own refresh. */
LILA_SCENE_API int addListener(std::function<void()> fn);
LILA_SCENE_API void removeListener(int id);

/** Whether the node's issue applies to a component (a type, category, section or namespace) the editor shows. */
LILA_SCENE_API bool componentMatches(std::string_view component, std::string_view typeName, std::string_view category, std::string_view attr);
/** Whether an issue's attribute names attr (a whole attribute or one of its elements, "weights[3]"). */
LILA_SCENE_API bool attrMatches(const Issue& issue, std::string_view attr);

/** Turns an object path no Lila path matches into a node, or kNoNode. Registered under a name with an owner. */
using Resolver = std::function<NodeId(std::string_view object)>;
LILA_SCENE_API Registry<Resolver>& resolvers();

/** A check run by the validate command; it reports with source set to its name, after its old issues are cleared. */
struct Validator
{
    std::string doc;
    std::function<void()> run;
};
LILA_SCENE_API Registry<Validator>& validators();
/** Runs the named validators, every one when names is empty. Throws CommandError for an unknown name. */
LILA_SCENE_API void validate(const std::vector<std::string>& names = {});

/** Blocking issues that stop the command, in report order. */
LILA_SCENE_API std::vector<const Issue*> blockers(std::string_view command);

/** reportIssue, clearIssues, listIssues, fixIssue, validate, addValidator, removeValidator; installs the command gate. */
LILA_SCENE_API void registerCommands();
} // namespace lila::scene::diagnostics
