// SPDX-License-Identifier: Apache-2.0
// Ptex reading (ptex.h): files written here byte by byte as Ptex lays them out (PtexIO.h), with
// the core's own deflate, and baked into an image at UVs and into vertex values. That the
// conventions match XGen's files (corner order, rows of v) is checked in Maya against files XGen
// writes itself (tests/hosts/maya/test_xgen_import.py).

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>

#include "groomlab/png.h"
#include "groomlab/ptex.h"

using namespace groomlab;

namespace {

template <class T>
void put(std::vector<std::uint8_t>& out, T v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    out.insert(out.end(), p, p + sizeof(T));
}

struct FaceSpec {
    int ulog = 0, vlog = 0;
    std::vector<std::uint8_t> block;  // the face's data after its header
    std::uint32_t encoding = 0;
    int constant = -1;  // 0-255: a constant face (its texel in the constant data, its block empty)
};

// A face's texels (uint8, `nc` channels interleaved, rows of v) as Ptex stores them zipped: a
// channel at a time, optionally differenced.
std::vector<std::uint8_t> zipTexels(const std::vector<std::uint8_t>& interleaved, int nc, bool diff) {
    const std::size_t n = interleaved.size() / nc;
    std::vector<std::uint8_t> planar;
    for (int c = 0; c < nc; ++c) {
        for (std::size_t i = 0; i < n; ++i) planar.push_back(interleaved[i * nc + c]);
    }
    if (diff) {
        std::uint8_t prev = 0;
        for (auto& b : planar) {
            const std::uint8_t v = b;
            b = static_cast<std::uint8_t>(v - prev);
            prev = v;
        }
    }
    return zlibCompress(planar);
}

std::vector<std::uint8_t> ptexFile(int nc, const std::vector<FaceSpec>& faces) {
    std::vector<std::uint8_t> faceInfo, constData, headers, data;
    for (const FaceSpec& f : faces) {
        faceInfo.push_back(static_cast<std::uint8_t>(f.ulog));
        faceInfo.push_back(static_cast<std::uint8_t>(f.vlog));
        faceInfo.push_back(0);
        faceInfo.push_back(f.constant >= 0 ? 1 : 0);  // flag_constant
        for (int k = 0; k < 4; ++k) put<std::int32_t>(faceInfo, -1);
        for (int c = 0; c < nc; ++c) constData.push_back(static_cast<std::uint8_t>(std::max(0, f.constant)));
        put<std::uint32_t>(headers, static_cast<std::uint32_t>(f.block.size()) | (f.encoding << 30));
        data.insert(data.end(), f.block.begin(), f.block.end());
    }
    const auto faceInfoZip = zlibCompress(faceInfo), constZip = zlibCompress(constData),
               headersZip = zlibCompress(headers);
    std::vector<std::uint8_t> out;
    put<std::uint32_t>(out, 0x78657450);  // magic
    put<std::uint32_t>(out, 1);           // version
    put<std::uint32_t>(out, 1);           // quads
    put<std::uint32_t>(out, 0);           // uint8
    put<std::int32_t>(out, -1);           // no alpha
    put<std::uint16_t>(out, static_cast<std::uint16_t>(nc));
    put<std::uint16_t>(out, 1);  // levels
    put<std::uint32_t>(out, static_cast<std::uint32_t>(faces.size()));
    put<std::uint32_t>(out, 48);  // extheadersize
    put<std::uint32_t>(out, static_cast<std::uint32_t>(faceInfoZip.size()));
    put<std::uint32_t>(out, static_cast<std::uint32_t>(constZip.size()));
    put<std::uint32_t>(out, 16);  // levelinfosize
    put<std::uint32_t>(out, 3);   // minor version
    put<std::uint64_t>(out, headersZip.size() + data.size());
    put<std::uint32_t>(out, 0);
    put<std::uint32_t>(out, 0);
    out.resize(out.size() + 48, 0);  // the extension header
    out.insert(out.end(), faceInfoZip.begin(), faceInfoZip.end());
    out.insert(out.end(), constZip.begin(), constZip.end());
    put<std::uint64_t>(out, headersZip.size() + data.size());
    put<std::uint32_t>(out, static_cast<std::uint32_t>(headersZip.size()));
    put<std::uint32_t>(out, static_cast<std::uint32_t>(faces.size()));
    out.insert(out.end(), headersZip.begin(), headersZip.end());
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

// Three gray faces: a constant one (51, in the constant data), a 2 x 2 one (0, 255 / 102, 204
// rows of v, differenced) and a tiled 4 x 2 one (tiles 2 x 2: a constant tile of 255, then a zipped tile 0 0
// / 0 0).
std::vector<std::uint8_t> sampleFile() {
    std::vector<FaceSpec> faces(3);
    faces[0].constant = 51;
    faces[1].ulog = 1;
    faces[1].vlog = 1;
    faces[1].block = zipTexels({0, 255, 102, 204}, 1, true);
    faces[1].encoding = 2;
    faces[2].ulog = 2;
    faces[2].vlog = 1;
    std::vector<std::uint8_t> tile0 = {255};
    std::vector<std::uint8_t> tile1 = zipTexels({0, 0, 0, 0}, 1, false);
    std::vector<std::uint8_t> tileHeaders;
    put<std::uint32_t>(tileHeaders, static_cast<std::uint32_t>(tile0.size()));
    put<std::uint32_t>(tileHeaders, static_cast<std::uint32_t>(tile1.size()) | (1u << 30));
    const auto tileHeadersZip = zlibCompress(tileHeaders);
    std::vector<std::uint8_t>& b = faces[2].block;
    b.push_back(1);  // tile res: 2 x 2
    b.push_back(1);
    put<std::uint32_t>(b, static_cast<std::uint32_t>(tileHeadersZip.size()));
    b.insert(b.end(), tileHeadersZip.begin(), tileHeadersZip.end());
    b.insert(b.end(), tile0.begin(), tile0.end());
    b.insert(b.end(), tile1.begin(), tile1.end());
    faces[2].encoding = 3;
    return ptexFile(1, faces);
}

}  // namespace

TEST(Ptex, ReadsConstantZippedDifferencedAndTiledFaces) {
    const PtexImage image = readPtexBytes(sampleFile());
    ASSERT_EQ(image.faces.size(), 3u);
    EXPECT_EQ(image.channels, 1);
    EXPECT_TRUE(image.faces[0].constant);
    EXPECT_NEAR(image.faces[0].pixels[0], 0.2f, 1e-6f);
    ASSERT_EQ(image.faces[1].pixels.size(), 4u);
    EXPECT_NEAR(image.faces[1].pixels[1], 1.0f, 1e-6f);
    EXPECT_NEAR(image.faces[1].pixels[2], 0.4f, 1e-6f);
    ASSERT_EQ(image.faces[2].pixels.size(), 8u);
    EXPECT_NEAR(image.faces[2].pixels[0], 1.0f, 1e-6f);  // tile 0 (constant)
    EXPECT_NEAR(image.faces[2].pixels[5], 1.0f, 1e-6f);
    EXPECT_NEAR(image.faces[2].pixels[2], 0.0f, 1e-6f);  // tile 1
    EXPECT_NEAR(image.faces[2].pixels[7], 0.0f, 1e-6f);

    float v = 0.0f;
    image.sample(1, 0.0f, 0.0f, &v);  // corner 0: texel (0, 0)
    EXPECT_NEAR(v, 0.0f, 1e-6f);
    image.sample(1, 1.0f, 0.0f, &v);  // corner 1: along u
    EXPECT_NEAR(v, 1.0f, 1e-6f);
    image.sample(1, 0.0f, 1.0f, &v);  // corner 3: along v (the second row)
    EXPECT_NEAR(v, 0.4f, 1e-6f);
    image.sample(1, 0.5f, 0.5f, &v);  // the middle: the four texels' mean
    EXPECT_NEAR(v, (0.0f + 1.0f + 0.4f + 0.8f) / 4.0f, 1e-5f);
    EXPECT_NEAR(image.mean(2)[0], 0.5f, 1e-6f);
}

TEST(Ptex, RefusesWhatItCannotRead) {
    std::vector<std::uint8_t> bytes = sampleFile();
    EXPECT_THROW(readPtexBytes(std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + 40)), PtexError);
    std::vector<std::uint8_t> triangles = bytes;
    triangles[8] = 0;  // mesh type: triangles
    EXPECT_THROW(readPtexBytes(triangles), PtexError);
    std::vector<std::uint8_t> cut(bytes.begin(), bytes.end() - 3);
    EXPECT_THROW(readPtexBytes(cut), PtexError);
    EXPECT_THROW(readPtex("no/such/file.ptx"), PtexError);
}

TEST(Ptex, BakesIntoAnImageAtUVsAndIntoVertexValues) {
    const PtexImage image = readPtexBytes(sampleFile());
    // Three quads side by side in UV space: x in [0, 1/3], [1/3, 2/3], [2/3, 1], y 0-1, each
    // polygon's corners counter-clockwise from bottom left (so u runs along +U, v along +V).
    PtexMesh mesh;
    mesh.faceCounts = {4, 4, 4};
    mesh.vertexCount = 8;
    for (int i = 0; i <= 3; ++i) {
        mesh.us.push_back(i / 3.0f);
        mesh.vs.push_back(0.0f);
    }
    for (int i = 0; i <= 3; ++i) {
        mesh.us.push_back(i / 3.0f);
        mesh.vs.push_back(1.0f);
    }
    for (int f = 0; f < 3; ++f) {
        const int c[4] = {f, f + 1, f + 5, f + 4};
        for (int k : c) {
            mesh.uvIds.push_back(k);
            mesh.vertexIds.push_back(k);
        }
    }
    double covered = 0.0;
    const int w = 30, h = 10;
    const std::vector<float> pixels = ptexToImage(image, mesh, w, h, 1, 2, &covered);
    ASSERT_EQ(pixels.size(), static_cast<std::size_t>(w * h));
    EXPECT_GT(covered, 0.99);
    auto at = [&](int x, int y) { return pixels[y * w + x]; };
    EXPECT_NEAR(at(5, 5), 0.2f, 1e-5f);   // face 0: constant
    EXPECT_NEAR(at(10, 9), 0.0f, 0.02f);  // face 1's corner 0 (bottom left, the last row)
    EXPECT_NEAR(at(19, 9), 1.0f, 0.02f);  // its corner 1 (bottom right)
    EXPECT_NEAR(at(10, 0), 0.4f, 0.02f);  // its corner 3 (top left)
    EXPECT_NEAR(at(19, 0), 0.8f, 0.02f);  // its corner 2 (top right)
    EXPECT_NEAR(at(21, 5), 1.0f, 0.02f);  // face 2: the constant tile on its left half
    EXPECT_NEAR(at(28, 5), 0.0f, 0.02f);  // the zipped tile on its right

    const std::vector<float> rgba = ptexToImage(image, mesh, w, h, 4, 2);
    EXPECT_NEAR(rgba[(5 * w + 5) * 4 + 1], 0.2f, 1e-5f);  // gray into g
    EXPECT_NEAR(rgba[(5 * w + 5) * 4 + 3], 1.0f, 1e-6f);  // alpha 1

    const std::vector<float> values = ptexToVertexValues(image, mesh, 1);
    ASSERT_EQ(values.size(), 8u);
    EXPECT_NEAR(values[0], 0.2f, 1e-5f);                  // face 0 alone
    EXPECT_NEAR(values[1], (0.2f + 0.0f) / 2.0f, 1e-5f);  // faces 0 and 1's corner 0
    EXPECT_NEAR(values[6], (0.8f + 1.0f) / 2.0f, 1e-5f);  // face 1's corner 2 and face 2's corner 3 (tile 0)

    PtexMesh wrong = mesh;
    wrong.faceCounts = {4, 4};
    EXPECT_THROW(ptexToImage(image, wrong, w, h, 1), PtexError);
    PtexMesh noUvs = mesh;
    noUvs.uvIds.clear();
    EXPECT_THROW(ptexToImage(image, noUvs, w, h, 1), PtexError);
}

TEST(Ptex, OtherPolygonsTakeOneSubFacePerCorner) {
    // One triangle: three constant sub-faces, 51, 102 and 153, one at each corner.
    std::vector<FaceSpec> faces(3);
    for (int k = 0; k < 3; ++k) faces[k].constant = 51 * (k + 1);
    const PtexImage image = readPtexBytes(ptexFile(1, faces));
    PtexMesh mesh;
    mesh.faceCounts = {3};
    mesh.vertexCount = 3;
    mesh.us = {0.0f, 1.0f, 0.0f};
    mesh.vs = {0.0f, 0.0f, 1.0f};
    mesh.uvIds = {0, 1, 2};
    mesh.vertexIds = {0, 1, 2};
    const std::vector<float> values = ptexToVertexValues(image, mesh, 1);
    ASSERT_EQ(values.size(), 3u);
    for (int k = 0; k < 3; ++k) EXPECT_NEAR(values[k], 0.2f * (k + 1), 1e-5f);
    const int size = 30;
    const std::vector<float> pixels = ptexToImage(image, mesh, size, size, 1, 0);
    EXPECT_NEAR(pixels[(size - 2) * size + 1], 0.2f, 1e-5f);  // near corner 0 (UV 0, 0: the bottom left)
    EXPECT_NEAR(pixels[(size - 2) * size + size - 4], 0.4f, 1e-5f);  // near corner 1 (UV 1, 0)
    EXPECT_NEAR(pixels[3 * size + 1], 0.6f, 1e-5f);                  // near corner 2 (UV 0, 1)
    PtexMesh asQuad = mesh;
    asQuad.faceCounts = {4};
    EXPECT_THROW(ptexToVertexValues(image, asQuad, 1), PtexError);  // a quad would be one face
}
