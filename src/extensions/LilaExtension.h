#pragma once

/**
 * Lila C++ extension SDK.
 *
 * An extension is a shared library exporting:
 *
 *     LILA_EXTENSION_EXPORT int lilaExtensionInit(LilaExtensionContext* ctx);    // 0 = loaded
 *     LILA_EXTENSION_EXPORT void lilaExtensionUninit(LilaExtensionContext* ctx); // optional
 *
 * Register everything through ctx so it is tagged with the extension's name and removed on unload. The registries live
 * in the Lila executable, so linking Lila's libraries into an extension gives it private, unused copies: only include
 * these headers. Build with the same compiler and runtime as Lila, since C++ types cross the boundary.
 */

#include "cmds/Command.h"
#include "core/Log.h"
#include "scene/Node.h"

#include <cstddef>

#define LILA_EXTENSION_API_VERSION 1

#ifdef _WIN32
#define LILA_EXTENSION_EXPORT extern "C" __declspec(dllexport)
#else
#define LILA_EXTENSION_EXPORT extern "C" __attribute__((visibility("default")))
#endif

struct LilaExtensionContext
{
    int apiVersion = LILA_EXTENSION_API_VERSION;
    size_t structSize = sizeof(LilaExtensionContext);
    /** Owner tag for everything registered through this context. */
    const char* extensionName = nullptr;
    const char* extensionPath = nullptr;
    /** The extension's own, untouched by Lila. */
    void* userData = nullptr;

    /** Takes def (its owner is set to the extension). False, with a log message, if the name is taken. */
    bool (*registerCommand)(LilaExtensionContext* ctx, lila::CommandDef* def) = nullptr;
    void (*unregisterCommand)(LilaExtensionContext* ctx, const char* name) = nullptr;
    /** Takes type (its owner is set to the extension). Null, with a log message, if the name is taken. */
    const lila::scene::NodeType* (*registerType)(LilaExtensionContext* ctx, lila::scene::NodeType* type) = nullptr;
    /** Built-in or extension type to derive from, e.g. "transform". */
    const lila::scene::NodeType* (*findType)(LilaExtensionContext* ctx, const char* name) = nullptr;
    /** Runs Python in the script editor's namespace. False when it raised or Python is not running. */
    bool (*runPython)(LilaExtensionContext* ctx, const char* code) = nullptr;
    /** level is a lila::LogLevel. */
    void (*log)(LilaExtensionContext* ctx, int level, const char* text) = nullptr;

    void* host = nullptr; // Lila's
};

using LilaExtensionInitFn = int (*)(LilaExtensionContext*);
using LilaExtensionUninitFn = void (*)(LilaExtensionContext*);

/** Extensions call this first and fail init when it is false. */
inline bool lilaExtensionCompatible(const LilaExtensionContext* ctx)
{
    return ctx && ctx->apiVersion == LILA_EXTENSION_API_VERSION && ctx->structSize >= sizeof(LilaExtensionContext);
}
