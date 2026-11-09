#pragma once

#include "uv/FinalMesh.h"
#include "uv/OptimizeMath.h"

#include <Eigen/Dense>
#include <Eigen/SparseCholesky>

#include <complex>
#include <span>
#include <utility>
#include <vector>

namespace lila::uv::detail
{
using CSpMat = Eigen::SparseMatrix<std::complex<double>>;

/**
 * A FinalIsland's exported corner uvs as a linear map of its chart's vertex uvs, y = R x + offset, for the solvers. Each
 * Fit Surface fill is applied by its own solve, and kept as a dense block over the few chart vertices its border reads
 * for R^H S R, so that costs a dense product per fill instead of rows over every vertex the island's border reaches.
 * Weights that rotate make R complex (uvs read as u + iv); an island whose weights all scale only stays real.
 */
class FinalOperator
{
public:
    /** Conjugate reads the island in a frame mirrored in u (weights conjugated), as solves of mirrored islands do. */
    FinalOperator(const FinalIsland& isl, size_t vertexCount, bool conjugate = false);

    size_t corners() const { return mCorners; }
    size_t vertices() const { return mVertices; }
    /** True when some weight rotates, so u and v couple and solves need the complex system. */
    bool rotates() const { return mRotates; }
    /** y for x; offsetScale multiplies the offsets per component ((0, 0) leaves them out, (-1, 1) mirrors them). */
    void apply(std::span<const glm::dvec2> x, std::span<glm::dvec2> y, glm::dvec2 offsetScale) const;
    /** R^H g, per chart vertex. */
    void applyTransposed(std::span<const glm::dvec2> g, std::span<glm::dvec2> out) const;
    /** R^H S R for a symmetric matrix S over the corners: its real part, and its imaginary part (empty unless rotates()). */
    std::pair<SpMat, SpMat> gram(const SpMat& s) const;
    /** Per chart vertex, the sum of |weight| over every corner reading it. */
    std::vector<double> columnWeight() const;

private:
    struct Block
    {
        std::vector<Index> cols;           // chart vertices the fill's border reads
        Eigen::MatrixXd d;                 // interior group x cols (real weights)
        Eigen::MatrixXcd dc;               // the same when the weights rotate
        std::vector<Index> start, corners; // interior group -> local corners
    };
    FinalWeight weight(Index k) const { return mConjugate ? std::conj(mIsl.weights[k]) : mIsl.weights[k]; }
    template <class T>
    Eigen::SparseMatrix<T> gramOf(const SpMat& s, const Eigen::SparseMatrix<T>& r) const;

    const FinalIsland& mIsl;
    size_t mCorners = 0, mVertices = 0;
    bool mConjugate = false, mRotates = false;
    SpMat mR;
    CSpMat mRc;
    std::vector<Block> mBlocks;
};

/**
 * The LDLT of a gram matrix from a FinalOperator, real or (when the weights rotate) complex Hermitian, solving u and v
 * together: real systems solve each on its own, complex ones as u + iv.
 */
class UvFactor
{
public:
    /** Factors re + i im (im empty for a real system). */
    bool compute(const SpMat& re, const SpMat& im);
    bool ok() const { return mOk; }
    bool solve(const Eigen::VectorXd& bu, const Eigen::VectorXd& bv, Eigen::VectorXd& su, Eigen::VectorXd& sv) const;

private:
    bool mComplex = false, mOk = false;
    Eigen::SimplicialLDLT<SpMat> mReal;
    Eigen::SimplicialLDLT<CSpMat> mCx;
};
} // namespace lila::uv::detail
