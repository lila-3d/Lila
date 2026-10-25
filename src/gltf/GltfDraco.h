#pragma once

#include <cgltf.h>

#include <map>
#include <string>
#include <vector>

/** KHR_draco_mesh_compression through the Draco library. */
namespace lila::gltf
{
/** A decoded Draco primitive: each compressed attribute's values by the primitive's accessor for it, and the triangles. */
struct DracoPrimitive
{
    size_t count = 0;
    std::map<const cgltf_accessor*, std::vector<float>> values;
    std::vector<uint32_t> tris;
};

/** Decodes the primitive's Draco buffer; false with error set when it can't. Normalized integers come back 0 to 1. */
bool decodeDraco(const cgltf_data& data, const cgltf_primitive& prim, DracoPrimitive& out, std::string* error);

struct DracoSettings
{
    int positionBits = 14;
    int normalBits = 10;
    int uvBits = 12;
    int colorBits = 8;
    int genericBits = 12;
    /** 0 fastest to 10 smallest. */
    int level = 7;
};

/** One vertex attribute to compress: floats, or unsigned bytes or shorts (joints). */
struct DracoAttribute
{
    enum Kind
    {
        Position,
        Normal,
        TexCoord,
        Color,
        Generic,
    };
    std::string semantic; // the glTF attribute name
    Kind kind = Generic;
    int components = 3;
    int componentType = 5126; // glTF component type: float, unsigned byte or unsigned short
    const void* data = nullptr;
};

struct DracoEncoded
{
    std::string bytes;
    /** Draco attribute id per glTF semantic. */
    std::map<std::string, int> ids;
    /** Points and indices of the mesh as a decoder will see it, and its position bounds. */
    size_t points = 0;
    size_t indices = 0;
    float minPos[3] = {0, 0, 0}, maxPos[3] = {0, 0, 0};
};

/**
 * Encodes one primitive. preserveOrder uses sequential encoding, so decoded points keep their order (needed when
 * uncompressed morph targets index them); otherwise edgebreaker. False with error set on failure.
 */
bool encodeDraco(const std::vector<DracoAttribute>& attrs, size_t points, const std::vector<uint32_t>& tris, const DracoSettings& settings,
                 bool preserveOrder, DracoEncoded& out, std::string* error);
} // namespace lila::gltf
