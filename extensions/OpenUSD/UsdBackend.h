#pragma once

// The entry points OpenUSDCore exports and OpenUSD.dll looks up when the first USD command runs. Nothing here touches
// Pixar's headers, so the front loads without them. Errors come back as strings, never as exceptions across the boundary.

#include <lila/Command.h>

#include <nlohmann/json.hpp>

#include <string>

#if defined(_WIN32)
#define USD_BACKEND_EXPORT extern "C" __declspec(dllexport)
#else
#define USD_BACKEND_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace lila::usd
{
#if defined(_WIN32)
inline constexpr const char* kBackendFile = "OpenUSDCore.dll";
#elif defined(__APPLE__)
inline constexpr const char* kBackendFile = "OpenUSDCore.dylib";
#else
inline constexpr const char* kBackendFile = "OpenUSDCore.so";
#endif

/** The node type hosting a live stage, registered by the front so scenes holding one load without Pixar's libraries. */
inline constexpr const char* kStageType = "usdStage";

/** The usdStage node's attributes. */
namespace stageAttr
{
inline constexpr const char* kFile = "filePath";
inline constexpr const char* kEditLayer = "editLayer";
/** The edit layer's usda text, when it is kept inside the .lila. */
inline constexpr const char* kEditData = "editLayerData";
/**
 * [relative node path, prim path, kind, extra] per linked node. kind 0 (or none): the node's own prim, by link path;
 * 1: a node the prim owns (a joint, by its Skeleton prim); 2: a point instance copy, extra its prototype prim;
 * 3: a point instance's transform, extra its index on the instancer.
 */
inline constexpr const char* kMap = "primMap";
/** {prim path or $mat:slot or $skel:path: {group: hash}} as last written or read. */
inline constexpr const char* kBaseline = "baseline";
/** {slot: material prim path}: where each Lila material came from in the stage. */
inline constexpr const char* kMaterials = "materialMap";
/** [[prim path, rule]] payload load rules. */
inline constexpr const char* kLoadRules = "loadRules";
/** {file: [modified time, size]} of every file the stage read, at the last save. */
inline constexpr const char* kStamps = "sourceStamps";
/** The MaterialX surface new and edited materials are written with beside their UsdPreviewSurface: openPBR,
 * standardSurface or none; empty follows the stage, OpenPBR when any of its materials uses MaterialX. */
inline constexpr const char* kMaterialX = "materialX";
} // namespace stageAttr

/** Registers the package's plugInfo files. 0 on success. */
using BackendInit = int (*)(const char* packageRoot, std::string* error);
/** Runs a USD command by name (importUsd, exportUsd, the usdStage commands and events). 0 on success, with its return value in result. */
using BackendRun = int (*)(const char* command, const Args* args, nlohmann::json* result, std::string* error);
/**
 * lilaUsdStage: the pxr::UsdStage* a usdStage node composes (see stage::liveStage), owned by the node; null with the
 * error on failure.
 */
using BackendStage = void* (*)(const char* node, std::string* error);
/**
 * lilaUsdStagePython, for lila.usd through ctypes: the same stage as a pxr.Usd.Stage, a new reference to a weak handle
 * that expires with the node. Null with a Python exception set on failure. Called with the GIL held, after pxr.Usd is
 * imported. Built only when the pxr libraries have Python bindings.
 */
using BackendStagePython = void* (*)(const char* node);
/** lilaUsdLoad, exported by OpenUSD.dll for lila.usd: loads OpenUSDCore and registers the plugins. Null, or the error. */
using FrontLoad = const char* (*)();
} // namespace lila::usd
