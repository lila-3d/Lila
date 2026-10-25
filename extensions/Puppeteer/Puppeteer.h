#pragma once

#include <lila/Extension.h>

/**
 * Puppeteer: character animation done on the joints themselves. Each subsystem registers through its own function so
 * the entry point (Puppeteer.cpp) reads as the list of what the extension adds.
 */
namespace puppeteer
{
inline constexpr const char* kCharacterType = "puppeteerCharacter";
// Rig elements are base Lila's; Puppeteer gives them its UI.
inline constexpr const char* kIkChainType = "ikChain";
inline constexpr const char* kReverseFootType = "reverseFoot";
inline constexpr const char* kLookerType = "looker";
// Older Puppeteer scenes' types, loaded only to be migrated.
inline constexpr const char* kLegacyIkChainType = "puppeteerIkChain";
inline constexpr const char* kLegacyReverseFootType = "puppeteerReverseFoot";
inline constexpr const char* kLegacyAlignType = "puppeteerAlign";
inline constexpr const char* kLegacyLookerType = "puppeteerLooker";
inline constexpr const char* kSpaceType = "puppeteerSpace";

inline constexpr const char* kRigMode = "puppeteerRig";
inline constexpr const char* kAnimateMode = "puppeteerAnimate";
inline constexpr const char* kSpinnerGizmo = "puppeteer.spinner";
inline constexpr const char* kRigSelection = "puppeteer.rig";
inline constexpr const char* kAnimateSelection = "puppeteer.animate";

/** Prefs file the extension's settings live in. */
inline constexpr const char* kPrefs = "puppeteer";

/** The character node type and its commands. */
bool registerCharacter(lila::ext::Context& ctx);
/** Puppeteer's rig element commands over base Lila's, and the migration of older scenes' elements. */
bool registerRigElements(lila::ext::Context& ctx);
/** IK/reverse foot controller commands: pole, roll/twist/bank, foot lock. */
void registerControllers(lila::ext::Context& ctx);
/** How reverse foot controllers look and pick in the viewport. */
void registerControllerDrawing(lila::ext::Context& ctx);
/** Pins for full-body posing. */
void registerFullBody(lila::ext::Context& ctx);
/** Joint tools: commands and actions. */
void registerJointTools(lila::ext::Context& ctx);
/** The Spinner's commands (rotate with inherit, toon scale, aim moves, breakdown), usable without the UI. */
void registerSpinnerOps(lila::ext::Context& ctx);
/** The Spinner gizmo provider, its overlay icons and actions. */
void registerSpinner(lila::ext::Context& ctx);
/** Rig and Animate modes: layouts, hotkey layers, selection handlers, menus, the graph editor swap. */
void registerModes(lila::ext::Context& ctx);
/** Animate-mode panels (anim/). */
void registerAnimPanels(lila::ext::Context& ctx);
} // namespace puppeteer
