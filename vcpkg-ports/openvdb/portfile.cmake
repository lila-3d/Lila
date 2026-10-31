vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO AcademySoftwareFoundation/openvdb
    REF "v${VERSION}"
    SHA512 67b859bf77c53e68116faa7915bb6a5a50a8cff10435762890e13348625e8aebdb6661b722017632471648afe31e2f9d4cd2e18456c728192bfd0accd70a40ef
    PATCHES
        fix_cmake.patch
)

file(REMOVE "${SOURCE_PATH}/cmake/FindTBB.cmake")
file(REMOVE "${SOURCE_PATH}/cmake/FindIlmBase.cmake")
file(REMOVE "${SOURCE_PATH}/cmake/FindBlosc.cmake")
file(REMOVE "${SOURCE_PATH}/cmake/FindOpenEXR.cmake")

string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "static" OPENVDB_STATIC)
string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "dynamic" OPENVDB_SHARED)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DOPENVDB_BUILD_UNITTESTS=OFF
        -DOPENVDB_BUILD_PYTHON_MODULE=OFF
        -DOPENVDB_3_ABI_COMPATIBLE=OFF
        # Boost (61 ports) only serves delayed loading, OpenEXR only vdb_render; half floats use OpenVDB's own.
        -DOPENVDB_USE_DELAYED_LOADING=OFF
        -DUSE_EXR=OFF
        -DUSE_IMATH_HALF=OFF
        -DOPENVDB_CORE_STATIC=${OPENVDB_STATIC}
        -DOPENVDB_CORE_SHARED=${OPENVDB_SHARED}
        -DOPENVDB_BUILD_VDB_PRINT=OFF
        -DOPENVDB_BUILD_VDB_VIEW=OFF
        -DOPENVDB_BUILD_VDB_RENDER=OFF
        -DOPENVDB_BUILD_VDB_LOD=OFF
        -DOPENVDB_BUILD_AX=OFF
        -DOPENVDB_BUILD_NANOVDB=OFF
        -DUSE_PKGCONFIG=OFF
        -DUSE_EXPLICIT_INSTANTIATION=OFF
    MAYBE_UNUSED_VARIABLES
        OPENVDB_3_ABI_COMPATIBLE
        OPENVDB_BUILD_TOOLS
        NANOVDB_BUILD_TOOLS
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/OpenVDB)

vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")

configure_file("${CMAKE_CURRENT_LIST_DIR}/vcpkg-cmake-wrapper.cmake.in" "${CURRENT_PACKAGES_DIR}/share/${PORT}/vcpkg-cmake-wrapper.cmake" @ONLY)
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
