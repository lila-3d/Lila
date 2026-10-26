#pragma once

#include "core/Export.h"
#include "core/UninitVector.h"

#include <filesystem>

namespace lila
{
/** The whole file, read in slices on several threads at once. Throws std::runtime_error when it can't be read. */
LILA_FOUNDATION_API UninitVector<char> readFileBytes(const std::filesystem::path& path);

/** Writes bytes as the whole file, in slices on several threads at once. Throws std::runtime_error when it can't. */
LILA_FOUNDATION_API void writeFileBytes(const std::filesystem::path& path, std::string_view bytes);

/** Frees a big buffer on another thread: gigabytes take long enough to free to be noticed. */
template <class T> void freeInBackground(T&& big);
} // namespace lila

#include <thread>

template <class T> void lila::freeInBackground(T&& big)
{
    std::thread([b = std::forward<T>(big)] {}).detach();
}
