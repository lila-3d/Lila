lila_module(lila_render UI
    Backdrop.cpp
    Baker.cpp
    Environment.cpp
    HdrImage.cpp
    Renderer.cpp
    Surface.cpp
    VulkanContext.cpp)

target_link_libraries(lila_render PUBLIC lila_core volk::volk Vulkan::Headers GPUOpen::VulkanMemoryAllocator)

lila_add_shaders(lila_render FILES
    shaders/ao.slang
    shaders/bake_dilate.slang
    shaders/bake_hw.slang
    shaders/bake_image.slang
    shaders/bake_raster32.slang
    shaders/bake_raster64.slang
    shaders/bake_sw.slang
    shaders/contact.slang
    shaders/grid.slang
    shaders/imageplane.slang
    shaders/lines.slang
    shaders/overlay.slang
    shaders/mesh.slang
    shaders/wire.slang
    INCLUDES
    shaders/bake_common.slang
    shaders/bake_maps.slang
    shaders/bake_raster.slang
    shaders/bake_trace.slang)
