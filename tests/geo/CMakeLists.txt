add_executable(lila_geo_tests
    main.cpp
    test_bevel.cpp
    test_bounding.cpp
    test_bvh.cpp
    test_cross_section.cpp
    test_knife.cpp
    test_loop_tools.cpp
    test_mesh.cpp
    test_multibox.cpp
    test_normals.cpp
    test_obj.cpp
    test_ops.cpp
    test_primitives.cpp
    test_radial_symmetry.cpp
    test_stl_ply.cpp
    test_triangulate.cpp)
target_link_libraries(lila_geo_tests PRIVATE LilaScene doctest::doctest)
add_test(NAME lila_geo_tests COMMAND lila_geo_tests)
