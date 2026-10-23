#pragma once

/**
 * Symbol visibility for Lila's three shared libraries. Each library's sources are compiled with its LILA_<LIB>_EXPORTS
 * defined; everything else (the hosts, other libraries, tests and extensions) imports.
 *
 * LILA_CRASHKIT_STATIC builds the crash helpers' own copy of the crash report code (lilacrash, lilawer), which must not
 * depend on Lila's libraries.
 */

#if defined(_WIN32)
#define LILA_DLL_EXPORT __declspec(dllexport)
#define LILA_DLL_IMPORT __declspec(dllimport)
#else
#define LILA_DLL_EXPORT __attribute__((visibility("default")))
#define LILA_DLL_IMPORT __attribute__((visibility("default")))
#endif

#if defined(LILA_FOUNDATION_EXPORTS)
#define LILA_FOUNDATION_API LILA_DLL_EXPORT
#else
#define LILA_FOUNDATION_API LILA_DLL_IMPORT
#endif

#if defined(LILA_SCENE_EXPORTS)
#define LILA_SCENE_API LILA_DLL_EXPORT
#else
#define LILA_SCENE_API LILA_DLL_IMPORT
#endif

#if defined(LILA_UI_EXPORTS)
#define LILA_UI_API LILA_DLL_EXPORT
#else
#define LILA_UI_API LILA_DLL_IMPORT
#endif

#if defined(LILA_CRASHKIT_STATIC)
#define LILA_CRASHKIT_API
#else
#define LILA_CRASHKIT_API LILA_FOUNDATION_API
#endif
