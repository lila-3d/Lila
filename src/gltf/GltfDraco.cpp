#include "gltf/GltfDraco.h"

#include "core/Profile.h"

#include <draco/compression/decode.h>
#include <draco/compression/encode.h>
#include <draco/mesh/mesh.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace lila::gltf
{
namespace
{
float normalizedMax(cgltf_component_type t)
{
    switch (t)
    {
    case cgltf_component_type_r_8: return 127.0f;
    case cgltf_component_type_r_8u: return 255.0f;
    case cgltf_component_type_r_16: return 32767.0f;
    case cgltf_component_type_r_16u: return 65535.0f;
    case cgltf_component_type_r_32u: return 4294967295.0f;
    default: return 1.0f;
    }
}

const cgltf_accessor* semanticAccessor(const cgltf_primitive& prim, const char* name)
{
    for (size_t a = 0; a < prim.attributes_count; ++a)
        if (prim.attributes[a].name && std::strcmp(prim.attributes[a].name, name) == 0)
            return prim.attributes[a].data;
    return nullptr;
}

std::unique_ptr<draco::Mesh> decode(const char* bytes, size_t size, std::string* error)
{
    draco::DecoderBuffer buffer;
    buffer.Init(bytes, size);
    draco::Decoder decoder;
    auto result = decoder.DecodeMeshFromBuffer(&buffer);
    if (!result.ok())
    {
        if (error)
            *error = result.status().error_msg_string();
        return nullptr;
    }
    return std::move(result).value();
}

draco::GeometryAttribute::Type dracoType(DracoAttribute::Kind k)
{
    switch (k)
    {
    case DracoAttribute::Position: return draco::GeometryAttribute::POSITION;
    case DracoAttribute::Normal: return draco::GeometryAttribute::NORMAL;
    case DracoAttribute::TexCoord: return draco::GeometryAttribute::TEX_COORD;
    case DracoAttribute::Color: return draco::GeometryAttribute::COLOR;
    default: return draco::GeometryAttribute::GENERIC;
    }
}
} // namespace

bool decodeDraco(const cgltf_data& data, const cgltf_primitive& prim, DracoPrimitive& out, std::string* error)
{
    LILA_ZONE();

    const cgltf_draco_mesh_compression& dc = prim.draco_mesh_compression;
    const uint8_t* bytes = dc.buffer_view ? cgltf_buffer_view_data(dc.buffer_view) : nullptr;
    if (!bytes)
    {
        if (error)
            *error = "its Draco buffer view has no data";
        return false;
    }
    std::unique_ptr<draco::Mesh> mesh = decode(reinterpret_cast<const char*>(bytes), dc.buffer_view->size, error);
    if (!mesh)
        return false;

    out = {};
    out.count = mesh->num_points();
    out.tris.reserve(size_t(mesh->num_faces()) * 3);
    for (draco::FaceIndex f(0); f < mesh->num_faces(); ++f)
        for (int k = 0; k < 3; ++k)
            out.tris.push_back(mesh->face(f)[k].value());
    // cgltf resolves the extension's attribute ids as accessor indices, so the id is that index.
    for (size_t a = 0; a < dc.attributes_count; ++a)
    {
        const cgltf_attribute& entry = dc.attributes[a];
        const cgltf_accessor* acc = entry.name ? semanticAccessor(prim, entry.name) : nullptr;
        if (!acc || !entry.data)
            continue;
        const auto id = uint32_t(cgltf_accessor_index(&data, entry.data));
        const draco::PointAttribute* att = mesh->GetAttributeByUniqueId(id);
        if (!att)
        {
            if (error)
                *error = std::string("its Draco data has no attribute for ") + entry.name;
            return false;
        }
        const int comps = int(cgltf_num_components(acc->type));
        std::vector<float> v(out.count * size_t(comps), 0.0f);
        for (draco::PointIndex p(0); p < mesh->num_points(); ++p)
            att->ConvertValue<float>(att->mapped_index(p), int8_t(comps), &v[size_t(p.value()) * size_t(comps)]);
        // Normalized glTF integers stay raw when the Draco attribute doesn't carry the flag.
        if (acc->normalized && !att->normalized() && att->data_type() != draco::DT_FLOAT32)
        {
            const float scale = 1.0f / normalizedMax(acc->component_type);
            for (float& x : v)
                x *= scale;
        }
        out.values[acc] = std::move(v);
    }
    return true;
}

bool encodeDraco(const std::vector<DracoAttribute>& attrs, size_t points, const std::vector<uint32_t>& tris, const DracoSettings& settings,
                 bool preserveOrder, DracoEncoded& out, std::string* error)
{
    LILA_ZONE();

    draco::Mesh mesh;
    mesh.set_num_points(uint32_t(points));
    for (size_t t = 0; t + 2 < tris.size(); t += 3)
        mesh.AddFace({draco::PointIndex(tris[t]), draco::PointIndex(tris[t + 1]), draco::PointIndex(tris[t + 2])});
    std::map<std::string, int> attrIds;
    for (const DracoAttribute& a : attrs)
    {
        const draco::DataType type = a.componentType == 5121 ? draco::DT_UINT8 : a.componentType == 5123 ? draco::DT_UINT16 : draco::DT_FLOAT32;
        const size_t size = type == draco::DT_UINT8 ? 1 : type == draco::DT_UINT16 ? 2 : 4;
        draco::GeometryAttribute ga;
        ga.Init(dracoType(a.kind), nullptr, uint8_t(a.components), type, false, int64_t(size * size_t(a.components)), 0);
        const int id = mesh.AddAttribute(ga, true, uint32_t(points));
        draco::PointAttribute* pa = mesh.attribute(id);
        const auto* src = static_cast<const uint8_t*>(a.data);
        for (size_t p = 0; p < points; ++p)
            pa->SetAttributeValue(draco::AttributeValueIndex(uint32_t(p)), src + p * size * size_t(a.components));
        attrIds[a.semantic] = id;
    }

    draco::Encoder encoder;
    encoder.SetAttributeQuantization(draco::GeometryAttribute::POSITION, settings.positionBits);
    encoder.SetAttributeQuantization(draco::GeometryAttribute::NORMAL, settings.normalBits);
    encoder.SetAttributeQuantization(draco::GeometryAttribute::TEX_COORD, settings.uvBits);
    encoder.SetAttributeQuantization(draco::GeometryAttribute::COLOR, settings.colorBits);
    encoder.SetAttributeQuantization(draco::GeometryAttribute::GENERIC, settings.genericBits);
    const int speed = 10 - std::clamp(settings.level, 0, 10);
    encoder.SetSpeedOptions(speed, speed);
    encoder.SetEncodingMethod(preserveOrder ? draco::MESH_SEQUENTIAL_ENCODING : draco::MESH_EDGEBREAKER_ENCODING);
    draco::EncoderBuffer buffer;
    if (draco::Status s = encoder.EncodeMeshToBuffer(mesh, &buffer); !s.ok())
    {
        if (error)
            *error = s.error_msg_string();
        return false;
    }
    out = {};
    out.bytes.assign(buffer.data(), buffer.size());
    for (const auto& [semantic, id] : attrIds)
        out.ids[semantic] = int(mesh.attribute(id)->unique_id());

    // The accessors must describe the mesh a decoder sees, which edgebreaker may renumber.
    std::unique_ptr<draco::Mesh> back = decode(out.bytes.data(), out.bytes.size(), error);
    if (!back)
        return false;
    out.points = back->num_points();
    out.indices = size_t(back->num_faces()) * 3;
    if (preserveOrder && out.points != points)
    {
        if (error)
            *error = "Draco changed the point count of an order-preserving mesh";
        return false;
    }
    const draco::PointAttribute* pos = back->GetNamedAttribute(draco::GeometryAttribute::POSITION);
    if (pos)
    {
        std::fill(std::begin(out.minPos), std::end(out.minPos), std::numeric_limits<float>::max());
        std::fill(std::begin(out.maxPos), std::end(out.maxPos), std::numeric_limits<float>::lowest());
        for (draco::PointIndex p(0); p < back->num_points(); ++p)
        {
            float v[3];
            pos->ConvertValue<float>(pos->mapped_index(p), 3, v);
            for (int c = 0; c < 3; ++c)
            {
                out.minPos[c] = std::min(out.minPos[c], v[c]);
                out.maxPos[c] = std::max(out.maxPos[c], v[c]);
            }
        }
    }
    return true;
}
} // namespace lila::gltf
