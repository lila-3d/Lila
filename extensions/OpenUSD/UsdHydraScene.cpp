#include "UsdHydraScene.h"

#include "UsdCommon.h"
#include "UsdMaterials.h"

#include <lila/Interchange.h>
#include <lila/Lights.h>
#include <lila/Mesh.h>
#include <lila/MeshOps.h>
#include <lila/Node.h>
#include <lila/Profile.h>
#include <lila/Transform.h>

#include <pxr/base/gf/range3d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/imaging/hd/cameraSchema.h>
#include <pxr/imaging/hd/extentSchema.h>
#include <pxr/imaging/hd/geomSubsetSchema.h>
#include <pxr/imaging/hd/light.h>
#include <pxr/imaging/hd/lightSchema.h>
#include <pxr/imaging/hd/materialBindingSchema.h>
#include <pxr/imaging/hd/materialBindingsSchema.h>
#include <pxr/imaging/hd/meshSchema.h>
#include <pxr/imaging/hd/meshTopologySchema.h>
#include <pxr/imaging/hd/overlayContainerDataSource.h>
#include <pxr/imaging/hd/primvarSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/sceneIndexPrimView.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/visibilitySchema.h>
#include <pxr/imaging/hd/xformSchema.h>
#include <pxr/imaging/pxOsd/tokens.h>
#include <pxr/usd/sdf/assetPath.h>

#include <format>
#include <functional>

PXR_NAMESPACE_USING_DIRECTIVE

namespace lila::usd::hydra
{
namespace
{
using scene::Graph;
using scene::NodeId;

template <class T> typename HdRetainedTypedSampledDataSource<T>::Handle retained(const T& v)
{
    return HdRetainedTypedSampledDataSource<T>::New(v);
}

HdContainerDataSourceHandle container(const std::vector<std::pair<TfToken, HdDataSourceBaseHandle>>& entries)
{
    std::vector<TfToken> names;
    std::vector<HdDataSourceBaseHandle> values;
    for (const auto& [n, v] : entries)
        if (v)
        {
            names.push_back(n);
            values.push_back(v);
        }
    return HdRetainedContainerDataSource::New(names.size(), names.data(), values.data());
}

HdContainerDataSourceHandle xformSource(const glm::dmat4& m)
{
    return HdXformSchema::Builder().SetMatrix(retained(toGf(m))).SetResetXformStack(retained(true)).Build();
}

HdContainerDataSourceHandle bindingSource(const SdfPath& material)
{
    // Every purpose, since delegates ask for theirs (Storm preview, final-frame renderers full) without falling back.
    const HdDataSourceBaseHandle binding = HdMaterialBindingSchema::Builder().SetPath(retained(material)).Build();
    const TfToken purposes[3] = {HdMaterialBindingsSchemaTokens->allPurpose, HdTokens->preview, HdTokens->full};
    const HdDataSourceBaseHandle bindings[3] = {binding, binding, binding};
    return HdMaterialBindingsSchema::BuildRetained(3, purposes, bindings);
}

HdDataSourceBaseHandle primvar(const VtValue& value, const TfToken& interpolation, const TfToken& role)
{
    return HdPrimvarSchema::Builder()
        .SetPrimvarValue(HdRetainedSampledDataSource::New(value))
        .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(interpolation))
        .SetRole(role.IsEmpty() ? nullptr : HdPrimvarSchema::BuildRoleDataSource(role))
        .Build();
}

size_t mix(size_t h, size_t v) { return h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2)); }
size_t hashOf(double v) { return std::hash<double>()(v); }
size_t hashOf(const glm::dvec3& v) { return mix(mix(hashOf(v.x), hashOf(v.y)), hashOf(v.z)); }

/** A UsdLux-style light: sphere (points and spots, shaped by a cone), distant, rect, disk or dome. */
std::pair<TfToken, HdContainerDataSourceHandle> lightSource(const scene::Node& n, size_t& hash)
{
    using scene::lights::Kind;
    const scene::lights::Settings l = scene::lights::settings(n);
    hash = mix(mix(mix(size_t(l.kind), hashOf(l.color)), hashOf(l.intensity)), mix(hashOf(l.sourceRadius), hashOf(l.width)));
    hash = mix(mix(hash, hashOf(l.height)), mix(hashOf(l.outerConeAngle), hashOf(l.innerConeAngle)));
    hash = mix(mix(hash, std::hash<std::string>()(l.texture)), mix(size_t(l.castShadows), mix(size_t(l.useTemperature), hashOf(l.temperature))));
    std::vector<std::pair<TfToken, HdDataSourceBaseHandle>> p = {
        {HdLightTokens->intensity, retained(float(l.intensity))},
        {HdLightTokens->exposure, retained(0.0f)},
        {HdLightTokens->color, retained(GfVec3f(float(l.color.x), float(l.color.y), float(l.color.z)))},
        {HdLightTokens->enableColorTemperature, retained(l.useTemperature)},
        {HdLightTokens->colorTemperature, retained(float(l.temperature))},
        {HdLightTokens->normalize, retained(false)},
        {HdLightTokens->diffuse, retained(1.0f)},
        {HdLightTokens->specular, retained(1.0f)},
        {HdLightTokens->shadowEnable, retained(l.castShadows)},
    };
    TfToken type;
    switch (l.kind)
    {
    case Kind::Point:
    case Kind::Spot:
        type = HdPrimTypeTokens->sphereLight;
        p.push_back({HdLightTokens->radius, retained(float(l.sourceRadius))});
        p.push_back({TfToken("treatAsPoint"), retained(l.sourceRadius <= 0.0)});
        if (l.kind == Kind::Spot)
        {
            p.push_back({HdLightTokens->shapingConeAngle, retained(float(l.outerConeAngle))});
            p.push_back({HdLightTokens->shapingConeSoftness,
                         retained(float(l.outerConeAngle > 0.0 ? 1.0 - l.innerConeAngle / l.outerConeAngle : 0.0))});
        }
        break;
    case Kind::Directional: type = HdPrimTypeTokens->distantLight; break;
    case Kind::Area:
        type = HdPrimTypeTokens->rectLight;
        p.push_back({HdLightTokens->width, retained(float(l.width))});
        p.push_back({HdLightTokens->height, retained(float(l.height))});
        break;
    case Kind::Disc:
        type = HdPrimTypeTokens->diskLight;
        p.push_back({HdLightTokens->radius, retained(float(l.width * 0.5))});
        break;
    case Kind::Dome:
        type = HdPrimTypeTokens->domeLight;
        if (!l.texture.empty())
            p.push_back({HdLightTokens->textureFile, retained(SdfAssetPath(l.texture, l.texture))});
        break;
    }
    return {type, container(p)};
}

/** Hydra's apertures and focal length are in scene units: Lila's inches and mm as cm. */
HdContainerDataSourceHandle cameraSource(const scene::Node& n, size_t& hash)
{
    using scene::Camera;
    const bool ortho = n.as<bool>(Camera::kOrthographic);
    const double hfa = n.as<double>(Camera::kHorizontalFilmAperture), vfa = n.as<double>(Camera::kVerticalFilmAperture);
    const double focal = n.as<double>(Camera::kFocalLength), width = n.as<double>(Camera::kOrthographicWidth);
    const double nearClip = n.as<double>(Camera::kNearClipPlane), farClip = n.as<double>(Camera::kFarClipPlane);
    hash = mix(mix(mix(size_t(ortho), hashOf(hfa)), mix(hashOf(vfa), hashOf(focal))), mix(hashOf(width), mix(hashOf(nearClip), hashOf(farClip))));
    const float h = float(ortho ? width : hfa * 2.54);
    const float v = float(ortho ? width * vfa / std::max(hfa, 1e-6) : vfa * 2.54);
    return HdCameraSchema::Builder()
        .SetProjection(HdCameraSchema::BuildProjectionDataSource(ortho ? HdCameraSchemaTokens->orthographic : HdCameraSchemaTokens->perspective))
        .SetHorizontalAperture(retained(h))
        .SetVerticalAperture(retained(v))
        .SetFocalLength(retained(float(focal * 0.1)))
        .SetClippingRange(retained(GfVec2f(float(nearClip), float(farClip))))
        .Build();
}

void addEntry(HdSceneIndexObserver::AddedPrimEntries& out, const SdfPath& p, const TfToken& type) { out.emplace_back(p, type); }
} // namespace

SdfPath stageRoot(NodeId stageNode) { return SdfPath(std::format("/Stages/S{}", stageNode)); }

bool drawnByLila(const scene::Node& n) { return n.type().isA("mesh") || n.type().isA("camera") || n.type().isA("light"); }

// ---------------------------------------------------------------- LilaSceneIndex

struct LilaSceneIndex::MeshData
{
    HdContainerDataSourceHandle mesh, primvars, extent;
    VtIntArray indices;
    /** Triangles per section, when there are several; each section's material slot. */
    std::vector<VtIntArray> subsets;
    std::vector<std::string> slots;
    bool sameTopology(const MeshData& o) const { return indices == o.indices && subsets == o.subsets && slots == o.slots; }
};

std::shared_ptr<const LilaSceneIndex::MeshData> LilaSceneIndex::meshData(const scene::MeshPtr& mesh)
{
    if (auto it = mMeshes.find(mesh.get()); it != mMeshes.end())
        return it->second.second;
    LILA_ZONE();
    geo::RenderMeshOptions o;
    o.wireframe = false;
    const geo::RenderMesh rm = geo::buildRenderMesh(*mesh, o);
    auto d = std::make_shared<MeshData>();
    const size_t nv = rm.positions.size();
    VtVec3fArray points(nv), normals(nv);
    GfRange3d bounds;
    for (size_t i = 0; i < nv; ++i)
    {
        points[i] = GfVec3f(rm.positions[i].x, rm.positions[i].y, rm.positions[i].z);
        normals[i] = GfVec3f(rm.normals[i].x, rm.normals[i].y, rm.normals[i].z);
        bounds.UnionWith(GfVec3d(points[i]));
    }
    d->indices.resize(rm.indices.size());
    for (size_t i = 0; i < rm.indices.size(); ++i)
        d->indices[i] = int(rm.indices[i]);
    const VtIntArray counts(rm.indices.size() / 3, 3);

    std::vector<std::pair<TfToken, HdDataSourceBaseHandle>> pv = {
        {HdTokens->points, primvar(VtValue(points), HdPrimvarSchemaTokens->vertex, HdPrimvarSchemaTokens->point)},
        {HdTokens->normals, primvar(VtValue(normals), HdPrimvarSchemaTokens->vertex, HdPrimvarSchemaTokens->normal)},
    };
    for (size_t s = 0; s < rm.uvs.size(); ++s)
    {
        VtVec2fArray uv(rm.uvs[s].size());
        for (size_t i = 0; i < uv.size(); ++i)
            uv[i] = GfVec2f(rm.uvs[s][i].x, rm.uvs[s][i].y);
        pv.push_back({TfToken(uvPrimvarName(int(s))), primvar(VtValue(uv), HdPrimvarSchemaTokens->vertex, HdPrimvarSchemaTokens->textureCoordinate)});
    }
    if (rm.colors.size() == nv)
    {
        VtVec3fArray color(nv);
        VtFloatArray opacity(nv);
        for (size_t i = 0; i < nv; ++i)
        {
            color[i] = GfVec3f(rm.colors[i].r, rm.colors[i].g, rm.colors[i].b);
            opacity[i] = rm.colors[i].a;
        }
        pv.push_back({HdTokens->displayColor, primvar(VtValue(color), HdPrimvarSchemaTokens->vertex, HdPrimvarSchemaTokens->color)});
        pv.push_back({HdTokens->displayOpacity, primvar(VtValue(opacity), HdPrimvarSchemaTokens->vertex, TfToken())});
    }
    d->primvars = container(pv);

    for (const geo::RenderSection& s : rm.sections)
        d->slots.push_back(scene::materials::slotName(*mesh, s.material));
    if (d->slots.empty())
        d->slots.push_back(scene::materials::slotName(*mesh, 0));
    if (rm.sections.size() > 1)
        for (const geo::RenderSection& s : rm.sections)
        {
            VtIntArray tris(s.indexCount / 3);
            for (size_t t = 0; t < tris.size(); ++t)
                tris[t] = int(s.firstIndex / 3 + t);
            d->subsets.push_back(std::move(tris));
        }

    d->mesh = HdMeshSchema::Builder()
                  .SetTopology(HdMeshTopologySchema::Builder()
                                   .SetFaceVertexCounts(retained(counts))
                                   .SetFaceVertexIndices(retained(d->indices))
                                   .SetOrientation(HdMeshTopologySchema::BuildOrientationDataSource(HdMeshTopologySchemaTokens->rightHanded))
                                   .Build())
                  .SetSubdivisionScheme(retained(PxOsdOpenSubdivTokens->none))
                  .SetDoubleSided(retained(true))
                  .Build();
    if (!bounds.IsEmpty())
        d->extent = HdExtentSchema::Builder().SetMin(retained(bounds.GetMin())).SetMax(retained(bounds.GetMax())).Build();
    mMeshes[mesh.get()] = {mesh, d};
    return d;
}

HdContainerDataSourceHandle LilaSceneIndex::nodeSource(const Node& n) const
{
    std::vector<std::pair<TfToken, HdDataSourceBaseHandle>> e = {
        {HdXformSchema::GetSchemaToken(), xformSource(n.world)},
        {HdVisibilitySchema::GetSchemaToken(), HdVisibilitySchema::BuildRetained(retained(n.visible))},
    };
    if (n.mesh)
    {
        e.push_back({HdMeshSchema::GetSchemaToken(), n.mesh->mesh});
        e.push_back({HdPrimvarsSchema::GetSchemaToken(), n.mesh->primvars});
        e.push_back({HdExtentSchema::GetSchemaToken(), n.mesh->extent});
        if (n.bindings.size() == 1)
            e.push_back({HdMaterialBindingsSchema::GetSchemaToken(), bindingSource(n.bindings[0])});
    }
    else if (n.type == HdPrimTypeTokens->camera)
        e.push_back({HdCameraSchema::GetSchemaToken(), n.own});
    else
        e.push_back({HdLightSchemaTokens->light, n.own});
    return container(e);
}

void LilaSceneIndex::put(const SdfPath& path, Prim prim)
{
    mPrims[path] = std::move(prim);
    for (SdfPath p = path; !p.IsAbsoluteRootPath(); p = p.GetParentPath())
        if (!mChildren[p.GetParentPath()].insert(p).second)
            break;
}

void LilaSceneIndex::drop(const SdfPath& path)
{
    for (auto it = mPrims.lower_bound(path); it != mPrims.end() && it->first.HasPrefix(path);)
        it = mPrims.erase(it);
    for (auto it = mChildren.lower_bound(path); it != mChildren.end() && it->first.HasPrefix(path);)
        it = mChildren.erase(it);
    if (auto parent = mChildren.find(path.GetParentPath()); parent != mChildren.end())
        parent->second.erase(path);
}

HdSceneIndexPrim LilaSceneIndex::GetPrim(const SdfPath& path) const
{
    if (auto it = mPrims.find(path); it != mPrims.end())
        return {it->second.type, it->second.ds};
    return {TfToken(), nullptr};
}

SdfPathVector LilaSceneIndex::GetChildPrimPaths(const SdfPath& path) const
{
    if (auto it = mChildren.find(path); it != mChildren.end())
        return SdfPathVector(it->second.begin(), it->second.end());
    return {};
}

void LilaSceneIndex::sync(Graph& g, const std::function<SdfPath(const std::string&)>& bind)
{
    LILA_ZONE();
    mCounts = {};
    HdSceneIndexObserver::AddedPrimEntries added;
    HdSceneIndexObserver::RemovedPrimEntries removed;
    HdSceneIndexObserver::DirtiedPrimEntries dirtied;
    std::unordered_map<NodeId, Node> next;
    std::set<const void*> usedMeshes;
    mSlots.clear();
    mLights = 0;

    if (!mPrims.count(nodesRoot()))
    {
        put(nodesRoot(), {TfToken(), nullptr});
        addEntry(added, nodesRoot(), TfToken());
    }

    for (NodeId id : g.nodes())
    {
        const scene::Node* sn = g.node(id);
        if (!sn || !drawnByLila(*sn))
            continue;
        Node n;
        n.path = nodesRoot().AppendChild(TfToken(std::format("N{}", id)));
        n.world = sn->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        n.visible = scene::effectivelyVisible(g, id);
        if (sn->type().isA("mesh"))
        {
            const scene::MeshPtr& m = sn->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
            if (!m || m->empty())
                continue;
            n.type = HdPrimTypeTokens->mesh;
            n.mesh = meshData(m);
            usedMeshes.insert(m.get());
            for (const std::string& slot : n.mesh->slots)
            {
                mSlots.insert(slot);
                n.bindings.push_back(bind(slot));
            }
        }
        else if (sn->type().isA("camera"))
        {
            n.type = HdPrimTypeTokens->camera;
            n.own = cameraSource(*sn, n.settings);
        }
        else
        {
            std::tie(n.type, n.own) = lightSource(*sn, n.settings);
            ++mLights;
        }

        auto was = mNodes.find(id);
        const bool fresh = was == mNodes.end() || was->second.type != n.type ||
                           (n.mesh && (was->second.mesh != n.mesh && !was->second.mesh->sameTopology(*n.mesh))) ||
                           was->second.bindings != n.bindings;
        HdDataSourceLocatorSet locators;
        if (!fresh)
        {
            const Node& o = was->second;
            if (o.world != n.world)
                locators.insert(HdXformSchema::GetDefaultLocator());
            if (o.visible != n.visible)
                locators.insert(HdVisibilitySchema::GetDefaultLocator());
            if (o.mesh != n.mesh)
            {
                locators.insert(HdPrimvarsSchema::GetDefaultLocator());
                locators.insert(HdExtentSchema::GetDefaultLocator());
            }
            if (o.settings != n.settings)
                locators.insert(n.type == HdPrimTypeTokens->camera ? HdCameraSchema::GetDefaultLocator()
                                                                    : HdDataSourceLocator(HdLightSchemaTokens->light));
            if (locators.IsEmpty())
            {
                n.own = o.own;
                next.emplace(id, std::move(n));
                continue;
            }
        }
        else if (was != mNodes.end())
        {
            // Re-added from scratch: its old geometry subsets go first.
            drop(was->second.path);
            removed.emplace_back(was->second.path);
        }
        put(n.path, {n.type, nodeSource(n)});
        if (fresh)
        {
            addEntry(added, n.path, n.type);
            ++mCounts.added;
            if (n.mesh && n.mesh->subsets.size() > 1)
                for (size_t s = 0; s < n.mesh->subsets.size(); ++s)
                {
                    const SdfPath sub = n.path.AppendChild(TfToken(std::format("S{}", s)));
                    put(sub, {HdPrimTypeTokens->geomSubset,
                              container({{HdGeomSubsetSchema::GetSchemaToken(),
                                          HdGeomSubsetSchema::Builder()
                                              .SetType(HdGeomSubsetSchema::BuildTypeDataSource(HdGeomSubsetSchemaTokens->typeFaceSet))
                                              .SetIndices(retained(n.mesh->subsets[s]))
                                              .Build()},
                                         {HdMaterialBindingsSchema::GetSchemaToken(), bindingSource(n.bindings[s])}})});
                    addEntry(added, sub, HdPrimTypeTokens->geomSubset);
                }
        }
        else
        {
            dirtied.emplace_back(n.path, locators);
            ++mCounts.dirtied;
        }
        next.emplace(id, std::move(n));
    }

    for (auto& [id, n] : mNodes)
        if (!next.count(id))
        {
            drop(n.path);
            removed.emplace_back(n.path);
            ++mCounts.removed;
        }
    mNodes = std::move(next);
    for (auto it = mMeshes.begin(); it != mMeshes.end();)
        it = usedMeshes.count(it->first) ? std::next(it) : mMeshes.erase(it);

    if (!removed.empty())
        _SendPrimsRemoved(removed);
    if (!added.empty())
        _SendPrimsAdded(added);
    if (!dirtied.empty())
        _SendPrimsDirtied(dirtied);
}

// ---------------------------------------------------------------- MaterialLibrary

MaterialLibrary::MaterialLibrary() : mStage(UsdStage::CreateInMemory("lilaMaterials.usda")) {}

SdfPath MaterialLibrary::bind(const std::string& slot) const
{
    return materialsRoot().AppendChild(TfToken("Materials")).AppendChild(TfToken(identifier(slot, "Material")));
}

void MaterialLibrary::changed(Graph& g, scene::Change c, NodeId id)
{
    using scene::Change;
    if (c == Change::Reset || c == Change::Cleared)
    {
        mAllStale = true;
        return;
    }
    const scene::Node* n = g.node(id);
    if (c == Change::Deleted || c == Change::Renamed || c == Change::Created)
    {
        if (!n || n->type().isA("material"))
            mAllStale = true;
        return;
    }
    if (n && n->type().isA("material") && (c == Change::Value || c == Change::Connection || c == Change::Attrs))
        mStale.insert(n->name());
}

void MaterialLibrary::sync(Graph& g, const std::set<std::string>& used)
{
    LILA_ZONE();
    std::vector<std::string> write;
    for (const std::string& slot : used)
        if (mAllStale || !mWritten.count(slot) || mStale.count(slot))
            write.push_back(slot);
    mAllStale = false;
    mStale.clear();
    if (write.empty())
        return;
    const SdfPath scope("/Materials");
    MaterialWriter w(g, mStage, scope, fs::temp_directory_path() / "lilaHydraMaterials.usda", TextureMode::Absolute);
    for (const std::string& slot : write)
    {
        mStage->RemovePrim(scope.AppendChild(TfToken(identifier(slot, "Material"))));
        w.material(slot);
        mWritten.insert(slot);
    }
}

// ---------------------------------------------------------------- StageOverrides

bool StageOverrides::insidePruned(const SdfPath& path) const
{
    for (const SdfPath& p : mPruned)
        if (path != p && path.HasPrefix(p))
            return true;
    return false;
}

HdSceneIndexPrim StageOverrides::GetPrim(const SdfPath& path) const
{
    if (insidePruned(path))
        return {TfToken(), nullptr};
    HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(path);
    const auto own = mOwned.find(path);
    const bool pruned = mPruned.count(path) > 0;
    if (own == mOwned.end() && !pruned)
        return prim;
    HdContainerDataSourceHandle placed;
    if (own != mOwned.end())
        placed = container({{HdXformSchema::GetSchemaToken(), xformSource(own->second.matrix)},
                            {HdVisibilitySchema::GetSchemaToken(), HdVisibilitySchema::BuildRetained(retained(own->second.visible))}});
    if (pruned || own->second.blank)
    {
        // Left as a transform: no geometry, instancing or anything else the stage gave it.
        HdDataSourceBaseHandle xf = placed ? HdDataSourceBaseHandle() : (prim.dataSource ? prim.dataSource->Get(HdXformSchema::GetSchemaToken()) : nullptr);
        return {TfToken(), placed ? placed : container({{HdXformSchema::GetSchemaToken(), xf}})};
    }
    prim.dataSource = HdOverlayContainerDataSource::New(placed, prim.dataSource);
    return prim;
}

SdfPathVector StageOverrides::GetChildPrimPaths(const SdfPath& path) const
{
    if (mPruned.count(path) || insidePruned(path))
        return {};
    return _GetInputSceneIndex()->GetChildPrimPaths(path);
}

void StageOverrides::_PrimsAdded(const HdSceneIndexBase&, const HdSceneIndexObserver::AddedPrimEntries& entries)
{
    HdSceneIndexObserver::AddedPrimEntries out;
    out.reserve(entries.size());
    for (const auto& e : entries)
    {
        if (insidePruned(e.primPath))
            continue;
        const auto own = mOwned.find(e.primPath);
        const bool blank = mPruned.count(e.primPath) || (own != mOwned.end() && own->second.blank);
        out.emplace_back(e.primPath, blank ? TfToken() : e.primType);
    }
    if (!out.empty())
        _SendPrimsAdded(out);
}

void StageOverrides::_PrimsRemoved(const HdSceneIndexBase&, const HdSceneIndexObserver::RemovedPrimEntries& entries) { _SendPrimsRemoved(entries); }

void StageOverrides::_PrimsDirtied(const HdSceneIndexBase&, const HdSceneIndexObserver::DirtiedPrimEntries& entries)
{
    if (mPruned.empty())
    {
        _SendPrimsDirtied(entries);
        return;
    }
    HdSceneIndexObserver::DirtiedPrimEntries out;
    for (const auto& e : entries)
        if (!insidePruned(e.primPath))
            out.push_back(e);
    if (!out.empty())
        _SendPrimsDirtied(out);
}

void StageOverrides::update(std::map<SdfPath, Override> owned, std::set<SdfPath> pruned)
{
    if (owned == mOwned && pruned == mPruned)
        return;
    HdSceneIndexObserver::RemovedPrimEntries removed;
    // Prims whose placement or type changed, re-added with everything below them: UsdImaging's flattening keeps the
    // descendants' world transforms until they are re-added, whatever was dirtied.
    std::set<SdfPath> subtrees;
    for (const SdfPath& p : pruned)
        if (!mPruned.count(p))
        {
            for (const SdfPath& c : GetChildPrimPaths(p))
                removed.emplace_back(c);
            subtrees.insert(p);
        }
    for (const SdfPath& p : mPruned)
        if (!pruned.count(p))
            subtrees.insert(p);
    for (const auto& [p, o] : owned)
        if (const auto was = mOwned.find(p); was == mOwned.end() || !(was->second == o))
            subtrees.insert(p);
    for (const auto& [p, o] : mOwned)
        if (!owned.count(p))
            subtrees.insert(p);

    mOwned = std::move(owned);
    mPruned = std::move(pruned);

    HdSceneIndexObserver::AddedPrimEntries added;
    std::set<SdfPath> done;
    std::function<void(const SdfPath&)> subtree = [&](const SdfPath& p) {
        if (!done.insert(p).second)
            return;
        added.emplace_back(p, GetPrim(p).primType);
        for (const SdfPath& c : GetChildPrimPaths(p))
            subtree(c);
    };
    for (const SdfPath& root : subtrees)
        if (!insidePruned(root))
            subtree(root);
    if (!removed.empty())
        _SendPrimsRemoved(removed);
    if (!added.empty())
        _SendPrimsAdded(added);
}

// ---------------------------------------------------------------- StageToWorld

HdSceneIndexPrim StageToWorld::GetPrim(const SdfPath& path) const
{
    HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(path);
    if (!prim.dataSource)
        return prim;
    glm::dmat4 m(1.0);
    if (HdXformSchema xf = HdXformSchema::GetFromParent(prim.dataSource))
        if (HdMatrixDataSourceHandle ds = xf.GetMatrix())
            m = toGlm(ds->GetTypedValue(0.0f));
    prim.dataSource = HdOverlayContainerDataSource::New(container({{HdXformSchema::GetSchemaToken(), xformSource(mToWorld * m)}}), prim.dataSource);
    return prim;
}

SdfPathVector StageToWorld::GetChildPrimPaths(const SdfPath& path) const { return _GetInputSceneIndex()->GetChildPrimPaths(path); }

void StageToWorld::_PrimsAdded(const HdSceneIndexBase&, const HdSceneIndexObserver::AddedPrimEntries& entries) { _SendPrimsAdded(entries); }
void StageToWorld::_PrimsRemoved(const HdSceneIndexBase&, const HdSceneIndexObserver::RemovedPrimEntries& entries) { _SendPrimsRemoved(entries); }
void StageToWorld::_PrimsDirtied(const HdSceneIndexBase&, const HdSceneIndexObserver::DirtiedPrimEntries& entries) { _SendPrimsDirtied(entries); }

void StageToWorld::setToWorld(const glm::dmat4& m)
{
    if (m == mToWorld)
        return;
    mToWorld = m;
    HdSceneIndexObserver::DirtiedPrimEntries dirtied;
    for (const SdfPath& p : HdSceneIndexPrimView(_GetInputSceneIndex()))
        dirtied.emplace_back(p, HdDataSourceLocatorSet{HdXformSchema::GetDefaultLocator()});
    if (!dirtied.empty())
        _SendPrimsDirtied(dirtied);
}
} // namespace lila::usd::hydra
