# The sample extensions, built against an installed SDK:
#   cmake -S <sdk>/examples -B build -G Ninja -DCMAKE_PREFIX_PATH=<sdk>
#   cmake --build build
# Each lands in build/<config>/extensions/<Id>.lilaext; point LILA_EXTENSION_PATH there to load them.
cmake_minimum_required(VERSION 3.28)
project(LilaExtensionExamples CXX)
find_package(LilaSDK REQUIRED)
add_subdirectory(hello)
add_subdirectory(scene_points)
add_subdirectory(ui_mode)
add_subdirectory(viewport_points)
