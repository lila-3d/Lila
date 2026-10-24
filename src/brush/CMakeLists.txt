lila_module(lila_brush Scene
    Brush.cpp
    Brush.h
    Stroke.cpp
    Stroke.h
    Surface.cpp
    Surface.h
    Symmetry.cpp
    Symmetry.h)
target_link_libraries(lila_brush PUBLIC lila_scene lila_geo)

# The Qt side: tablet pressure, held-key adjustments, the cursor and the panel options group.
lila_module(lila_brush_ui UI BrushUi.cpp BrushUi.h)
target_link_libraries(lila_brush_ui PUBLIC lila_brush lila_scene lila_render Qt6::Widgets)
