#pragma once

#include "core/Export.h"

#include "geo/Bvh.h"
#include "geo/Mesh.h"
#include "geo/Symmetry.h"

#include <glm/glm.hpp>

#include <array>
#include <optional>
#include <span>
#include <vector>

/**
 * Live relax for retopology: the mesh behaves like a sheet held on the reference surface, its faces pulling toward even
 * squares of a target size. Ticked by the UI while you drag, or run to rest in one go.
 */
namespace lila::retopo
{
using geo::Index;

struct LiveRelaxOptions
{
    /** Edge length the faces settle toward; zero or negative uses the mesh's mean edge length at the start. */
    double targetLength = 0.0;
    /** 0 keeps each face's own size and only evens its shape, 1 pulls every face to targetLength. */
    double evenSize = 0.5;
    /** 0 only evens spacing (edges as springs of one length), 1 pulls each face fully to a square (or regular polygon) of its size. */
    double squareness = 0.7;
    /** Fraction of the way to the solved position each step takes, 0..1. */
    double stiffness = 0.5;
    /** Border points stay put; otherwise they slide along the border. */
    bool pinBoundary = true;
    /** Keep points on the surface (when one is given). */
    bool project = true;
    /** Points on this plane stay on it and the result is mirror-exact across it (object space). */
    std::optional<geo::SymmetryPlane> symmetry;
    /** Settled once no point moves more than this fraction of the target length in a step. */
    double tolerance = 1e-3;
};

/**
 * Position-based shape matching on the surface: each face fits a regular polygon to its points (best rotation in its
 * plane, size blended between its own and the target), the fits are averaged per point, points move toward that average
 * weighted by their mobility, and land back on the surface. Steps that would fold a face are cut short. Parallel and
 * deterministic.
 */
class LILA_SCENE_API LiveRelax
{
public:
    LiveRelax() = default;
    /** surface, when given, is what points stay on; its mesh must be in the mesh's space. The pointer must outlive this. */
    LiveRelax(const geo::Mesh& mesh, const geo::MeshBvh* surface, const LiveRelaxOptions& opts = {});

    void setOptions(const LiveRelaxOptions& opts);
    const LiveRelaxOptions& options() const { return mOpts; }

    /** Per point capacity of the mesh it was made from. Invalid points stay where they are. */
    std::span<const glm::dvec3> positions() const { return mX; }
    void setPosition(Index p, const glm::dvec3& at);

    /** Pinned points never move. */
    void setPinned(Index p, bool on);
    /** 0..1 per point capacity: how much of each step a point takes. Empty means 1 everywhere. */
    void setMobility(std::vector<double> mobility);
    /** Pulls a point toward `at` (a drag); its neighbours follow elastically. weight 1 follows closely. */
    void setTarget(Index p, const glm::dvec3& at, double weight = 1.0);
    void clearTargets();

    /** One step. Returns the largest move, in units of the target length. */
    double step();
    /** Steps until settled or maxSteps; returns steps run. */
    int solve(int maxSteps = 200);
    bool settled() const { return mSettled; }
    void wake() { mSettled = false; }
    /** The target length in use. */
    double targetLength() const { return mLength; }

private:
    /** A target point; its pull carries the points around it along by falloff from where they were when it began. */
    struct Drag
    {
        Index point = geo::kInvalid;
    };

    void buildSymmetry();
    double mobility(Index p) const;
    bool slides(Index p) const;
    /** Slides a border point along its border, then lands it on the surface. */
    glm::dvec3 place(Index p, const glm::dvec3& at) const;
    /** Makes p and its mirror partner (or p and the plane) exact in x. */
    void symmetrize(Index p, std::vector<glm::dvec3>& x) const;
    bool folded(Index f, const std::vector<glm::dvec3>& x) const;
    /** Where p's edges, as springs of one rest length (its own mean blended toward the target), would put it. */
    glm::dvec3 spacingGoal(Index p, double evenSize) const;
    /** Each target's pull on the points around it this step, into mDrag. */
    void spreadDrag();
    /** Whether the move of the point at slot, alone, shrinks the corner it makes in face f. */
    bool shrinks(Index f, Index slot) const;

    const geo::MeshBvh* mSurface = nullptr;
    LiveRelaxOptions mOpts;
    /** Faces as runs of points; a slot (index into mFacePts) also indexes the goal buffer. */
    std::vector<Index> mFaceStart, mFacePts, mSlotFace;
    /** Per point: the slots it occupies, and its edge neighbours. */
    std::vector<Index> mPtStart, mPtSlot, mNbStart, mNb;
    /** Per sliding border point: the starting border segments it may slide over. */
    std::vector<Index> mSegStart;
    std::vector<std::array<glm::dvec3, 2>> mSegs;
    std::vector<uint8_t> mValid, mBoundary, mPinned, mOnPlane;
    std::vector<Index> mMirror;
    std::vector<glm::dvec3> mX, mTarget, mGoal, mRaw, mNext, mArea, mDrag;
    std::vector<Index> mDragged;
    std::vector<Drag> mDrags;
    /** Positions when the current drags began, and scratch distances for spreading them. */
    std::vector<glm::dvec3> mStartX;
    std::vector<double> mDist;
    std::vector<double> mTargetW, mMobility, mCut;
    double mMeanLength = 1.0;
    double mLength = 1.0;
    bool mSettled = false;
};

/** Settles a mesh in place with live relax; active (per point capacity, optional) limits which points move. */
LILA_SCENE_API void settleMesh(geo::Mesh& mesh, const geo::MeshBvh* surface, const LiveRelaxOptions& opts = {}, int maxSteps = 200,
                               std::span<const uint8_t> active = {});
} // namespace lila::retopo
