#pragma once

#include "curve/Spline.h"
#include "geo/Mesh.h"

#include <groomlab/guide_set.h>
#include <groomlab/scalp_mesh.h>
#include <groomlab/strand_set.h>

#include <glm/glm.hpp>

#include <memory>
#include <vector>

/** Conversions between Lila's meshes and curves and GroomLab's scalps, guides and strands (GroomLab.cpp). */
namespace lila::groom::groomLabBridge
{
using curve::Splines;
groomlab::Vec3f toVec(const glm::dvec3& p);
/** The mesh's live points in order, moved by `matrix`: the scalp's points as scalpMesh numbers them. */
std::vector<groomlab::Vec3f> worldPoints(const geo::Mesh& mesh, const glm::dmat4& matrix);
/** The mesh's faces fanned into triangles over its live points, renumbered densely and moved by `matrix`. */
std::shared_ptr<const groomlab::ScalpMesh> scalpMesh(const geo::Mesh& mesh, const glm::dmat4& matrix = glm::dmat4(1.0));
/** GroomLab strands as poly curves, a width per point and an id per curve. */
Splines toSplines(const groomlab::StrandSet& strands);
} // namespace lila::groom::groomLabBridge
