// Sample C++ file translator: reads Blender 5.0's default scene (a cube, a camera and a point light in a collection).

#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/FileTranslator.h>
#include <lila/Graph.h>
#include <lila/Interchange.h>
#include <lila/Lights.h>
#include <lila/Math.h>
#include <lila/Mesh.h>
#include <lila/Transform.h>

#include <zstd.h>

#include <cstring>
#include <fstream>
#include <map>
#include <unordered_map>

using namespace lila;

namespace
{
constexpr double kCmPerMetre = 100.0;

std::filesystem::path utf8Path(const std::string& s)
{
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

template <typename T>
T read(const uint8_t* p)
{
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

/** A .blend: its blocks by address, and the struct layouts (SDNA) the file describes itself with. */
class Blend
{
public:
    struct Field
    {
        size_t offset = 0;
        std::string type;
        bool pointer = false;
        size_t count = 1;
    };
    struct Block
    {
        std::string code;
        uint32_t sdna = 0;
        const uint8_t* data = nullptr;
        size_t size = 0;
    };

    /** One struct in the file. Pointers come back as addresses for at(). */
    struct View
    {
        const Blend* blend = nullptr;
        std::string type;
        const uint8_t* data = nullptr;

        explicit operator bool() const { return data != nullptr; }
        const Field& field(const std::string& name) const
        {
            const auto& fields = blend->mLayouts.at(type);
            auto it = fields.find(name);
            if (it == fields.end())
                throw CommandError("importBlend: " + type + " has no field " + name);
            return it->second;
        }
        template <typename T>
        T get(const std::string& name, size_t i = 0) const { return read<T>(data + field(name).offset + i * sizeof(T)); }
        uint64_t ptr(const std::string& name) const { return get<uint64_t>(name); }
        std::string str(const std::string& name) const
        {
            const Field& f = field(name);
            const char* s = reinterpret_cast<const char*>(data + f.offset);
            return std::string(s, strnlen(s, f.count));
        }
        View sub(const std::string& name) const
        {
            const Field& f = field(name);
            return {blend, f.type, data + f.offset};
        }
        /** The ID's name without Blender's two-letter type prefix ("OBCube" -> "Cube"). */
        std::string name() const { return sub("id").str("name").substr(2); }
    };

    explicit Blend(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            throw CommandError("importBlend: can't read the file");
        mBytes.assign(std::istreambuf_iterator<char>(in), {});
        if (mBytes.size() >= 4 && read<uint32_t>(mBytes.data()) == ZSTD_MAGICNUMBER)
            decompress();
        if (mBytes.size() < 17 || std::memcmp(mBytes.data(), "BLENDER17-01v", 13) != 0)
            throw CommandError("importBlend: not a Blender 5.0 file");
        for (size_t pos = 17; pos + 32 <= mBytes.size();)
        {
            const uint8_t* h = mBytes.data() + pos;
            Block b{.code = std::string(reinterpret_cast<const char*>(h), strnlen(reinterpret_cast<const char*>(h), 4)),
                    .sdna = read<uint32_t>(h + 4),
                    .data = h + 32,
                    .size = size_t(read<uint64_t>(h + 16))};
            if (b.code == "ENDB")
                break;
            mByAddress[read<uint64_t>(h + 8)] = mBlocks.size();
            mBlocks.push_back(b);
            pos += 32 + b.size;
        }
        for (const Block& b : mBlocks)
            if (b.code == "DNA1")
                readSdna(b.data);
    }

    /** Every datablock with this block code ("OB", "ME", "SC"). */
    std::vector<View> ids(std::string_view code) const
    {
        std::vector<View> out;
        for (const Block& b : mBlocks)
            if (b.code == code)
                out.push_back({this, mStructs[b.sdna], b.data});
        return out;
    }
    View at(uint64_t address, const std::string& type = {}) const
    {
        const Block* b = block(address);
        return b ? View{this, type.empty() ? mStructs[b->sdna] : type, b->data} : View{};
    }
    const Block* block(uint64_t address) const
    {
        auto it = mByAddress.find(address);
        return it == mByAddress.end() ? nullptr : &mBlocks[it->second];
    }
    /** A ListBase's items. */
    std::vector<View> items(const View& listBase, const std::string& type) const
    {
        std::vector<View> out;
        for (uint64_t a = listBase.ptr("first"); a;)
        {
            out.push_back(at(a, type));
            a = out.back().ptr("next");
        }
        return out;
    }
    size_t sizeOf(const std::string& type) const { return mSizes.at(type); }

private:
    /** Blender writes several frames plus a seek table, so it streams rather than trusting one frame's size. */
    void decompress()
    {
        std::vector<uint8_t> out;
        ZSTD_DStream* ds = ZSTD_createDStream();
        ZSTD_inBuffer src{mBytes.data(), mBytes.size(), 0};
        std::vector<uint8_t> chunk(ZSTD_DStreamOutSize());
        while (src.pos < src.size)
        {
            ZSTD_outBuffer dst{chunk.data(), chunk.size(), 0};
            if (ZSTD_isError(ZSTD_decompressStream(ds, &dst, &src)))
            {
                ZSTD_freeDStream(ds);
                throw CommandError("importBlend: the file is damaged");
            }
            out.insert(out.end(), chunk.begin(), chunk.begin() + dst.pos);
        }
        ZSTD_freeDStream(ds);
        mBytes = std::move(out);
    }

    void readSdna(const uint8_t* d)
    {
        size_t pos = 8; // "SDNANAME"
        auto align = [&] { pos = (pos + 3) & ~size_t(3); };
        auto strings = [&] {
            std::vector<std::string> out(read<uint32_t>(d + pos));
            pos += 4;
            for (std::string& s : out)
            {
                s = reinterpret_cast<const char*>(d + pos);
                pos += s.size() + 1;
            }
            align();
            return out;
        };
        const std::vector<std::string> names = strings();
        pos += 4; // "TYPE"
        const std::vector<std::string> types = strings();
        pos += 4; // "TLEN"
        std::vector<uint16_t> lengths(types.size());
        for (size_t i = 0; i < types.size(); ++i, pos += 2)
            mSizes[types[i]] = lengths[i] = read<uint16_t>(d + pos);
        align();
        pos += 4; // "STRC"
        const uint32_t count = read<uint32_t>(d + pos);
        pos += 4;
        for (uint32_t s = 0; s < count; ++s)
        {
            const std::string& type = types[read<uint16_t>(d + pos)];
            const uint16_t members = read<uint16_t>(d + pos + 2);
            pos += 4;
            mStructs.push_back(type);
            std::map<std::string, Field>& fields = mLayouts[type];
            size_t offset = 0;
            for (uint16_t m = 0; m < members; ++m, pos += 4)
            {
                const uint16_t memberType = read<uint16_t>(d + pos);
                const std::string& raw = names[read<uint16_t>(d + pos + 2)];
                Field f{.offset = offset, .type = types[memberType], .pointer = raw[0] == '*' || raw[0] == '('};
                std::string bare;
                for (size_t i = 0; i < raw.size(); ++i)
                    if (raw[i] == '[')
                    {
                        f.count *= std::stoul(raw.substr(i + 1));
                        i = raw.find(']', i);
                    }
                    else if (raw[i] != '*' && raw[i] != '(' && raw[i] != ')')
                        bare += raw[i];
                offset += (f.pointer ? 8 : lengths[memberType]) * f.count;
                fields[bare] = f;
            }
        }
    }

    std::vector<uint8_t> mBytes;
    std::vector<Block> mBlocks;
    std::unordered_map<uint64_t, size_t> mByAddress;
    std::vector<std::string> mStructs; // by SDNA index
    std::unordered_map<std::string, std::map<std::string, Field>> mLayouts;
    std::unordered_map<std::string, size_t> mSizes;
};

/** A Blender 5.0 mesh's attributes (position, .corner_vert, UVMap, ...) by name. */
std::map<std::string, const uint8_t*> meshAttributes(const Blend& blend, const Blend::View& mesh)
{
    std::map<std::string, const uint8_t*> out;
    const Blend::View storage = mesh.sub("attribute_storage");
    const Blend::Block* table = blend.block(storage.ptr("dna_attributes"));
    const size_t stride = blend.sizeOf("Attribute");
    for (int i = 0; table && i < storage.get<int>("dna_attributes_num"); ++i)
    {
        const Blend::View a{&blend, "Attribute", table->data + i * stride};
        const Blend::Block* name = blend.block(a.ptr("name"));
        const Blend::View array = blend.at(a.ptr("data"), "AttributeArray");
        const Blend::Block* values = array ? blend.block(array.ptr("data")) : nullptr;
        if (name && values)
            out[reinterpret_cast<const char*>(name->data)] = values->data;
    }
    return out;
}

/** Blender is Z up. A Y-up Lila scene turns it -90 degrees about X: (x, y, z) -> (x, z, -y). */
glm::dmat3 toLilaAxes()
{
    if (math::upAxis() == math::UpAxis::Z)
        return glm::dmat3(1.0);
    return glm::dmat3(glm::dvec3(1, 0, 0), glm::dvec3(0, 0, -1), glm::dvec3(0, 1, 0));
}

/** Sets translate, rotate and scale from Blender's location, XYZ Euler and scale, turned onto Lila's axes. */
void setTransform(scene::Graph& g, scene::NodeId id, const Blend::View& ob, const glm::dmat3& axes, bool turnLocalAxes)
{
    const glm::dvec3 loc(ob.get<float>("loc", 0), ob.get<float>("loc", 1), ob.get<float>("loc", 2));
    const glm::dvec3 rot(ob.get<float>("rot", 0), ob.get<float>("rot", 1), ob.get<float>("rot", 2));
    glm::dvec3 scale(ob.get<float>("size", 0), ob.get<float>("size", 1), ob.get<float>("size", 2));
    glm::dmat3 r = axes * glm::dmat3(scene::eulerToMatrix(glm::degrees(rot), scene::RotateOrder::XYZ));
    // A mesh's points were turned with the axes, so its own axes turn too. Cameras and lights look down -Z in both.
    if (turnLocalAxes)
    {
        r = r * glm::transpose(axes);
        scale = glm::abs(axes * scale);
    }
    g.setValue({id, scene::Transform::kTranslate, -1}, axes * loc * kCmPerMetre);
    g.setValue({id, scene::Transform::kRotate, -1}, scene::matrixToEuler(r, scene::RotateOrder::XYZ));
    g.setValue({id, scene::Transform::kScale, -1}, scale);
}

scene::NodeId makeMesh(scene::Graph& g, const Blend& blend, const Blend::View& ob, scene::NodeId parent, const glm::dmat3& axes)
{
    const Blend::View me = blend.at(ob.ptr("data"));
    const auto attrs = meshAttributes(blend, me);
    const int numPoints = me.get<int>("totvert"), numFaces = me.get<int>("totpoly"), numCorners = me.get<int>("totloop");
    const Blend::Block* offsets = blend.block(me.ptr("poly_offset_indices"));
    if (!attrs.contains("position") || !attrs.contains(".corner_vert") || !offsets)
        throw CommandError("importBlend: " + ob.name() + " has no geometry");

    std::vector<glm::dvec3> points(static_cast<size_t>(numPoints));
    for (int i = 0; i < numPoints; ++i)
    {
        const float* p = reinterpret_cast<const float*>(attrs.at("position")) + 3 * i;
        points[size_t(i)] = axes * glm::dvec3(p[0], p[1], p[2]) * kCmPerMetre;
    }
    std::vector<geo::Index> sizes, corners(static_cast<size_t>(numCorners));
    for (int f = 0; f < numFaces; ++f)
        sizes.push_back(geo::Index(read<int>(offsets->data + 4 * (f + 1)) - read<int>(offsets->data + 4 * f)));
    for (int c = 0; c < numCorners; ++c)
        corners[size_t(c)] = geo::Index(read<int>(attrs.at(".corner_vert") + 4 * c));
    auto mesh = std::make_shared<geo::Mesh>(geo::Mesh::fromPolygons(points, sizes, corners));

    if (auto uvMap = attrs.find("UVMap"); uvMap != attrs.end() && mesh->numFaces() == geo::Index(numFaces))
    {
        std::span<glm::vec2> uv = mesh->cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
        const glm::vec2* src = reinterpret_cast<const glm::vec2*>(uvMap->second);
        for (geo::Index f = 0, at = 0; f < mesh->numFaces(); ++f)
        {
            geo::Index c = mesh->faceCorner(f);
            for (geo::Index k = 0; k < sizes[f]; ++k, c = mesh->cornerNext(c))
                uv[c] = src[at++];
        }
    }

    scene::Node* n = g.createNode(*scene::types::find("mesh"), ob.name(), parent);
    g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::move(mesh)));
    setTransform(g, n->id(), ob, axes, true);
    return n->id();
}

scene::NodeId makeCamera(scene::Graph& g, const Blend& blend, const Blend::View& ob, scene::NodeId parent, const glm::dmat3& axes)
{
    using scene::Camera;
    const Blend::View ca = blend.at(ob.ptr("data"));
    scene::Node* n = g.createNode(*scene::types::find("camera"), ob.name(), parent);
    auto set = [&](int attr, scene::PlugValue v) { g.setValue({n->id(), attr, -1}, std::move(v)); };
    set(Camera::kFocalLength, double(ca.get<float>("lens")));
    set(Camera::kHorizontalFilmAperture, ca.get<float>("sensor_x") / 25.4);
    set(Camera::kVerticalFilmAperture, ca.get<float>("sensor_y") / 25.4);
    set(Camera::kFilmFit, int64_t(ca.get<char>("sensor_fit") == 2 ? scene::FilmFit::Vertical : scene::FilmFit::Horizontal));
    set(Camera::kNearClipPlane, ca.get<float>("clipsta") * kCmPerMetre);
    set(Camera::kFarClipPlane, ca.get<float>("clipend") * kCmPerMetre);
    setTransform(g, n->id(), ob, axes, false);
    return n->id();
}

scene::NodeId makeLight(scene::Graph& g, const Blend& blend, const Blend::View& ob, scene::NodeId parent, const glm::dmat3& axes)
{
    const Blend::View la = blend.at(ob.ptr("data"));
    if (la.get<short>("type") != 0)
        return scene::kNoNode; // only point lights
    scene::lights::Settings s;
    s.color = glm::dvec3(la.get<float>("r"), la.get<float>("g"), la.get<float>("b"));
    s.intensity = la.get<float>("energy");
    s.units = scene::lights::Units::Watts;
    s.sourceRadius = la.get<float>("radius") * kCmPerMetre;
    s.sourceFormat = "blend:point";
    scene::Node& n = scene::lights::create(g, ob.name(), parent, s);
    setTransform(g, n.id(), ob, axes, false);
    return n.id();
}

void addCollection(scene::Graph& g, const Blend& blend, const Blend::View& collection, scene::NodeId parent, const glm::dmat3& axes,
                   json& made)
{
    for (const Blend::View& child : blend.items(collection.sub("children"), "CollectionChild"))
    {
        const Blend::View sub = blend.at(child.ptr("collection"));
        scene::Node* group = g.createNode(*scene::types::find("transform"), sub.name(), parent);
        made.push_back(group->path());
        addCollection(g, blend, sub, group->id(), axes, made);
    }
    for (const Blend::View& entry : blend.items(collection.sub("gobject"), "CollectionObject"))
    {
        const Blend::View ob = blend.at(entry.ptr("ob"));
        scene::NodeId id = scene::kNoNode;
        switch (ob.get<short>("type"))
        {
        case 1: id = makeMesh(g, blend, ob, parent, axes); break;    // OB_MESH
        case 10: id = makeLight(g, blend, ob, parent, axes); break;  // OB_LAMP
        case 11: id = makeCamera(g, blend, ob, parent, axes); break; // OB_CAMERA
        }
        if (id != scene::kNoNode)
            made.push_back(g.node(id)->path());
    }
}

json importBlend(const Args& a)
{
    if (a.positional.size() != 1)
        throw CommandError("importBlend: give one path");
    const Blend blend(utf8Path(a.positional[0].get<std::string>()));
    const std::vector<Blend::View> scenes = blend.ids("SC");
    if (scenes.empty())
        throw CommandError("importBlend: the file has no scene");
    scene::Graph& g = scene::Graph::current();
    json made = json::array();
    addCollection(g, blend, blend.at(scenes.front().ptr("master_collection")), scene::kNoNode, toLilaAxes(), made);
    return made;
}
} // namespace

LILA_EXTENSION_EXPORT int lilaExtensionInit(ext::Context& ctx)
{
    if (!ctx.registerCommand({.name = "importBlend",
                              .doc = "Import a Blender 5.0 scene: each collection becomes a group holding its meshes, cameras and point lights.",
                              .positionalDoc = "path",
                              .run = importBlend}))
        return 1;
    if (!io::translators::add(ctx, {.id = "blend",
                                    .label = "Blender scenes",
                                    .doc = "Blender 5.0 scenes: meshes, cameras and point lights, by collection.",
                                    .extensions = {".blend"},
                                    .import = {.command = "importBlend"}}))
        return 2;
    return 0;
}
