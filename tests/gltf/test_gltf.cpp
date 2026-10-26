#include "Helpers.h"

#include "geo/ColorSets.h"
#include "geo/Normals.h"
#include "geo/Primitives.h"
#include "io/FileTranslators.h"
#include "math/MathCore.h"
#include "scene/Anim.h"
#include "scene/Camera.h"
#include "scene/Light.h"
#include "fbx/Fbx.h"
#include "scene/Materials.h"

#include <glm/gtc/constants.hpp>

#include <map>
#include <set>
#include <tuple>

using namespace gltftest;

namespace
{
/** A cube with two materials, a colour set and a second uv set. */
geo::Mesh decoratedCube(double size)
{
    geo::Mesh m = geo::makeCube({.width = size, .height = size, .depth = size});
    m.materialNames() = {"M_Body", "M_Trim"};
    std::span<int32_t> mat = m.faceAttrs().getOrAdd<int32_t>(geo::attr::kMaterial);
    for (size_t f = 0; f < mat.size(); ++f)
        mat[f] = int32_t(f % 2);
    std::span<glm::vec4> col = geo::colors::add(m, "Paint");
    for (size_t c = 0; c < col.size(); ++c)
        col[c] = glm::vec4(float(c % 3) / 2.0f, 0.25f, 1.0f, 1.0f);
    std::vector<glm::vec2> uv0(m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(0)).begin(),
                               m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(0)).end());
    std::span<glm::vec2> uv1 = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(1));
    for (size_t c = 0; c < uv1.size(); ++c)
        uv1[c] = uv0[c] * 0.5f + glm::vec2(0.25f);
    return m;
}

struct Textures
{
    std::string base, normal, orm;
};

Textures writeTextures()
{
    const fs::path dir = tempDir("sources");
    Textures t;
    t.base = (dir / "T_Body_BC.png").generic_string();
    pngBytes(t.base, 4, 4, [](int x, int y) { return glm::vec4(float(x) / 3.0f, float(y) / 3.0f, 0.5f, 1.0f); });
    t.normal = (dir / "T_Body_N.png").generic_string();
    pngBytes(t.normal, 4, 4, [](int, int) { return glm::vec4(0.5f, 0.25f, 1.0f, 1.0f); });
    t.orm = (dir / "T_Body_ORM.png").generic_string();
    pngBytes(t.orm, 4, 4, [](int x, int) { return glm::vec4(1.0f, float(x) / 3.0f, 0.2f, 1.0f); });
    return t;
}

void buildScene(const Textures& tex)
{
    run("newScene");
    Graph& g = graph();
    Node* body = addMesh(g, "Body", decoratedCube(100.0));
    setTrs(g, body, {10, 20, 30}, {15, 30, 45}, {1, 2, 1.5});
    Node* ucx = addMesh(g, "UCX_Body_00", geo::makeCube({.width = 20, .height = 20, .depth = 20}), body->id());
    setTrs(g, ucx, {0, 50, 0}, {0, 90, 0});
    Node* socket = addNode(g, "locator", "SOCKET_Top", body->id());
    setTrs(g, socket, {0, 100, 5}, {0, 0, 30});

    using namespace material;
    Node* mb = materials::create(g, "M_Body");
    g.setValue({mb->id(), kBaseColorMap, -1}, tex.base);
    g.setValue({mb->id(), kNormalMap, -1}, tex.normal);
    g.setValue({mb->id(), kOrmMap, -1}, tex.orm);
    g.setValue({mb->id(), kSpecular, -1}, 0.25);
    Node* mt = materials::create(g, "M_Trim");
    g.setValue({mt->id(), kBaseColor, -1}, glm::dvec3(0.8, 0.1, 0.2));
    g.setValue({mt->id(), kMetallic, -1}, 0.75);
    g.setValue({mt->id(), kRoughness, -1}, 0.3);
    g.setValue({mt->id(), kEmissiveColor, -1}, glm::dvec3(1.0, 0.5, 0.0));
    g.setValue({mt->id(), kEmissiveIntensity, -1}, 4.0);
    g.setValue({mt->id(), kBlendMode, -1}, int64_t(BlendMode::Masked));
    g.setValue({mt->id(), kOpacity, -1}, 0.6);
    g.setValue({mt->id(), kOpacityMaskClip, -1}, 0.4);
    g.setValue({mt->id(), kTwoSided, -1}, true);
    g.evaluate();
}

using CornerKey = std::tuple<long long, long long, long long, long long, long long, long long, long long, long long>;

long long q(double v, double step) { return std::llround(v / step); }

/** Every corner as (object-space position, uv0, normal), rounded. Topology changes (quads to triangles) don't change the set. */
std::set<CornerKey> corners(const geo::Mesh& m)
{
    std::vector<glm::vec3> computed;
    std::span<const glm::vec3> n = m.cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
    if (n.size() != m.cornerCapacity())
    {
        computed = geo::cornerNormals(m);
        n = computed;
    }
    std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(0));
    std::set<CornerKey> out;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        m.forEachFaceCorner(f, [&](geo::Index c) {
            const glm::dvec3 p = m.position(m.cornerPoint(c));
            out.insert({q(p.x, 1e-3), q(p.y, 1e-3), q(p.z, 1e-3), q(uv[c].x, 1e-4), q(uv[c].y, 1e-4), q(n[c].x, 1e-3), q(n[c].y, 1e-3),
                        q(n[c].z, 1e-3)});
        });
    }
    return out;
}

struct Snap
{
    std::string parent;
    glm::dmat4 world{1.0};
    MeshPtr mesh;
};

std::map<std::string, Snap> snapshot()
{
    Graph& g = graph();
    g.evaluate();
    std::map<std::string, Snap> out;
    for (NodeId id : g.nodes())
    {
        Node* n = g.node(id);
        if (!n->type().isA("transform"))
            continue;
        Snap s;
        NodeId p = g.parent(id);
        s.parent = p ? g.node(p)->name() : "";
        s.world = n->as<glm::dmat4>(Transform::kWorldMatrix);
        if (n->type().isA("mesh"))
            s.mesh = n->as<MeshPtr>(MeshNode::kOutMesh);
        out[n->name()] = s;
    }
    return out;
}

void compareScenes(const std::map<std::string, Snap>& a, const std::map<std::string, Snap>& b)
{
    for (const auto& [name, s] : a)
    {
        auto it = b.find(name);
        REQUIRE_MESSAGE(it != b.end(), name);
        CHECK_MESSAGE(it->second.parent == s.parent, name);
        CHECK_MESSAGE(nearMat(it->second.world, s.world, 1e-4), name);
        if (s.mesh)
        {
            REQUIRE_MESSAGE(it->second.mesh, name);
            CHECK_MESSAGE(it->second.mesh->numPoints() == s.mesh->numPoints(), name);
            CHECK_MESSAGE(corners(*it->second.mesh) == corners(*s.mesh), name);
        }
    }
}

std::vector<float> floatsOf(const cgltf_accessor* a)
{
    std::vector<float> out(a->count * cgltf_num_components(a->type));
    cgltf_accessor_unpack_floats(a, out.data(), out.size());
    return out;
}

void checkFile(const fs::path& file)
{
    Parsed p(file);
    REQUIRE(p.data);
    CHECK(std::string(p.data->asset.version) == "2.0");
    const cgltf_node* body = p.find("Body");
    REQUIRE(body);
    REQUIRE(body->mesh);
    CHECK(body->mesh->primitives_count == 2);
    CHECK(body->children_count == 2);
    const cgltf_primitive& prim = body->mesh->primitives[0];
    CHECK(attribute(prim, cgltf_attribute_type_normal));
    CHECK(attribute(prim, cgltf_attribute_type_tangent));
    CHECK(attribute(prim, cgltf_attribute_type_texcoord, 0));
    CHECK(attribute(prim, cgltf_attribute_type_texcoord, 1));
    CHECK(attribute(prim, cgltf_attribute_type_color, 0));
    // Metres in the file: the Y-up scene's 10, 20, 30 cm.
    REQUIRE(body->has_translation);
    CHECK(body->translation[0] == doctest::Approx(0.1));
    CHECK(body->translation[1] == doctest::Approx(0.2));
    CHECK(body->translation[2] == doctest::Approx(0.3));
    const cgltf_accessor* pos = attribute(prim, cgltf_attribute_type_position);
    REQUIRE(pos);
    CHECK(pos->max[0] == doctest::Approx(0.5));
    // A cube's 8 points, split by 3 normals and their UVs at each corner.
    CHECK(pos->count == 24);

    const cgltf_material* trim = p.material("M_Trim");
    REQUIRE(trim);
    CHECK(trim->pbr_metallic_roughness.metallic_factor == doctest::Approx(0.75));
    CHECK(trim->pbr_metallic_roughness.roughness_factor == doctest::Approx(0.3));
    CHECK(trim->alpha_mode == cgltf_alpha_mode_mask);
    CHECK(trim->alpha_cutoff == doctest::Approx(0.4));
    CHECK(trim->pbr_metallic_roughness.base_color_factor[3] == doctest::Approx(0.6));
    CHECK(trim->double_sided);
    CHECK(trim->has_emissive_strength);
    CHECK(trim->emissive_strength.emissive_strength == doctest::Approx(4.0));
    CHECK(trim->emissive_factor[1] == doctest::Approx(0.5));
    const cgltf_material* bodyMat = p.material("M_Body");
    REQUIRE(bodyMat);
    REQUIRE(bodyMat->pbr_metallic_roughness.base_color_texture.texture);
    REQUIRE(bodyMat->pbr_metallic_roughness.metallic_roughness_texture.texture);
    // ORM is shared as glTF's metallic-roughness and occlusion texture.
    CHECK(bodyMat->occlusion_texture.texture == bodyMat->pbr_metallic_roughness.metallic_roughness_texture.texture);
    REQUIRE(bodyMat->normal_texture.texture);
    CHECK(bodyMat->has_specular);
    CHECK(bodyMat->specular.specular_factor == doctest::Approx(0.5));
}
} // namespace

TEST_CASE("static scene round-trips through .gltf and .glb")
{
    const Textures tex = writeTextures();
    for (const char* ext : {".gltf", ".glb"})
    {
        CAPTURE(ext);
        buildScene(tex);
        const auto before = snapshot();
        const fs::path file = tempDir(std::string("static") + ext) / (std::string("scene") + ext);
        run("exportGltf", {{"args", {file.string(), "Body"}}});
        checkFile(file);
        const std::string head = readAll(file).substr(0, 4);
        if (std::string(ext) == ".glb")
            CHECK(head == "glTF");
        else
        {
            CHECK(head[0] == '{');
            CHECK(fs::exists(file.parent_path() / "scene.bin"));
            CHECK(fs::exists(file.parent_path() / "T_Body_BC.png"));
        }

        run("newScene");
        json r = run("importGltf", {{"args", {file.string()}}, {"report", true}});
        CHECK(r["errors"].empty());
        CHECK(r["roots"].size() == 1);
        compareScenes(before, snapshot());

        using namespace material;
        Node& trim = node("M_Trim");
        CHECK(trim.as<double>(kMetallic) == doctest::Approx(0.75));
        CHECK(trim.as<double>(kRoughness) == doctest::Approx(0.3));
        CHECK(trim.as<int64_t>(kBlendMode) == int64_t(BlendMode::Masked));
        CHECK(trim.as<double>(kOpacity) == doctest::Approx(0.6));
        CHECK(trim.as<double>(kOpacityMaskClip) == doctest::Approx(0.4));
        CHECK(trim.as<bool>(kTwoSided));
        const glm::dvec3 bc = trim.as<glm::dvec3>(kBaseColor);
        CHECK(bc.r == doctest::Approx(0.8));
        CHECK(bc.g == doctest::Approx(0.1));
        const glm::dvec3 e = trim.as<glm::dvec3>(kEmissiveColor) * trim.as<double>(kEmissiveIntensity);
        CHECK(e.r == doctest::Approx(4.0));
        CHECK(e.g == doctest::Approx(2.0));

        Node& body = node("M_Body");
        CHECK(body.as<double>(kSpecular) == doctest::Approx(0.25));
        CHECK(!body.as<std::string>(kBaseColorMap).empty());
        CHECK(fs::exists(fs::path(body.as<std::string>(kBaseColorMap))));
        CHECK(!body.as<std::string>(kOrmMap).empty());
        CHECK(body.as<int64_t>(kNormalFormat) == int64_t(NormalFormat::OpenGL));
        // The DirectX map was written green-flipped; imported as OpenGL it reads the same normals.
        image::Image n = image::read(fs::path(body.as<std::string>(kNormalMap)));
        REQUIRE(n.width == 4);
        CHECK(n.rgba[1] == doctest::Approx(0.75).epsilon(0.01));
        // Base color texels come back as written.
        image::Image b = image::read(fs::path(body.as<std::string>(kBaseColorMap)));
        REQUIRE(b.width == 4);
        CHECK(b.rgba[(1 * 4 + 3) * 4] == doctest::Approx(1.0).epsilon(0.01));
    }
}

TEST_CASE("embedded .gltf is one self-contained file")
{
    const Textures tex = writeTextures();
    buildScene(tex);
    const fs::path dir = tempDir("embedded");
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path file = dir / "one.gltf";
    run("exportGltf", {{"args", {file.string(), "Body"}}, {"embedImages", true}, {"embedBuffers", true}});
    size_t files = 0;
    for (const auto& e : fs::directory_iterator(dir))
        files += e.is_regular_file();
    CHECK(files == 1);
    json doc = json::parse(readAll(file));
    CHECK(doc["buffers"][0]["uri"].get<std::string>().rfind("data:", 0) == 0);
    CHECK(doc["images"][0]["uri"].get<std::string>().rfind("data:image/png;base64,", 0) == 0);
    checkFile(file);

    const auto before = snapshot();
    run("newScene");
    json r = run("importGltf", {{"args", {file.string()}}, {"report", true}});
    compareScenes(before, snapshot());
    // Embedded images land in the textures folder next to the file.
    CHECK(!r["images"].empty());
    for (const json& img : r["images"])
        CHECK(fs::exists(fs::path(img.get<std::string>())));
}

TEST_CASE("axes and units: Y-up and Z-up scenes face glTF's +Z with Y up")
{
    for (math::UpAxis up : {math::UpAxis::Y, math::UpAxis::Z})
    {
        math::setUpAxis(up);
        CAPTURE(int(up));
        run("newScene");
        Graph& g = graph();
        const math::AxisBasis& b = math::axisBasis(up);
        // A point 1 m forward, 2 m up and 3 m right of the origin, on a node 1 m up.
        geo::Mesh m = geo::makeCube({.width = 10, .height = 10, .depth = 10});
        const glm::dvec3 offset = b.forward * 100.0 + b.up * 200.0 + b.right * 300.0;
        for (geo::Index p = 0; p < m.pointCapacity(); ++p)
            m.setPosition(p, m.position(p) + offset);
        Node* n = addMesh(g, "Box", std::move(m));
        setTrs(g, n, b.up * 100.0);
        g.evaluate();
        const auto before = snapshot();

        const fs::path file = tempDir("axes") / (up == math::UpAxis::Y ? "y.glb" : "z.glb");
        run("exportGltf", {{"args", {file.string(), "Box"}}});
        {
            Parsed p(file);
            REQUIRE(p.data);
            const cgltf_node* box = p.find("Box");
            REQUIRE(box);
            CHECK(box->translation[0] == doctest::Approx(0.0));
            CHECK(box->translation[1] == doctest::Approx(1.0));
            CHECK(box->translation[2] == doctest::Approx(0.0));
            const cgltf_accessor* pos = attribute(box->mesh->primitives[0], cgltf_attribute_type_position);
            // glTF: +Z forward, +Y up, -X right.
            CHECK((pos->min[2] + pos->max[2]) * 0.5 == doctest::Approx(1.0));
            CHECK((pos->min[1] + pos->max[1]) * 0.5 == doctest::Approx(2.0));
            CHECK((pos->min[0] + pos->max[0]) * 0.5 == doctest::Approx(-3.0));
        }
        run("newScene");
        run("importGltf", {{"args", {file.string()}}});
        compareScenes(before, snapshot());

        // units= scales lengths.
        const fs::path cm = tempDir("axes") / "cm.gltf";
        run("exportGltf", {{"args", {cm.string(), "Box"}}, {"units", "cm"}});
        Parsed p(cm);
        REQUIRE(p.data);
        CHECK(p.find("Box")->translation[1] == doctest::Approx(100.0));
    }
    math::setUpAxis(math::UpAxis::Y);
}

TEST_CASE("cameras round-trip with their field of view, clipping and facing")
{
    for (math::UpAxis up : {math::UpAxis::Y, math::UpAxis::Z})
    {
        math::setUpAxis(up);
        CAPTURE(int(up));
        run("newScene");
        Graph& g = graph();
        Node* persp = addNode(g, "camera", "Shot");
        setTrs(g, persp, {100, 200, 300}, {-20, 35, 5});
        g.setValue({persp->id(), Camera::kFocalLength, -1}, 50.0);
        g.setValue({persp->id(), Camera::kNearClipPlane, -1}, 5.0);
        g.setValue({persp->id(), Camera::kFarClipPlane, -1}, 50000.0);
        Node* child = addNode(g, "locator", "Rig", persp->id());
        setTrs(g, child, {0, 0, -100});
        Node* ortho = addNode(g, "camera", "Top");
        g.setValue({ortho->id(), Camera::kOrthographic, -1}, true);
        g.setValue({ortho->id(), Camera::kOrthographicWidth, -1}, 400.0);
        g.evaluate();
        const glm::dvec2 fov = camera::angleOfView(*persp, 1.5);
        const auto before = snapshot();

        const fs::path file = tempDir("camera") / "cams.gltf";
        run("exportGltf", {{"args", {file.string(), "Shot", "Top"}}});
        {
            Parsed p(file);
            REQUIRE(p.data);
            REQUIRE(p.data->cameras_count == 2);
            const cgltf_node* shot = p.find("Shot");
            REQUIRE(shot->camera);
            CHECK(shot->camera->type == cgltf_camera_type_perspective);
            CHECK(shot->camera->data.perspective.znear == doctest::Approx(0.05));
            CHECK(shot->camera->data.perspective.zfar == doctest::Approx(500.0));
            // The camera's -Z in the file is its Lila view direction in glTF axes.
            float m[16];
            cgltf_node_transform_world(shot, m);
            const glm::dvec3 fileView(-m[8], -m[9], -m[10]);
            const glm::dvec3 lilaView = -glm::dvec3(persp->as<glm::dmat4>(Transform::kWorldMatrix)[2]);
            const glm::dvec3 expected = fbx::profileBasis(fbx::AxisProfile::Ue6, up) * glm::normalize(lilaView);
            CHECK(glm::length(glm::normalize(fileView) - expected) < 1e-5);
            const cgltf_node* top = p.find("Top");
            CHECK(top->camera->type == cgltf_camera_type_orthographic);
            CHECK(top->camera->data.orthographic.xmag == doctest::Approx(2.0));
        }
        run("newScene");
        run("importGltf", {{"args", {file.string()}}});
        compareScenes(before, snapshot());
        const glm::dvec2 fov2 = camera::angleOfView(node("Shot"), 1.5);
        CHECK(fov2.y == doctest::Approx(fov.y).epsilon(1e-4));
        CHECK(node("Shot").as<double>(Camera::kNearClipPlane) == doctest::Approx(5.0));
        CHECK(node("Shot").as<double>(Camera::kFarClipPlane) == doctest::Approx(50000.0));
        CHECK(node("Top").as<bool>(Camera::kOrthographic));
        CHECK(node("Top").as<double>(Camera::kOrthographicWidth) == doctest::Approx(400.0));
    }
    math::setUpAxis(math::UpAxis::Y);
}

TEST_CASE("the gltf translator handles .gltf and .glb")
{
    namespace tr = io::translators;
    const tr::FileTranslator* imp = tr::forPath(tr::Op::Import, "C:/x/model.GLB");
    REQUIRE(imp);
    CHECK(imp->id == "gltf");
    const tr::FileTranslator* exp = tr::forPath(tr::Op::Export, "C:/x/model.gltf");
    REQUIRE(exp);
    CHECK(exp->exportSelection.command == "exportGltf");

    buildScene(writeTextures());
    const fs::path file = tempDir("translator") / "t.glb";
    graph().select({node("Body").id()});
    run("exportFile", {{"args", {file.string()}}});
    CHECK(fs::exists(file));
    run("newScene");
    run("importFile", {{"args", {file.string()}}});
    CHECK(graph().find("Body"));
}

TEST_CASE("animated cameras and their children keep their motion in Y-up and Z-up scenes")
{
    for (math::UpAxis up : {math::UpAxis::Y, math::UpAxis::Z})
    {
        math::setUpAxis(up);
        CAPTURE(int(up));
        run("newScene");
        run("playbackOptions", {{"fps", 24.0}, {"minTime", 0.0}, {"maxTime", 1.0}, {"animationStartTime", 0.0}, {"animationEndTime", 1.0}});
        Graph& g = graph();
        Node* cam = addNode(g, "camera", "Dolly");
        Node* child = addNode(g, "locator", "Target", cam->id());
        setTrs(g, child, {0, 10, -200});
        const NodeId id = cam->id();
        anim::setKey(g, {id, Transform::kTranslate, 0}, 0.0, 0.0);
        anim::setKey(g, {id, Transform::kTranslate, 0}, 1.0, 300.0);
        anim::setKey(g, {id, Transform::kRotate, 1}, 0.0, 0.0);
        anim::setKey(g, {id, Transform::kRotate, 1}, 1.0, 60.0);
        anim::setKey(g, {id, Transform::kRotate, 0}, 0.0, -10.0);
        anim::setKey(g, {id, Transform::kRotate, 0}, 1.0, 20.0);
        auto sampleWorlds = [&] {
            std::vector<glm::dmat4> out;
            for (int f = 0; f <= 24; f += 4)
            {
                graph().setTime(f / 24.0);
                graph().evaluate();
                out.push_back(world("Dolly"));
                out.push_back(world("Target"));
            }
            graph().setTime(0.0);
            graph().evaluate();
            return out;
        };
        const std::vector<glm::dmat4> before = sampleWorlds();
        const fs::path file = tempDir("camera_anim") / (up == math::UpAxis::Y ? "y.glb" : "z.glb");
        run("exportGltf", {{"args", {file.string(), "Dolly"}}});
        run("newScene");
        run("importGltf", {{"args", {file.string()}}});
        const std::vector<glm::dmat4> after = sampleWorlds();
        REQUIRE(after.size() == before.size());
        for (size_t i = 0; i < before.size(); ++i)
        {
            CAPTURE(i);
            CHECK(nearMat(after[i], before[i], 1e-4));
        }
    }
    math::setUpAxis(math::UpAxis::Y);
}

TEST_CASE("lights round-trip through KHR_lights_punctual in Y-up and Z-up scenes")
{
    for (math::UpAxis up : {math::UpAxis::Y, math::UpAxis::Z})
    {
        math::setUpAxis(up);
        CAPTURE(int(up));
        run("newScene");
        Graph& g = graph();
        lights::Settings spot;
        spot.kind = lights::Kind::Spot;
        spot.color = {1.0, 0.5, 0.25};
        spot.intensity = 800.0;
        spot.units = lights::Units::Candela;
        spot.innerConeAngle = 15.0;
        spot.outerConeAngle = 30.0;
        spot.range = 2000.0;
        Node& key = lights::create(g, "Key", kNoNode, spot);
        setTrs(g, &key, {100, 200, 300}, {-20, 35, 5});
        Node* child = addNode(g, "locator", "Target", key.id());
        setTrs(g, child, {0, 0, -100});
        lights::Settings sun;
        sun.kind = lights::Kind::Directional;
        sun.intensity = 3.0;
        sun.units = lights::Units::Lux;
        lights::create(g, "Sun", kNoNode, sun);
        lights::Settings bulb;
        bulb.intensity = 1000.0;
        bulb.units = lights::Units::Lumens;
        lights::create(g, "Bulb", kNoNode, bulb);
        lights::Settings panel;
        panel.kind = lights::Kind::Area;
        lights::create(g, "Panel", kNoNode, panel);
        g.evaluate();
        const glm::dmat4 keyWorld = key.as<glm::dmat4>(Transform::kWorldMatrix);
        const glm::dmat4 childWorld = child->as<glm::dmat4>(Transform::kWorldMatrix);

        const fs::path file = tempDir("lights") / "lights.gltf";
        run("exportGltf", {{"args", {file.string(), "Key", "Sun", "Bulb", "Panel"}}});
        {
            Parsed p(file);
            REQUIRE(p.data);
            REQUIRE(p.data->lights_count == 3);
            const cgltf_node* k = p.find("Key");
            REQUIRE(k->light);
            CHECK(k->light->type == cgltf_light_type_spot);
            CHECK(k->light->range == doctest::Approx(20.0));
            CHECK(k->light->spot_outer_cone_angle == doctest::Approx(glm::radians(30.0)));
            float m[16];
            cgltf_node_transform_world(k, m);
            const glm::dvec3 fileAim(-m[8], -m[9], -m[10]);
            const glm::dvec3 expected = fbx::profileBasis(fbx::AxisProfile::Ue6, up) * glm::normalize(-glm::dvec3(keyWorld[2]));
            CHECK(glm::length(glm::normalize(fileAim) - expected) < 1e-5);
            CHECK(p.find("Bulb")->light->intensity == doctest::Approx(1000.0 / (4.0 * glm::pi<double>())));
            CHECK_FALSE(p.find("Panel")->light);
        }
        run("newScene");
        run("importGltf", {{"args", {file.string()}}});
        g.evaluate();
        Node& k2 = node("Key");
        CHECK(k2.type().isA("light"));
        const lights::Settings s = lights::settings(k2);
        CHECK(s.kind == lights::Kind::Spot);
        CHECK(s.units == lights::Units::Candela);
        CHECK(s.intensity == doctest::Approx(800.0));
        CHECK(s.range == doctest::Approx(2000.0));
        CHECK(s.innerConeAngle == doctest::Approx(15.0));
        CHECK(s.outerConeAngle == doctest::Approx(30.0));
        CHECK(glm::length(s.color - glm::dvec3(1.0, 0.5, 0.25)) < 1e-6);
        CHECK(s.sourceFormat == "gltf:spot");
        const glm::dmat4 w = k2.as<glm::dmat4>(Transform::kWorldMatrix);
        for (int c = 0; c < 4; ++c)
            CHECK(glm::length(glm::dvec4(w[c]) - glm::dvec4(keyWorld[c])) < 1e-4);
        const glm::dmat4 cw = node("Target").as<glm::dmat4>(Transform::kWorldMatrix);
        CHECK(glm::length(glm::dvec3(cw[3]) - glm::dvec3(childWorld[3])) < 1e-4);
        CHECK(lights::settings(node("Sun")).units == lights::Units::Lux);
    }
    math::setUpAxis(math::UpAxis::Y);
}
