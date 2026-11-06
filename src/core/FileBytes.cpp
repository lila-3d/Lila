#include "core/FileBytes.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace lila
{
UninitVector<char> readFileBytes(const std::filesystem::path& path)
{
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec)
        throw std::runtime_error("can't open " + path.string());
    UninitVector<char> bytes(static_cast<size_t>(size));
    // Each slice stays under the 2 GB a single MSVC stream read stops at.
    constexpr size_t kSlice = size_t(32) << 20;
    std::atomic<bool> failed = false;
    tbb::parallel_for(size_t(0), (bytes.size() + kSlice - 1) / kSlice, [&](size_t k) {
        std::ifstream in(path, std::ios::binary);
        const size_t at = k * kSlice;
        if (!in || !in.seekg(std::streamoff(at)) || !in.read(bytes.data() + at, std::streamsize(std::min(kSlice, bytes.size() - at))))
            failed = true;
    });
    if (failed)
        throw std::runtime_error("can't read " + path.string());
    return bytes;
}

void writeFileBytes(const std::filesystem::path& path, std::string_view bytes)
{
    constexpr size_t kSlice = size_t(32) << 20;
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out || !out.write(bytes.data(), std::streamsize(std::min(kSlice, bytes.size()))))
            throw std::runtime_error("can't write " + path.string());
    }
    std::atomic<bool> failed = false;
    tbb::parallel_for(size_t(1), (bytes.size() + kSlice - 1) / kSlice, [&](size_t k) {
        std::ofstream out(path, std::ios::binary | std::ios::in | std::ios::out);
        const size_t at = k * kSlice;
        if (!out || !out.seekp(std::streamoff(at)) || !out.write(bytes.data() + at, std::streamsize(std::min(kSlice, bytes.size() - at))))
            failed = true;
    });
    if (failed)
        throw std::runtime_error("can't write " + path.string());
}

std::error_code renameRetrying(const std::filesystem::path& from, const std::filesystem::path& to)
{
    using namespace std::chrono;
    const auto giveUp = steady_clock::now() + seconds(2);
    auto wait = milliseconds(5);
    std::error_code ec;
    for (;;)
    {
        std::filesystem::rename(from, to, ec);
#ifdef _WIN32
        // ERROR_ACCESS_DENIED and ERROR_SHARING_VIOLATION: someone else has a handle open.
        const bool held = ec.value() == 5 || ec.value() == 32;
#else
        const bool held = false;
#endif
        if (!ec || !held || steady_clock::now() >= giveUp)
            return ec;
        std::this_thread::sleep_for(wait);
        wait = std::min(wait * 2, milliseconds(100));
    }
}
} // namespace lila
