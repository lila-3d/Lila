#include "gn/NodesCommon.h"

namespace lila::gn
{
void registerNodeLibrary()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    nodes::registerInputNodes();
    nodes::registerUtilityNodes();
    nodes::registerGeometryNodes();
    nodes::registerMeshNodes();
    nodes::registerCurveNodes();
    nodes::registerSampleNodes();
    nodes::registerBooleanNode();
}
} // namespace lila::gn
