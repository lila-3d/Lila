# Throwaway visual check: builds the hotkey and marking menu editors and grabs them to PNGs.
add_executable(lila_hk_demo EXCLUDE_FROM_ALL demo.cpp)
target_link_libraries(lila_hk_demo PRIVATE lila_ui)
