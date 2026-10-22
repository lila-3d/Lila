#include "extensions/Library.h"

#include "core/Profile.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace lila::extensions
{
std::unique_ptr<Library> Library::open(const std::filesystem::path& path, std::string* error)
{
    LILA_ZONE();

    std::unique_ptr<Library> lib(new Library());
#ifdef _WIN32
    // Lets the extension's own dependencies load from its folder.
    lib->mHandle = LoadLibraryExW(path.wstring().c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!lib->mHandle)
    {
        DWORD code = GetLastError();
        if (error)
        {
            char* msg = nullptr;
            FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                           reinterpret_cast<char*>(&msg), 0, nullptr);
            *error = msg ? msg : "error " + std::to_string(code);
            while (!error->empty() && (error->back() == '\n' || error->back() == '\r'))
                error->pop_back();
            if (msg)
                LocalFree(msg);
        }
        return nullptr;
    }
#else
    lib->mHandle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib->mHandle)
    {
        if (error)
            *error = dlerror();
        return nullptr;
    }
#endif
    return lib;
}

Library::~Library()
{
    if (!mHandle || mLeak)
        return;
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(mHandle));
#else
    dlclose(mHandle);
#endif
}

void* Library::symbol(const char* name) const
{
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(mHandle), name));
#else
    return dlsym(mHandle, name);
#endif
}

const char* Library::extension()
{
#if defined(_WIN32)
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}
} // namespace lila::extensions
