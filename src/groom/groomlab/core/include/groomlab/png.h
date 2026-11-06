// SPDX-License-Identifier: Apache-2.0
#pragma once

// Compressed PNG for exports (engine textures, glTF images), with no library:
// each row takes the PNG filter (none, sub, up, average, Paeth) that leaves
// the smallest sum of magnitudes, then zlib deflate with LZ77 matches and
// fixed Huffman codes. Card atlases are mostly empty, so they shrink many
// times over the stored-block PNGs writePng (scalp_paint.h) writes for
// painted maps. Any PNG reader decodes them. Rows are filtered, and large
// images parsed, in parallel; the bytes are those of a serial encoder.

#include <cstdint>
#include <string>
#include <vector>

namespace groomlab {

// An 8-bit PNG's bytes: `channels` 1 (gray), 2 (gray + alpha), 3 (RGB) or 4
// (RGBA), rows top to bottom. Throws std::invalid_argument on a bad size or
// a pixel count that does not match it.
std::vector<std::uint8_t> encodePng(int width, int height, int channels, const std::vector<std::uint8_t>& pixels);

// The same with 16 bits a sample (a height map's precision), values as they are (0-65535).
std::vector<std::uint8_t> encodePng16(int width, int height, int channels, const std::vector<std::uint16_t>& pixels);

// encodePng written to `path` (UTF-8). Throws std::runtime_error when it cannot write.
void writeCompressedPng(const std::string& path, int width, int height, int channels,
                        const std::vector<std::uint8_t>& pixels);

void writeCompressedPng16(const std::string& path, int width, int height, int channels,
                          const std::vector<std::uint16_t>& pixels);

// An image for encodePngs: its pixels are not copied (they must outlive the call).
struct PngImage {
    int width = 0;
    int height = 0;
    int channels = 0;
    const std::vector<std::uint8_t>* pixels = nullptr;
};
// encodePng of each image, the images side by side (an export's several maps): the same bytes
// as one at a time. Throws std::invalid_argument as encodePng does, before encoding any.
std::vector<std::vector<std::uint8_t>> encodePngs(const std::vector<PngImage>& images);
// encodePngs, each written to its path (UTF-8). Throws std::runtime_error when one cannot be written.
void writeCompressedPngs(const std::vector<std::string>& paths, const std::vector<PngImage>& images);

// zlib-wrapped deflate of `data` (LZ77 + fixed Huffman codes), as PNG's IDAT holds it. The
// greedy parse runs in stretches of `stretchBytes` side by side (0: a megabyte), joined where
// the serial parse would be: any stretch size gives the same bytes (tests compare them).
std::vector<std::uint8_t> zlibCompress(const std::vector<std::uint8_t>& data, std::size_t stretchBytes = 0);

}  // namespace groomlab
