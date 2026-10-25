#include "curve/Internal.h"

#include <algorithm>
#include <cmath>

namespace lila::curve::detail
{
int findSpan(const std::vector<double>& U, double u, int lo, int hi)
{
    if (u >= U[size_t(hi)])
    {
        int i = hi - 1;
        while (i > lo && U[size_t(i)] >= U[size_t(i) + 1])
            --i;
        return i;
    }
    if (u <= U[size_t(lo)])
        return lo;
    int a = lo, b = hi;
    while (b - a > 1)
    {
        int mid = (a + b) / 2;
        if (u < U[size_t(mid)])
            b = mid;
        else
            a = mid;
    }
    return a;
}

void basisFuns(int i, double u, int p, const std::vector<double>& U, double* N)
{
    double left[8], right[8];
    N[0] = 1.0;
    for (int j = 1; j <= p; ++j)
    {
        left[j] = u - U[size_t(i + 1 - j)];
        right[j] = U[size_t(i + j)] - u;
        double saved = 0.0;
        for (int r = 0; r < j; ++r)
        {
            double den = right[r + 1] + left[j - r];
            double temp = den != 0.0 ? N[r] / den : 0.0;
            N[r] = saved + right[r + 1] * temp;
            saved = left[j - r] * temp;
        }
        N[j] = saved;
    }
}

bool solveDense(std::vector<double>& A, int n, std::vector<glm::dvec4>& b)
{
    for (int c = 0; c < n; ++c)
    {
        int piv = c;
        double best = std::abs(A[size_t(c) * size_t(n) + size_t(c)]);
        for (int r = c + 1; r < n; ++r)
            if (double v = std::abs(A[size_t(r) * size_t(n) + size_t(c)]); v > best)
            {
                best = v;
                piv = r;
            }
        if (best < 1e-14)
            return false;
        if (piv != c)
        {
            for (int k = 0; k < n; ++k)
                std::swap(A[size_t(c) * size_t(n) + size_t(k)], A[size_t(piv) * size_t(n) + size_t(k)]);
            std::swap(b[size_t(c)], b[size_t(piv)]);
        }
        double inv = 1.0 / A[size_t(c) * size_t(n) + size_t(c)];
        for (int r = c + 1; r < n; ++r)
        {
            double f = A[size_t(r) * size_t(n) + size_t(c)] * inv;
            if (f == 0.0)
                continue;
            for (int k = c; k < n; ++k)
                A[size_t(r) * size_t(n) + size_t(k)] -= f * A[size_t(c) * size_t(n) + size_t(k)];
            b[size_t(r)] -= b[size_t(c)] * f;
        }
    }
    for (int r = n - 1; r >= 0; --r)
    {
        glm::dvec4 v = b[size_t(r)];
        for (int k = r + 1; k < n; ++k)
            v -= b[size_t(k)] * A[size_t(r) * size_t(n) + size_t(k)];
        b[size_t(r)] = v / A[size_t(r) * size_t(n) + size_t(r)];
    }
    return true;
}

std::vector<double> chordParams(const std::vector<glm::dvec3>& pts, bool closed, double alpha)
{
    size_t n = pts.size();
    std::vector<double> t(n + (closed ? 1 : 0), 0.0);
    for (size_t i = 1; i < t.size(); ++i)
        t[i] = t[i - 1] + std::pow(std::max(glm::distance(pts[i - 1], pts[i % n]), 1e-12), alpha);
    double total = t.back();
    if (total > 0.0)
        for (double& x : t)
            x /= total;
    else
        for (size_t i = 0; i < t.size(); ++i)
            t[i] = t.size() > 1 ? double(i) / double(t.size() - 1) : 0.0;
    return t;
}
} // namespace lila::curve::detail
