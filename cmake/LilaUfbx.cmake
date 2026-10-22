include(FetchContent)

# ufbx (MIT) is a single C file; build it ourselves rather than through its repo's CMake.
FetchContent_Declare(ufbx
    GIT_REPOSITORY https://github.com/ufbx/ufbx.git
    GIT_TAG v0.23.1
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR no-cmake)
FetchContent_MakeAvailable(ufbx)

if(NOT TARGET lila_ufbx)
    add_library(lila_ufbx STATIC ${ufbx_SOURCE_DIR}/ufbx.c)
    target_include_directories(lila_ufbx SYSTEM PUBLIC ${ufbx_SOURCE_DIR})
    if(MSVC)
        target_compile_options(lila_ufbx PRIVATE /W0)
    endif()
endif()
