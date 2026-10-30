#pragma once

/**
 * Viewport renderers: an external renderer draws a viewport's image and Lila composites its overlays on top
 * (viewportRenderers::add(ctx, ...)). With the view's isolate select set (Request::view.isolation) and the theme's
 * colours, for renderers that draw what the viewport draws.
 */
#include "scene/Isolation.h"
#include "ui/Theme.h"
#include "ui/ViewportRenderers.h"
