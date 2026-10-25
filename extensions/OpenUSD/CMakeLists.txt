# OpenUSD, built as <bin>/<config>/extensions/OpenUSD.lilaext next to lila.exe, which loads it by default. The extension
# itself (OpenUSD) only registers the commands and translator; OpenUSDCore links Pixar's libraries and is loaded on the
# first USD command, so startup never loads them. Their DLLs and plugInfo trees are deployed inside the package, so
# neither the core exe nor the environment needs anything.
find_package(pxr CONFIG REQUIRED)
lila_add_extension(lila_openusd_extension ID OpenUSD MANIFEST extension.json SOURCES OpenUsd.cpp UsdBackend.h)

if(CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(_usd_pkg ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/extensions/OpenUSD.lilaext)
else()
    set(_usd_pkg ${CMAKE_BINARY_DIR}/$<CONFIG>/extensions/OpenUSD.lilaext)
endif()
set(_usd_core_sources UsdBackend.cpp UsdCommon.cpp UsdImport.cpp UsdImportSkel.cpp UsdExport.cpp UsdMaterials.cpp
    UsdBackend.h UsdCommon.h UsdImport.h UsdExport.h UsdMaterials.h)
add_library(lila_openusd_core SHARED ${_usd_core_sources})
target_link_libraries(lila_openusd_core PRIVATE LilaScene usd usdGeom usdShade usdSkel usdLux usdUtils)
set_target_properties(lila_openusd_core PROPERTIES
    OUTPUT_NAME OpenUSDCore
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${_usd_pkg}
    LIBRARY_OUTPUT_DIRECTORY ${_usd_pkg})
lila_check_extension_sources(lila_openusd_core ${_usd_core_sources})
add_dependencies(lila_openusd_extension lila_openusd_core)
if(MSVC)
    # Pixar's headers trip /W4 (conversions, unreferenced parameters) and use the deprecated std iterator base.
    target_compile_options(lila_openusd_core PRIVATE /wd4244 /wd4267 /wd4100 /wd4127 /wd4305 /wd4996 /bigobj)
    target_compile_definitions(lila_openusd_core PRIVATE _SILENCE_CXX17_ITERATOR_BASE_CLASS_DEPRECATION_WARNING)
endif()

# <pkg>/*.dll, <pkg>/usd (the top-level plugInfo.json and each library's resources) and <pkg>/plugin/usd (module plugins):
# the relative layout Pixar's plugInfo files expect, kept inside the package.
cmake_path(GET pxr_DIR PARENT_PATH _pxr_share)
cmake_path(GET _pxr_share PARENT_PATH _pxr_root)
set(_pkg ${_usd_pkg})
set(_pxr_cfg $<$<CONFIG:Debug>:/debug>)
# Every USD library, not just the linked ones: plugInfo names them all, and a stage using one of their schemas loads it.
# They depend on nothing outside the system but TBB, which lila and lilabatch already carry.
file(GLOB _pxr_dlls_release ${_pxr_root}/bin/usd_*.dll)
file(GLOB _pxr_dlls_debug ${_pxr_root}/debug/bin/usd_*.dll)
add_custom_command(TARGET lila_openusd_core POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<IF:$<CONFIG:Debug>,${_pxr_dlls_debug},${_pxr_dlls_release}>" ${_pkg}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${_pxr_root}${_pxr_cfg}/bin/usd ${_pkg}/usd
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_pkg}/plugin/usd
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${_pxr_root}${_pxr_cfg}/plugin/usd ${_pkg}/plugin/usd
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_pxr_root}/share/usd/copyright ${_pkg}/OpenUSD-LICENSE.txt
    COMMENT "Deploying OpenUSD into OpenUSD.lilaext"
    COMMAND_EXPAND_LISTS)

foreach(_host lila lilabatch)
    if(TARGET ${_host})
        add_dependencies(${_host} lila_openusd_extension)
    endif()
endforeach()
