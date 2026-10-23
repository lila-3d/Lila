nanobind_build_library(nanobind-static)

lila_module(lila_python Scene
    Bindings.cpp
    MathBindings.cpp
    SceneBindings.cpp
    ExtensionBindings.cpp
    RegistryBindings.cpp
    Python.cpp)

target_link_libraries(lila_python PUBLIC lila_scene lila_math lila_vcolor PRIVATE nanobind-static Python::Python)
nanobind_compile_options(lila_python)
nanobind_link_options(lila_python)

cmake_path(GET Python_EXECUTABLE PARENT_PATH LILA_PYTHON_HOME)
target_compile_definitions(lila_python PRIVATE LILA_PYTHON_HOME="${LILA_PYTHON_HOME}")
