#include "gn/Bake.h"
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
    nodes::registerTextureNodes();
    nodes::registerShadingNodes();
    nodes::registerGroupNodes();
    nodes::registerRoutingNodes();
    nodes::registerDestructionNodes();
    nodes::registerMeshOpsNodes();
    nodes::registerTopologyNodes();
    nodes::registerAnalysisNodes();
    nodes::registerFieldNodes();
    nodes::registerPatternNodes();
    nodes::registerCurveOpsNodes();
    nodes::registerInstanceNodes();
    nodes::registerGameNodes();
    nodes::registerDataNodes();
    nodes::registerProceduralNodes();
    nodes::registerVolumeNodes();
    nodes::registerGridNodes();
    nodes::registerToolNodes();
    nodes::registerWrangleNode();
    nodes::registerZoneNodes();
    nodes::registerMatrixNodes();
    nodes::registerBundleNodes();
    registerBakeNode();
}
} // namespace lila::gn
