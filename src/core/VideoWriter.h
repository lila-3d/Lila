#pragma once

#include "core/Export.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace lila::video
{
namespace fs = std::filesystem;

struct Options
{
    int width = 1920;
    int height = 1080;
    double fps = 24.0;
    /** "h264" or "hevc"; both in an .mp4. */
    std::string codec = "h264";
    /** Bits per second; 0 picks one from the size, rate and quality. */
    int64_t bitrate = 0;
    /** 0..1, used when bitrate is 0: 0.5 is a good web upload, 1 near lossless. */
    double quality = 0.6;
};

/**
 * Encodes frames into an MP4 with the system's encoder (Media Foundation on Windows: hardware where the GPU has it).
 * Odd sizes are cropped by a pixel, as the codecs need even ones.
 */
class LILA_FOUNDATION_API Writer
{
public:
    /** Null with err set when the platform has no encoder or the file can't be made. */
    static std::unique_ptr<Writer> open(const fs::path& path, const Options& options, std::string* err = nullptr);
    virtual ~Writer() = default;
    /** One frame, 8-bit BGRA, top row first, width * 4 bytes per row. */
    virtual bool add(const uint8_t* bgra, std::string* err = nullptr) = 0;
    /** Writes the file's index; the writer takes no frames after. */
    virtual bool finish(std::string* err = nullptr) = 0;
};

/** True when this build can encode video at all. */
LILA_FOUNDATION_API bool available();
} // namespace lila::video
