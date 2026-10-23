#include "render/RenderMesh.h"

namespace lila::render
{
uint64_t nextResourceKey()
{
    static std::atomic<uint64_t> counter{1};
    return counter++;
}
} // namespace lila::render
