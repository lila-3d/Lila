# lila_physics: the rigid-body scene on Jolt that physics extensions link statically. Jolt (static, cross-platform
# deterministic) comes with it, so an extension linking this needs nothing else.
find_package(Jolt CONFIG REQUIRED)
find_package(TBB CONFIG REQUIRED)
add_library(lila_physics STATIC RigidScene.cpp RigidScene.h Decompose.cpp Decompose.h)
target_include_directories(lila_physics PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lila_physics PUBLIC Jolt::Jolt glm::glm PRIVATE TBB::tbb)
set_target_properties(lila_physics PROPERTIES POSITION_INDEPENDENT_CODE ON)
