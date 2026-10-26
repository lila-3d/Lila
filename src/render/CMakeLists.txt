lila_module(lila_render UI
    Backdrop.cpp
    Baker.cpp
    Compute.cpp
    DeformGpu.cpp
    DrawBatches.cpp
    Environment.cpp
    GpuImage.cpp
    HdrImage.cpp
    MeshMaps.cpp
    PaintCommands.cpp
    PaintGpu.cpp
    Renderer.cpp
    ShadingModels.cpp
    Surface.cpp
    TexEval.cpp
    TextureFormat.cpp
    VirtualTexture.cpp
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
    shaders/deform.slang
    shaders/grid.slang
    shaders/imageplane.slang
    shaders/lines.slang
    shaders/overlay.slang
    shaders/paint_dab.slang
    shaders/paint_depth.slang
    shaders/paint_gather.slang
    shaders/paint_screen.slang
    shaders/post.slang
    shaders/mesh.slang
    shaders/mesh_bindless.slang
    shaders/tex_blur.slang
    shaders/tex_decode.slang
    shaders/tex_page.slang
    shaders/tex_store.slang
    shaders/tex_uvfill.slang
    shaders/vt.slang
    shaders/wire.slang
    INCLUDES
    shaders/mesh.slang
    shaders/stylised.slang
    shaders/bake_common.slang
    shaders/bake_maps.slang
    shaders/bake_raster.slang
    shaders/bake_trace.slang
    shaders/tex_formats.slang)
