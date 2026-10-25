#pragma once

/**
 * Scene interchange for file format extensions: the format-neutral export snapshot (fbx::gatherExport, ExportScene),
 * the axis profiles and conversions the FBX translator uses, cameras, skins, LOD groups and scene curves, and LILA_ZONE
 * for profiling heavy readers and writers.
 */
#include "core/Profile.h"
#include "curve/CurveNodes.h"
#include "fbx/Fbx.h"
#include "fbx/FbxAxes.h"
#include "geo/Primitives.h"
#include "scene/Camera.h"
#include "scene/LodGroup.h"
#include "scene/Skin.h"
