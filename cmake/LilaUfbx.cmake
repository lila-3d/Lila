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
        # Swapped rather than appended: /W4 from the top level next to /W0 makes cl warn D9025.
        get_target_property(ufbx_options lila_ufbx COMPILE_OPTIONS)
        list(REMOVE_ITEM ufbx_options /W4)
        set_target_properties(lila_ufbx PROPERTIES COMPILE_OPTIONS "${ufbx_options};/W0")
    endif()
endif()
