lila_module(lila_rigmath Scene
    Builder.h
    Easing.h
    MathMatrix.cpp
    MathMisc.cpp
    MathNodes.cpp
    MathNodes.h
    MathRamp.cpp
    MathRotation.cpp
    MathScalar.cpp
    MathVector.cpp)
target_link_libraries(lila_rigmath PUBLIC lila_scene lila_gn)
