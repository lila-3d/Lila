lila_module(lila_curve Scene
    EdgeCurve.cpp
    EdgeCurve.h
    Fit.cpp
    Fit.h
    Internal.cpp
    Internal.h
    Ops.cpp
    Ops.h
    Pipe.cpp
    Pipe.h
    Spline.cpp
    Spline.h
    Sweep.cpp
    Sweep.h
    Tessellate.cpp
    Tessellate.h)
target_link_libraries(lila_curve PUBLIC lila_geo)

lila_module(lila_curve_scene Scene CurveCommands.cpp CurveNodes.cpp CurveNodes.h EdgeToCurve.cpp EdgeToCurve.h PipeNode.cpp PipeNode.h)
target_link_libraries(lila_curve_scene PUBLIC lila_curve lila_scene)
