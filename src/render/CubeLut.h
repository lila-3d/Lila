#pragma once

#include "core/Export.h"
#include "render/FrameState.h"

#include <memory>
#include <string>

namespace lila::render
{
/**
 * An Adobe/Resolve .cube 3D LUT, cached by path until the file changes. Null (with err) when it can't be read or holds
 * only a 1D table.
 */
LILA_UI_API std::shared_ptr<const Lut3D> loadCubeLut(const std::string& path, std::string* err = nullptr);
} // namespace lila::render
