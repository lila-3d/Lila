// Bake kernels tracing in compute: stack traversal of each high's flat MeshBvh, for GPUs without ray query.
#define TRACE_SW 1
#include "bake_common.slang"
#include "bake_trace.slang"
#include "bake_maps.slang"
