# VT parser and screen model for the terminal panel. Plain C++, so it can be tested without Qt.
add_library(lila_term STATIC
    VtScreen.cpp VtScreen.h
    VtInput.cpp VtInput.h)
target_include_directories(lila_term PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_features(lila_term PUBLIC cxx_std_20)
