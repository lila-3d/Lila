#pragma once

#include "uv/FinalMesh.h"
#include "uv/OptimizeMath.h"

#include <Eigen/Dense>

#include <span>
#include <vector>

namespace lila::uv::detail
{
/**
 * A FinalIsland's exported corner uvs as a linear map of its chart's vertex uvs, y = R x + offset, for the solvers. Each
 * Fit Surface fill is applied by its own solve, and kept as a dense block over the few chart vertices its border reads
 * for R^T S R, so that costs a dense product per fill instead of rows over every vertex the island's border reaches.
 */
class FinalOperator
{
public:
    FinalOperator(const FinalIsland& isl, size_t vertexCount);

    size_t corners() const { return mCorners; }
    size_t vertices() const { return mVertices; }
    /** y for x; offsetScale multiplies the offsets per component ((0, 0) leaves them out, (-1, 1) mirrors them). */
    void apply(std::span<const glm::dvec2> x, std::span<glm::dvec2> y, glm::dvec2 offsetScale) const;
    /** R^T g, per chart vertex. */
    void applyTransposed(std::span<const glm::dvec2> g, std::span<glm::dvec2> out) const;
    /** R^T S R for a symmetric matrix S over the corners. */
    SpMat gram(const SpMat& s) const;
    /** Per chart vertex, the sum of |weight| over every corner reading it. */
    std::vector<double> columnWeight() const;

private:
    struct Block
    {
        std::vector<Index> cols;          // chart vertices the fill's border reads
        Eigen::MatrixXd d;                // interior group x cols
        std::vector<Index> start, corners; // interior group -> local corners
    };

    const FinalIsland& mIsl;
    size_t mCorners = 0, mVertices = 0;
    SpMat mR, mRt;
    std::vector<Block> mBlocks;
};
} // namespace lila::uv::detail
