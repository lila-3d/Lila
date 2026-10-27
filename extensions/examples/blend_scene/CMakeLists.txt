# Sample C++ file translator for .blend files, built as <build>/bin/<config>/extensions/blendScene.lilaext.
find_package(zstd CONFIG REQUIRED)
lila_add_extension(lila_blend_scene_extension ID blendScene MANIFEST extension.json SOURCES BlendScene.cpp)
target_link_libraries(lila_blend_scene_extension PRIVATE zstd::libzstd)
