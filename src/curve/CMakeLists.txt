lila_module(lila_curve Scene
    Along.cpp
    Along.h
    BevelPlus.cpp
    BevelPlus.h
    Blend.cpp
    Blend.h
    BorderBlend.cpp
    BorderBlend.h
    Cable.cpp
    Cable.h
    CapFill.cpp
    CapFill.h
    Contour.cpp
    Contour.h
    CurveBoolean.cpp
    CurveBoolean.h
    CurveDeform.cpp
    CurveDeform.h
    CurveMath.h
    Dxf.cpp
    Dxf.h
    EdgeCurve.cpp
    EdgeCurve.h
    Fair.cpp
    Fair.h
    Fit.cpp
    Fit.h
    Groom.cpp
    Groom.h
    HairCards.cpp
    HairCards.h
    Internal.cpp
    Internal.h
    Intersect.cpp
    Intersect.h
    Network.cpp
    Network.h
    Ops.cpp
    Ops.h
    Pipe.cpp
    Pipe.h
    PipeRoute.cpp
    PipeRoute.h
    PipeSupport.cpp
    PipeSupport.h
    Channel.cpp
    Channel.h
    WorldMeshes.cpp
    WorldMeshes.h
    Planar.cpp
    Planar.h
    Roof.cpp
    Roof.h
    Skeleton.cpp
    Skeleton.h
    Sketch.cpp
    Sketch.h
    Spline.cpp
    Spline.h
    Strap.cpp
    Strap.h
    SurfaceConform.cpp
    SurfaceConform.h
    Svg.cpp
    Svg.h
    Sweep.cpp
    Sweep.h
    Tessellate.cpp
    Tessellate.h
    Text.cpp
    Text.h)
target_link_libraries(lila_curve PUBLIC lila_geo)
# Clipper2 built here with Z values (vertex ids traced through clips); the vcpkg build targets a newer toolset's STL.
lila_quiet_module(lila_clipper2 Scene third_party/clipper2/clipper.engine.cpp third_party/clipper2/clipper.offset.cpp
    third_party/clipper2/clipper.rectclip.cpp)
foreach(_t lila_curve lila_clipper2)
    target_include_directories(${_t} BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/clipper2/include)
    target_compile_definitions(${_t} PRIVATE USINGZ CLIPPER2_MAX_DECIMAL_PRECISION=8)
endforeach()

lila_module(lila_curve_scene Scene AlongNodes.cpp AlongNodes.h CurveCommands.cpp CurveControls.cpp CurveControls.h CurveDeformCommands.cpp CurveNodes.cpp CurveNodes.h DxfCommands.cpp EdgeToCurve.cpp EdgeToCurve.h PipeNode.cpp PipeNode.h
    StripUvAttrs.cpp StripUvAttrs.h Carve.cpp Carve.h PipeToolsNodes.cpp PipeToolsNodes.h
    CableNode.cpp ContourNode.cpp StrapNode.cpp StrapNode.h NodeHelpers.h EnvironmentNodes.h EzCurve.cpp LiveSurface.cpp
    VehicleCommands.cpp VehicleNodes.cpp VehicleNodes.h
    SketchCommands.cpp SketchNode.h GroomCommands.cpp GroomNode.h
    PlanarNodes.cpp PlanarNodes.h TurntableBase.cpp TurntableBase.h)
target_link_libraries(lila_curve_scene PUBLIC lila_curve lila_scene lila_brush lila_io)
