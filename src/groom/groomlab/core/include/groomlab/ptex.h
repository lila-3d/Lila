// SPDX-License-Identifier: Apache-2.0
#pragma once

// Ptex files (per-face textures, Disney's format: XGen's paint maps, ${DESC}/paintmaps/<map>/<mesh>.ptx)
// read without the Ptex library, and turned into what GroomLab maps hold: an image at a mesh's UVs
// or a value per vertex. Maya ships Ptex.dll and its headers but no import library, and the format
// is small and fixed since 2010: a header, per-face resolutions, then each face's texels, deflated
// (stb's inflate, already in the core for PNG). Only the full-resolution level is read (no mipmaps),
// quad meshes only (Ptex's triangle layout is not read: XGen and Maya write quad files), every data
// type (8 and 16 bits as 0-1, half and float as they are), tiled faces, constant faces (their texel
// in the constant data), the differenced encoding. Edits and per-face metadata are ignored.
//
// Faces follow the mesh's polygons in order, as Ptex numbers them (and XGen's files, checked against
// files XGen writes): a quad is one face, any other polygon one sub-face per corner. Within a quad,
// u runs from corner 0 to corner 1 and v from corner 0 to corner 3 (texel (i, j) at
// u = (i + 0.5) / ures, v = (j + 0.5) / vres, rows of v). Sub-face k of an n-gon is the quad from
// corner k (u toward the middle of the edge to corner k + 1, v toward the middle of the edge from
// corner k - 1) to the polygon's centre.

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace groomlab {

class PtexError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct PtexImage {
    enum class DataType { UInt8 = 0, UInt16 = 1, Half = 2, Float = 3 };
    struct Face {
        int ures = 1;
        int vres = 1;
        bool constant = false;      // one texel stands for the face (pixels holds one)
        std::vector<float> pixels;  // ures * vres * channels, rows of v (constant: channels)
    };
    int channels = 0;
    int alphaChannel = -1;
    DataType dataType = DataType::UInt8;
    std::vector<Face> faces;

    // Face `face`'s value at (u, v) in [0, 1]: bilinear between its texel centers, clamped at its
    // edges. `out` gets `channels` floats. Throws PtexError for a face out of range.
    void sample(int face, float u, float v, float* out) const;
    // A face's mean, `channels` floats.
    std::vector<float> mean(int face) const;
};

// Reads a .ptx file (UTF-8 path). Throws PtexError when it cannot be read or is not a quad Ptex file.
PtexImage readPtex(const std::string& path);
// The same from the file's bytes (tests).
PtexImage readPtexBytes(const std::vector<std::uint8_t>& bytes);

// A polygon mesh as the bakes read it: corners per polygon, then per corner its UV's index (into
// us, vs) and its vertex's index.
struct PtexMesh {
    std::vector<int> faceCounts;
    std::vector<int> uvIds;  // one per corner (empty: no UVs)
    std::vector<float> us, vs;
    std::vector<int> vertexIds;  // one per corner
    int vertexCount = 0;
};

// The Ptex painted into an image at the mesh's UVs (0-1 tile; other tiles are left out), rows top
// down (v = 0 the bottom row, as Texture samples), `channels` floats a pixel: the file's first
// channels (a gray file repeated into r g b when 3 or 4 are asked; alpha 1 when the file has
// none). Pixels no polygon covers take their covered neighbours' value, `padding` pixels out,
// then the image's mean, so bilinear lookups at UV seams stay right. `covered` (optional) gets the
// share of pixels some polygon covered. Throws PtexError when the mesh has no UVs or its Ptex face
// count (quads and sub-faces, above) is not the file's.
std::vector<float> ptexToImage(const PtexImage& ptex, const PtexMesh& mesh, int width, int height,
                               int channels, int padding = 4, double* covered = nullptr);

// The Ptex as a value per mesh vertex: the mean of the texels at the vertex's corner in every
// polygon round it, `channels` floats a vertex as ptexToImage gives them. Vertices on no polygon
// get 0. Throws PtexError as ptexToImage does when the face counts differ.
std::vector<float> ptexToVertexValues(const PtexImage& ptex, const PtexMesh& mesh, int channels);

}  // namespace groomlab
