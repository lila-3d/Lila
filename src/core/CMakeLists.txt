# The crash report and capture code, without Lila's dependencies, for the lilacrash watchdog and the lilawer WER helper.
# LilaFoundation compiles the same sources for itself.
set(LILA_CRASHKIT_SOURCES CrashReport.cpp CrashCapture.cpp CrashWatch.cpp)
set(LILA_CRASHKIT_LIBS)
if(WIN32)
    set(LILA_CRASHKIT_LIBS dbghelp version wevtapi shcore dxgi)
endif()
add_library(lila_crashkit STATIC ${LILA_CRASHKIT_SOURCES})
target_include_directories(lila_crashkit PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(lila_crashkit PUBLIC LILA_CRASHKIT_STATIC)
target_link_libraries(lila_crashkit PUBLIC ${LILA_CRASHKIT_LIBS})

lila_module(lila_core Foundation
    ${LILA_CRASHKIT_SOURCES}
    CrashHandler.cpp
    Events.cpp
    FileBytes.cpp
    ExtensionContext.cpp
    ExtensionManifest.cpp
    ImageIO.cpp
    Log.cpp
    MainQueue.cpp
    Paths.cpp
    PrefStore.cpp
    Progress.cpp
    ProjectTextures.cpp
    Registry.cpp
    ResourceKey.cpp
    SystemMemory.cpp
    TextMatch.cpp
    Version.cpp
    Workspace.cpp)

find_package(miniz CONFIG REQUIRED)
target_link_libraries(lila_core PRIVATE miniz::miniz)
target_include_directories(lila_core PUBLIC ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/sdk/include)
target_link_libraries(lila_core PUBLIC nlohmann_json::nlohmann_json TBB::tbb glm::glm ${LILA_CRASHKIT_LIBS})
# Out of the unity batch: the stamp changes with every commit and should recompile only this file.
set_source_files_properties(Version.cpp PROPERTIES INCLUDE_DIRECTORIES ${LILA_BUILD_INFO_DIR} SKIP_UNITY_BUILD_INCLUSION ON SKIP_PRECOMPILE_HEADERS ON)
add_dependencies(lila_core lila_version)
# windows.h stays out of the unity batch.
set_source_files_properties(SystemMemory.cpp PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
if(LILA_TRACY)
    target_link_libraries(lila_core PUBLIC Tracy::TracyClient)
    target_compile_definitions(lila_core PUBLIC LILA_TRACY)
endif()
target_compile_definitions(lila_core PUBLIC
    GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_ENABLE_EXPERIMENTAL
    LILA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
