#pragma once

#include "core/Export.h"

#include <filesystem>
#include <memory>
#include <string>

namespace lila::extensions
{
/** A loaded shared library: LoadLibrary / dlopen. */
class LILA_SCENE_API Library
{
public:
    static std::unique_ptr<Library> open(const std::filesystem::path& path, std::string* error);
    ~Library();

    void* symbol(const char* name) const;

    /** Keep the library mapped past this object, for shutdown when code from it may still run. */
    void leak() { mLeak = true; }

    /** .dll, .so or .dylib. */
    static const char* extension();

private:
    Library() = default;
    void* mHandle = nullptr;
    bool mLeak = false;
};
} // namespace lila::extensions
