#pragma once

#include <Eigen/Core>
#include <glm/glm.hpp>

namespace lila::deform
{
/** A square glm matrix as the Eigen matrix of the same size. */
template <glm::length_t N, glm::qualifier Q> Eigen::Matrix<double, N, N> toEigen(const glm::mat<N, N, double, Q>& m)
{
    Eigen::Matrix<double, N, N> e;
    for (int c = 0; c < N; ++c)
        for (int r = 0; r < N; ++r)
            e(r, c) = m[c][r];
    return e;
}

/** A square fixed-size Eigen matrix or block as the glm matrix of the same size. */
template <class Derived> glm::mat<Derived::RowsAtCompileTime, Derived::ColsAtCompileTime, double> toGlm(const Eigen::MatrixBase<Derived>& e)
{
    constexpr int N = Derived::RowsAtCompileTime;
    static_assert(N == Derived::ColsAtCompileTime && N > 0, "toGlm needs a fixed-size square matrix");
    glm::mat<N, N, double> m;
    for (int c = 0; c < N; ++c)
        for (int r = 0; r < N; ++r)
            m[c][r] = e(r, c);
    return m;
}
} // namespace lila::deform
