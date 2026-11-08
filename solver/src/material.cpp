#include "skinsim/material.hpp"

#include <limits>

#include <Eigen/Eigenvalues>

namespace skinsim {
namespace membrane_nh {

namespace {

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

}  // namespace

double energy_density(const Eigen::Matrix2d& F, double mu) {
    const double J = F.determinant();
    if (!(J > 0.0)) return std::numeric_limits<double>::infinity();
    // squaredNorm() of a matrix = sum of squared entries = tr(F^T F).
    return 0.5 * mu * (F.squaredNorm() + 1.0 / (J * J) - 3.0);
}

Eigen::Matrix2d first_piola(const Eigen::Matrix2d& F, double mu) {
    const double J = F.determinant();
    return mu * (F - cofactor(F) / (J * J * J));
}

Mat4 stiffness(const Eigen::Matrix2d& F, double mu) {
    const double J = F.determinant();
    const Eigen::Matrix2d C = cofactor(F);
    // Eigen::Map views existing memory as a different shape without copying:
    // the 2x2 matrix's 4 numbers (stored column by column) read as vec(C).
    const Eigen::Map<const Vec4> c(C.data());
    const double J3 = J * J * J;
    return mu * (Mat4::Identity() + (3.0 / (J3 * J)) * c * c.transpose() -
                 cofactor_derivative() / J3);
}

Mat4 project_psd(const Mat4& K) {
    Eigen::SelfAdjointEigenSolver<Mat4> eig(K);
    const Vec4 clipped = eig.eigenvalues().cwiseMax(0.0);
    return eig.eigenvectors() * clipped.asDiagonal() * eig.eigenvectors().transpose();
}

}  // namespace membrane_nh
}  // namespace skinsim
