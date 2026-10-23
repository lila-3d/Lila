# Sample viewport extension, built as <build>/bin/<config>/extensions/viewportPoints.lilaext, which Lila searches.
lila_add_extension(lila_viewport_points_extension ID viewportPoints MANIFEST extension.json RESOURCES resources UI SOURCES ViewportPoints.cpp)
