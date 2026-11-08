#include "skinsim/material.hpp"

#include <cmath>
#include <limits>

#include <Eigen/Eigenvalues>

namespace skinsim {
namespace membrane {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

// Below this, b is treated as exactly 0 (pure Neo-Hookean), avoiding a 0/0.
constexpr double kMinStiffening = 1e-12;

// cof(F) = J F^-T. For a 2x2 matrix it's just a rearrangement of entries:
//   F = [a b; c d]  ->  cof = [d -c; -b a]
Eigen::Matrix2d cofactor(const Eigen::Matrix2d& F) {
    Eigen::Matrix2d C;
    C << F(1, 1), -F(1, 0),
        -F(0, 1), F(0, 0);
    return C;
}

// Hc: d(vec cof F)/d(vec F). Constant, because cof F is linear in F in 2D.
// With vecF = [F00, F10, F01, F11] and vec cof = [F11, -F01, -F10, F00]:
Mat4 cofactor_derivative() {
    Mat4 H;
    H << 0, 0, 0, 1,
         0, 0, -1, 0,
         0, -1, 0, 0,
         1, 0, 0, 0;
    return H;
}

// dQ/dF / 2 = F - J^-3 cof(F). Equal to P_nh / mu, but computed without
// dividing by mu, so it stays exact even when mu is tiny.
Eigen::Matrix2d half_dq(const Eigen::Matrix2d& F) {
    const double J = F.determinant();
    return F - cofactor(F) / (J * J * J);
}

// exp(bQ): how many times stiffer than Neo-Hookean the tissue currently is.
double stiffening_factor(double Q, double b) {
    return (b > kMinStiffening) ? std::exp(b * Q) : 1.0;
}

}  // namespace

double invariant_q(const Eigen::Matrix2d& F) {
    const double J = F.determinant();
    if (!(J > 0.0)) return kInf;
    // squaredNorm() of a matrix = sum of squared entries = tr(F^T F).
    return F.squaredNorm() + 1.0 / (J * J) - 3.0;
}

double energy_density(const Eigen::Matrix2d& F, double mu, double b) {
    const double Q = invariant_q(F);
    if (Q == kInf) return kInf;
    if (b <= kMinStiffening) return 0.5 * mu * Q;
    // expm1(x) = exp(x) - 1, computed accurately even when x is tiny (where
    // exp(x) - 1 would lose most digits to cancellation). Overflows to +inf for
    // absurd stretches, which the line search then treats as "not allowed".
    return 0.5 * mu * std::expm1(b * Q) / b;
}

Eigen::Matrix2d first_piola(const Eigen::Matrix2d& F, double mu, double b) {
    return stiffening_factor(invariant_q(F), b) * mu * half_dq(F);
}

Mat4 stiffness(const Eigen::Matrix2d& F, double mu, double b) {
    const double J = F.determinant();
    const Eigen::Matrix2d C = cofactor(F);
    // Eigen::Map views existing memory as a different shape without copying:
    // the 2x2 matrix's 4 numbers (stored column by column) read as vec(C).
    const Eigen::Map<const Vec4> c(C.data());
    const double J3 = J * J * J;
    Mat4 K = mu * (Mat4::Identity() + (3.0 / (J3 * J)) * c * c.transpose() - cofactor_derivative() / J3);

    if (b > kMinStiffening) {
        // (2b/mu) p p^T with p = mu * q  ==  2 b mu q q^T
        const Eigen::Matrix2d q_mat = half_dq(F);
        const Eigen::Map<const Vec4> q(q_mat.data());
        K += 2.0 * b * mu * q * q.transpose();
        K *= stiffening_factor(invariant_q(F), b);
    }
    return K;
}

Mat4 project_psd(const Mat4& K) {
    Eigen::SelfAdjointEigenSolver<Mat4> eig(K);
    const Vec4 clipped = eig.eigenvalues().cwiseMax(0.0);
    return eig.eigenvectors() * clipped.asDiagonal() * eig.eigenvectors().transpose();
}

}  // namespace membrane
}  // namespace skinsim
