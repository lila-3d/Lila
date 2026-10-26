# OpenUSD, built as <bin>/<config>/extensions/OpenUSD.lilaext next to lila.exe, which loads it by default. The extension
# itself (OpenUSD) only registers the commands and translator; OpenUSDCore links Pixar's libraries and is loaded on the
# first USD command, so startup never loads them. Their DLLs and plugInfo trees are deployed inside the package, so
# neither the core exe nor the environment needs anything.
find_package(pxr CONFIG REQUIRED)
lila_add_extension(lila_openusd_extension ID OpenUSD MANIFEST extension.json UI SOURCES OpenUsd.cpp OpenUsdUi.cpp OpenUsd.h UsdBackend.h)

if(CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(_usd_pkg ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/extensions/OpenUSD.lilaext)
else()
    set(_usd_pkg ${CMAKE_BINARY_DIR}/$<CONFIG>/extensions/OpenUSD.lilaext)
endif()
set(_usd_core_sources UsdBackend.cpp UsdCommon.cpp UsdImport.cpp UsdImportSkel.cpp UsdExport.cpp UsdMaterials.cpp UsdStage.cpp
    UsdBackend.h UsdCommon.h UsdImport.h UsdExport.h UsdMaterials.h UsdStage.h)
# pxr's Python bindings (the pxr_boost python target), when the USD build has them: lila.usd hands scripts the live
# stages through them.
if(TARGET python)
    list(APPEND _usd_core_sources UsdPython.cpp)
    set_property(GLOBAL PROPERTY LILA_USD_PYTHON ON)
endif()
add_library(lila_openusd_core SHARED ${_usd_core_sources})
target_link_libraries(lila_openusd_core PRIVATE LilaScene usd usdGeom usdShade usdSkel usdLux usdUtils)
if(TARGET python)
    target_link_libraries(lila_openusd_core PRIVATE python)
    target_compile_definitions(lila_openusd_core PRIVATE LILA_USD_PYTHON)
endif()
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
# Beyond the system they need TBB and Python, which lila and lilabatch already carry, MaterialX (Storm and the
# MaterialX schemas) and the Vulkan loader, which comes with the driver.
file(GLOB _pxr_dlls_release ${_pxr_root}/bin/usd_*.dll ${_pxr_root}/bin/MaterialX*.dll)
file(GLOB _pxr_dlls_debug ${_pxr_root}/debug/bin/usd_*.dll ${_pxr_root}/debug/bin/MaterialX*.dll)
add_custom_command(TARGET lila_openusd_core POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<IF:$<CONFIG:Debug>,${_pxr_dlls_debug},${_pxr_dlls_release}>" ${_pkg}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${_pxr_root}${_pxr_cfg}/bin/usd ${_pkg}/usd
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_pkg}/plugin/usd
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${_pxr_root}${_pxr_cfg}/plugin/usd ${_pkg}/plugin/usd
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_pxr_root}/share/usd/copyright ${_pkg}/OpenUSD-LICENSE.txt
    COMMENT "Deploying OpenUSD into OpenUSD.lilaext"
    COMMAND_EXPAND_LISTS)
if(TARGET python)
    # <pkg>/python/pxr: the bindings, which load the libraries above from beside OpenUSDCore.
    add_custom_command(TARGET lila_openusd_core POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${_pxr_root}${_pxr_cfg}/lib/python ${_pkg}/python
        COMMENT "Deploying OpenUSD's Python bindings into OpenUSD.lilaext")
endif()

foreach(_host lila lilabatch)
    if(TARGET ${_host})
        add_dependencies(${_host} lila_openusd_extension)
    endif()
endforeach()
