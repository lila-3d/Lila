add_library(lila_render STATIC
    Backdrop.cpp
    Environment.cpp
    HdrImage.cpp
    Renderer.cpp
    Surface.cpp
    VulkanContext.cpp)

target_link_libraries(lila_render PUBLIC lila_core volk::volk Vulkan::Headers GPUOpen::VulkanMemoryAllocator)

lila_add_shaders(lila_render FILES
    shaders/ao.slang
    shaders/contact.slang
    shaders/grid.slang
    shaders/imageplane.slang
    shaders/lines.slang
    shaders/overlay.slang
    shaders/mesh.slang
    shaders/wire.slang)
