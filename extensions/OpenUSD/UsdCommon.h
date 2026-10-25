#pragma once

#include <lila/Interchange.h>
#include <lila/Math.h>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/vt/dictionary.h>
#include <pxr/usd/usd/prim.h>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

/**
 * Shared by the USD reader and writer.
 *
 * Axes. USD stages are right-handed and declare Y or Z up. Lila reads and writes them the way its FBX translator does:
 * a Z-up stage is Unreal's space (Lila forward +X, right -Y, up +Z, the ue5 profile) and a Y-up stage the ue6 one
 * (forward +Z, right -X, up +Y), so an asset lands identically through either format. metersPerUnit scales lengths;
 * Lila is centimetres.
 *
 * Lila keeps a few things USD has no schema for (material settings, camera film fit, LOD screen sizes, the prims it
 * adds only to satisfy USD) in a "lila" customData dictionary, so they survive a round trip and other tools ignore them.
 */
namespace lila::usd
{
namespace fs = std::filesystem;
using json = nlohmann::json;

/** Lila space and file space for one stage. */
struct Axes
{
    bool zUp = true;
    double cmPerUnit = 1.0;
    /** Lila axes to file axes, a rotation. */
    glm::dmat3 basis{1.0};
    fbx::AxisMap map;
    /** Lila cm to file units and axes, and back. */
    glm::dmat4 toFile{1.0}, toLila{1.0};

    glm::dvec3 pointToFile(const glm::dvec3& p) const { return glm::dvec3(toFile * glm::dvec4(p, 1.0)); }
    glm::dvec3 pointToLila(const glm::dvec3& p) const { return glm::dvec3(toLila * glm::dvec4(p, 1.0)); }
    /** A local or world matrix carried across: conjugated by the basis and the unit scale. */
    glm::dmat4 matrixToFile(const glm::dmat4& m) const { return toFile * m * toLila; }
    glm::dmat4 matrixToLila(const glm::dmat4& m) const { return toLila * m * toFile; }

    /** Transform channels across the axis permutation: file to Lila (lengths in cm), and the rotate order. */
    glm::dvec3 translateToLila(const glm::dvec3& v) const
    {
        glm::dvec3 out;
        for (int i = 0; i < 3; ++i)
            out[i] = map.sign[i] * v[map.to[i]] * cmPerUnit;
        return out;
    }
    glm::dvec3 rotateToLila(const glm::dvec3& v) const
    {
        glm::dvec3 out;
        for (int i = 0; i < 3; ++i)
            out[i] = map.sign[i] * map.det * v[map.to[i]];
        return out;
    }
    glm::dvec3 scaleToLila(const glm::dvec3& v) const
    {
        glm::dvec3 out;
        for (int i = 0; i < 3; ++i)
            out[i] = v[map.to[i]];
        return out;
    }
    scene::RotateOrder orderToLila(scene::RotateOrder fileOrder) const
    {
        for (int o = 0; o < 6; ++o)
            if (map.order(scene::RotateOrder(o)) == fileOrder)
                return scene::RotateOrder(o);
        return scene::RotateOrder::XYZ;
    }
};

Axes makeAxes(bool fileZUp, double metersPerUnit);

glm::dmat4 toGlm(const pxr::GfMatrix4d& m);
pxr::GfMatrix4d toGf(const glm::dmat4& m);

/** A valid prim or node name: letters, digits and underscores, not starting with a digit. */
std::string identifier(std::string_view raw, const char* fallback);

fs::path utf8Path(const std::string& s);
std::string utf8(const fs::path& p);

/** The prim's "lila" customData dictionary, empty when it has none. */
pxr::VtDictionary lilaData(const pxr::UsdObject& obj);
void setLilaData(const pxr::UsdObject& obj, const std::string& key, const pxr::VtValue& value);
bool lilaFlag(const pxr::UsdObject& obj, const std::string& key);

/** Translation, rotation (degrees, in order) and scale of a matrix, Euler angles near ref; sheared when it had shear. */
struct Trs
{
    glm::dvec3 t{0.0}, r{0.0}, s{1.0};
    bool sheared = false;
};
Trs decompose(const glm::dmat4& m, scene::RotateOrder order, const glm::dvec3& ref, const glm::dmat3& pre = glm::dmat3(1.0));

bool nearMatrix(const glm::dmat4& a, const glm::dmat4& b, double eps = 1e-7);

/** Collects OpenUSD's errors and warnings while alive, instead of letting them reach stderr. */
class Diagnostics
{
public:
    Diagnostics();
    ~Diagnostics();
    Diagnostics(const Diagnostics&) = delete;
    Diagnostics& operator=(const Diagnostics&) = delete;

    /** Errors raised so far, cleared from USD's error list. */
    std::vector<std::string> errors();
    /** Warnings so far, each distinct one once. */
    std::vector<std::string> warnings();

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

/**
 * Registers the USD plugins deployed in the package (its usd and plugin/usd folders) before any stage is opened.
 * Safe to call more than once. False, with why, when the core plugins can't be found.
 */
bool registerPlugins(const fs::path& packageRoot, std::string* error);
} // namespace lila::usd
