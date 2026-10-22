# The crash report and capture code, without Lila's dependencies, for the lilacrash watchdog and the lilawer WER helper.
add_library(lila_crashkit STATIC CrashReport.cpp CrashCapture.cpp CrashWatch.cpp)
target_include_directories(lila_crashkit PUBLIC ${CMAKE_SOURCE_DIR}/src)
if(WIN32)
    target_link_libraries(lila_crashkit PUBLIC dbghelp version wevtapi shcore dxgi)
endif()

add_library(lila_core STATIC
    CrashHandler.cpp
    Log.cpp
    MainQueue.cpp
    Paths.cpp
    PrefStore.cpp
    Progress.cpp
    Workspace.cpp)

target_include_directories(lila_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(lila_core PUBLIC lila_crashkit nlohmann_json::nlohmann_json TBB::tbb glm::glm)
set_source_files_properties(CrashHandler.cpp PROPERTIES COMPILE_DEFINITIONS LILA_VERSION="${PROJECT_VERSION}")
if(LILA_TRACY)
    target_link_libraries(lila_core PUBLIC Tracy::TracyClient)
    target_compile_definitions(lila_core PUBLIC LILA_TRACY)
endif()
target_compile_definitions(lila_core PUBLIC
    GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_ENABLE_EXPERIMENTAL
    LILA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
