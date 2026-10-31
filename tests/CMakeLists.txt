add_executable(lila_tests
    main.cpp
    test_graph.cpp
    test_prefs.cpp
    test_formats.cpp
    test_references.cpp
    test_meshes.cpp
    test_modeling.cpp
    test_normal_frames.cpp
    test_orient_pivot.cpp
    test_knife.cpp
    test_step_history.cpp
    test_command_history.cpp
    test_conform.cpp
    test_modifiers.cpp
    test_array_modifier.cpp
    test_draw_batches.cpp
    test_modstack.cpp
    test_instancing.cpp
    test_duplicate.cpp
    test_user_attrs.cpp
    test_array_group.cpp
    test_docs.cpp
    test_dds.cpp
    test_samples.cpp
    test_user_docs.cpp
    test_transfer.cpp
    test_boolean.cpp
    test_fracture.cpp
    test_gate.cpp
    test_autobevel.cpp
    test_measure.cpp
    test_debug_draw.cpp
    test_surface_snap.cpp
    test_geometry_snap.cpp
    test_imageplane.cpp
    test_hotkeys.cpp
    test_display.cpp
    test_display_aids.cpp
    test_icon_bars.cpp
    test_preselect.cpp
    test_layouts.cpp
    test_themes.cpp
    test_telemetry.cpp
    test_toolkit.cpp
    test_modeling_defaults.cpp
    test_symmetry.cpp
    test_radial_symmetry.cpp
    test_softselect.cpp
    test_gizmo.cpp
    test_history.cpp
    test_crash.cpp
    test_component_query.cpp
    test_outliner.cpp
    test_outliner_filter.cpp
    test_value_reset.cpp
    test_progress.cpp
    test_scene_scripts.cpp
    test_camera_history.cpp
    test_options.cpp
    test_extrude.cpp
    test_hud.cpp
    test_backdrop.cpp
    test_tearoff.cpp
    test_visibility.cpp
    test_display_layers.cpp
    test_object_sets.cpp
    test_create_select.cpp
    test_primitive_axis.cpp
    test_camera.cpp
    test_light.cpp
    test_showcase.cpp
    test_locks.cpp
    test_paths.cpp
    test_project_textures.cpp
    test_text_match.cpp
    test_ui_registries.cpp
    test_graph_backends.cpp
    test_viewport_ext.cpp
    test_select_paths.cpp
    test_isolation.cpp
    test_diagnostics.cpp
    test_smart_join.cpp
    test_batch_rename.cpp
    test_change_stream.cpp
    test_batch_run.cpp
    test_node_paths.cpp
    test_select_strokes.cpp
    test_render_textures.cpp)
target_link_libraries(lila_tests PRIVATE LilaUI doctest::doctest)
find_package(miniz CONFIG REQUIRED)
target_link_libraries(lila_tests PRIVATE miniz::miniz)
# The batch run tests start real lilabatch workers.
add_dependencies(lila_tests lilabatch)
add_test(NAME lila_tests COMMAND lila_tests)

if(WIN32)
    # Real crashes of a helper process, checked through the watchdog's report; kept out of lila_tests for speed.
    add_executable(lila_crashtest crash_helper.cpp)
    target_link_libraries(lila_crashtest PRIVATE LilaFoundation)
    add_executable(lila_crash_tests test_crash_watch.cpp)
    target_link_libraries(lila_crash_tests PRIVATE LilaFoundation doctest::doctest)
    add_dependencies(lila_crash_tests lila_crashtest lilacrash lilawer)
    add_test(NAME lila_crash_tests COMMAND lila_crash_tests)
endif()

add_subdirectory(math)
add_subdirectory(geo)
add_subdirectory(curve)
add_subdirectory(vcs)
add_subdirectory(editor)
add_subdirectory(docks)
add_subdirectory(extensions)
add_subdirectory(terminal)
add_subdirectory(hk)
add_subdirectory(link)
add_subdirectory(fbx)
add_subdirectory(gltf)
add_subdirectory(seq)
add_subdirectory(bake)
add_subdirectory(tex)
add_subdirectory(anim)
add_subdirectory(sim)
if(TARGET lila_puppeteer_extension)
    add_subdirectory(puppeteer_anim)
    add_subdirectory(puppeteer)
endif()
add_subdirectory(uv)
add_subdirectory(rig)
add_subdirectory(rigtemplates)
add_subdirectory(gn)
if(LILA_VOLUMES)
    add_subdirectory(nanovdb)
endif()
add_subdirectory(retopo)
add_subdirectory(clay)
add_subdirectory(sculpt)
add_subdirectory(vcolor)
add_subdirectory(brush)
add_subdirectory(deform)
add_subdirectory(materials)
add_subdirectory(stylised)
add_subdirectory(viewport_renderers)
add_subdirectory(fbxskin)
add_subdirectory(maya)
add_subdirectory(agentic)
add_subdirectory(foliage)
add_subdirectory(masonry)
add_subdirectory(trim)
add_subdirectory(stagehand)
add_subdirectory(settlement)
add_subdirectory(groundtiles)
add_subdirectory(terrain)
if(LILA_USD)
    add_subdirectory(usd)
endif()
