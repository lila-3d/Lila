# Sample viewport renderer, built as <build>/bin/<config>/extensions/viewportRenderer.lilaext, which Lila searches.
lila_add_extension(lila_viewport_renderer_extension ID viewportRenderer MANIFEST extension.json UI SOURCES ViewportRenderer.cpp)
