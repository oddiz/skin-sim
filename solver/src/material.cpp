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

namespace {

// The fibre strain measure E and its derivative dE/dF (see material.hpp).
struct FibreStrain {
    double E = 0.0;
    Eigen::Matrix2d dE = Eigen::Matrix2d::Zero();
};

FibreStrain fibre_strain(const Eigen::Matrix2d& F, const Fibres& f) {
    const double J = F.determinant();
    const double I1 = F.squaredNorm() + 1.0 / (J * J);
    const Eigen::Vector2d Fa = F * f.direction;
    const double I4 = Fa.squaredNorm();
    FibreStrain s;
    s.E = f.kappa * (I1 - 3.0) + (1.0 - 3.0 * f.kappa) * (I4 - 1.0);
    s.dE = 2.0 * f.kappa * half_dq(F) + 2.0 * (1.0 - 3.0 * f.kappa) * Fa * f.direction.transpose();
    return s;
}

}  // namespace

double fibre_energy_density(const Eigen::Matrix2d& F, const Fibres& f) {
    if (!(F.determinant() > 0.0)) return kInf;
    if (!(f.k1 > 0.0)) return 0.0;
    const double E = fibre_strain(F, f).E;
    if (E <= 0.0) return 0.0;  // slack fibres store nothing
    const double x = f.k2 * E * E;
    // expm1(x)/k2 -> E^2 as k2 -> 0, so the k2 = 0 limit is handled exactly.
    return (f.k2 > kMinStiffening) ? 0.5 * f.k1 * std::expm1(x) / f.k2 : 0.5 * f.k1 * E * E;
}

Eigen::Matrix2d fibre_piola(const Eigen::Matrix2d& F, const Fibres& f) {
    if (!(f.k1 > 0.0)) return Eigen::Matrix2d::Zero();
    const FibreStrain s = fibre_strain(F, f);
    if (s.E <= 0.0) return Eigen::Matrix2d::Zero();
    return f.k1 * s.E * std::exp(f.k2 * s.E * s.E) * s.dE;
}

Mat4 fibre_stiffness(const Eigen::Matrix2d& F, const Fibres& f) {
    if (!(f.k1 > 0.0)) return Mat4::Zero();
    const FibreStrain s = fibre_strain(F, f);
    if (s.E <= 0.0) return Mat4::Zero();
    const double ex = std::exp(f.k2 * s.E * s.E);
    const double dpsi = f.k1 * s.E * ex;
    const double ddpsi = f.k1 * (1.0 + 2.0 * f.k2 * s.E * s.E) * ex;

    const Eigen::Map<const Vec4> d(s.dE.data());
    // d2I1/dF2: the Neo-Hookean stiffness divided by mu/2.
    const double J = F.determinant();
    const Eigen::Matrix2d C = cofactor(F);
    const Eigen::Map<const Vec4> c(C.data());
    const double J3 = J * J * J;
    const Mat4 d2I1 = 2.0 * (Mat4::Identity() + (3.0 / (J3 * J)) * c * c.transpose() - cofactor_derivative() / J3);
    // d2I4/dF2 = 2 (a a^T (x) I): entry (i + 2j, k + 2l) = 2 delta_ik a_j a_l.
    Mat4 d2I4 = Mat4::Zero();
    for (int j = 0; j < 2; ++j)
        for (int l = 0; l < 2; ++l)
            for (int i = 0; i < 2; ++i) d2I4(i + 2 * j, i + 2 * l) = 2.0 * f.direction[j] * f.direction[l];

    return ddpsi * d * d.transpose() + dpsi * (f.kappa * d2I1 + (1.0 - 3.0 * f.kappa) * d2I4);
}

Mat4 project_psd(const Mat4& K) {
    Eigen::SelfAdjointEigenSolver<Mat4> eig(K);
    const Vec4 clipped = eig.eigenvalues().cwiseMax(0.0);
    return eig.eigenvectors() * clipped.asDiagonal() * eig.eigenvectors().transpose();
}

}  // namespace membrane
}  // namespace skinsim
