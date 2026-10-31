#pragma once

#include "core/Export.h"

#include "gn/NodeLib.h"

#include <map>
#include <string>

/** The Bake node: what reaches it saved to disk by gnBake and read back in place of the nodes before it. */
namespace lila::gn
{
/** What reached one node's inputs, recorded each time it runs while a CaptureScope names it. */
struct Capture
{
    std::string group, node;
    GeometryPtr geometry;
    std::map<std::string, Value> items;
    bool got = false;
};

/** Points capturing at a node (by group and node name) for its lifetime. One at a time; main thread. */
class LILA_SCENE_API CaptureScope
{
public:
    explicit CaptureScope(Capture& c);
    ~CaptureScope();
    CaptureScope(const CaptureScope&) = delete;
    CaptureScope& operator=(const CaptureScope&) = delete;
};

/** In a node's exec: records its "geometry" input and the named inputs when a capture names it. True when it did. */
LILA_SCENE_API bool recordCapture(const ExecContext& c, const std::vector<std::string>& items = {});

/** Whether something is being captured right now, so nothing upstream may be skipped. */
LILA_SCENE_API bool bakeCaptureActive();
/** A Bake node holding saved results. */
LILA_SCENE_API bool isBaked(const GNode& n);

void registerBakeNode();
void registerBakeCommands();
} // namespace lila::gn
