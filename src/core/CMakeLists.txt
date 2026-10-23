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
    ExtensionContext.cpp
    ExtensionManifest.cpp
    Log.cpp
    MainQueue.cpp
    Paths.cpp
    PrefStore.cpp
    Progress.cpp
    Registry.cpp
    ResourceKey.cpp
    TextMatch.cpp
    Workspace.cpp)

target_include_directories(lila_core PUBLIC ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/sdk/include)
target_link_libraries(lila_core PUBLIC nlohmann_json::nlohmann_json TBB::tbb glm::glm ${LILA_CRASHKIT_LIBS})
set_source_files_properties(CrashHandler.cpp PROPERTIES COMPILE_DEFINITIONS LILA_VERSION="${PROJECT_VERSION}")
if(LILA_TRACY)
    target_link_libraries(lila_core PUBLIC Tracy::TracyClient)
    target_compile_definitions(lila_core PUBLIC LILA_TRACY)
endif()
target_compile_definitions(lila_core PUBLIC
    GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_ENABLE_EXPERIMENTAL
    LILA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
