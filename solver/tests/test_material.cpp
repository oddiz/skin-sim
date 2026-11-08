// Tests for the incompressible Neo-Hookean membrane law.

#include <doctest/doctest.h>

#include <cmath>

#include <Eigen/Eigenvalues>

#include "skinsim/material.hpp"

using namespace skinsim::membrane_nh;

namespace {
constexpr double kMu = 0.05;  // MPa

Eigen::Matrix2d rotation(double angle) {
    Eigen::Matrix2d R;
    R << std::cos(angle), -std::sin(angle), std::sin(angle), std::cos(angle);
    return R;
}

// A generic, non-symmetric deformation with J > 0.
Eigen::Matrix2d sample_F() {
    Eigen::Matrix2d F;
    F << 1.3, 0.2, -0.15, 0.85;
    return F;
}
}  // namespace

TEST_CASE("undeformed state: zero energy, zero stress") {
    const Eigen::Matrix2d I = Eigen::Matrix2d::Identity();
    CHECK(energy_density(I, kMu) == doctest::Approx(0.0));
    CHECK(first_piola(I, kMu).norm() == doctest::Approx(0.0));
}

TEST_CASE("rigid rotation stores no energy (frame invariance)") {
    for (double angle : {0.3, 1.0, 2.5, -1.2}) {
        CHECK(energy_density(rotation(angle), kMu) == doctest::Approx(0.0).epsilon(1e-12));
        // Rotating a deformed state must not change its energy either.
        const Eigen::Matrix2d F = sample_F();
        CHECK(energy_density(rotation(angle) * F, kMu) ==
              doctest::Approx(energy_density(F, kMu)).epsilon(1e-12));
    }
}

TEST_CASE("inverted or collapsed elements have infinite energy") {
    Eigen::Matrix2d F;
    F << 1.0, 0.0, 0.0, -0.5;  // mirror image: J < 0
    CHECK(std::isinf(energy_density(F, kMu)));
    F << 1.0, 0.0, 0.0, 0.0;  // flattened to a line: J = 0
    CHECK(std::isinf(energy_density(F, kMu)));
}

TEST_CASE("uniaxial tension matches the analytic solution") {
    // Pull a free strip to stretch lambda. With free sides, the transverse
    // stretch settles at 1/sqrt(lambda) (incompressibility shared between
    // width and thickness). Textbook result for the nominal stress:
    //     P11 = mu (lambda - 1/lambda^2),   P22 = 0 (free sides).
    for (double lambda : {1.1, 1.5, 2.0, 0.8}) {
        Eigen::Matrix2d F = Eigen::Matrix2d::Zero();
        F(0, 0) = lambda;
        F(1, 1) = 1.0 / std::sqrt(lambda);
        const Eigen::Matrix2d P = first_piola(F, kMu);
        CHECK(P(0, 0) == doctest::Approx(kMu * (lambda - 1.0 / (lambda * lambda))));
        CHECK(P(1, 1) == doctest::Approx(0.0).epsilon(1e-12));
        CHECK(P(0, 1) == doctest::Approx(0.0));
        CHECK(P(1, 0) == doctest::Approx(0.0));
    }
}

TEST_CASE("stress is the derivative of energy (finite differences)") {
    // Central difference: dW/dF_ij ~ (W(F + h e_ij) - W(F - h e_ij)) / 2h.
    // Error ~ h^2, so h = 1e-6 gives ~1e-12 truncation and a few 1e-10 of roundoff.
    const Eigen::Matrix2d F = sample_F();
    const Eigen::Matrix2d P = first_piola(F, kMu);
    const double h = 1e-6;
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2; ++j) {
            Eigen::Matrix2d Fp = F, Fm = F;
            Fp(i, j) += h;
            Fm(i, j) -= h;
            const double fd = (energy_density(Fp, kMu) - energy_density(Fm, kMu)) / (2 * h);
            CHECK(P(i, j) == doctest::Approx(fd).epsilon(1e-6));
        }
    }
}

TEST_CASE("stiffness is the derivative of stress (finite differences)") {
    const Eigen::Matrix2d F = sample_F();
    const Mat4 K = stiffness(F, kMu);
    CHECK((K - K.transpose()).norm() < 1e-14);  // second derivatives commute
    const double h = 1e-6;
    for (int col = 0; col < 4; ++col) {
        Eigen::Matrix2d Fp = F, Fm = F;
        Fp.data()[col] += h;  // .data()[k] walks entries in vec() order
        Fm.data()[col] -= h;
        const Eigen::Matrix2d dP = (first_piola(Fp, kMu) - first_piola(Fm, kMu)) / (2 * h);
        const Eigen::Map<const Vec4> fd(dP.data());
        for (int row = 0; row < 4; ++row) {
            CHECK(K(row, col) == doctest::Approx(fd[row]).epsilon(1e-6));
        }
    }
}

TEST_CASE("PSD projection: keeps PSD matrices, removes negative directions") {
    const Mat4 K = stiffness(Eigen::Matrix2d::Identity(), kMu);  // PSD at rest
    CHECK((project_psd(K) - K).norm() < 1e-12);

    // Under strong compression the exact stiffness can be indefinite.
    Eigen::Matrix2d F;
    F << 0.4, 0.0, 0.0, 1.0;
    Eigen::SelfAdjointEigenSolver<Mat4> before(stiffness(F, kMu));
    Eigen::SelfAdjointEigenSolver<Mat4> after(project_psd(stiffness(F, kMu)));
    CHECK(after.eigenvalues().minCoeff() >= -1e-12);
    // (Documenting the reason the projection exists:)
    CHECK(before.eigenvalues().minCoeff() < 0.0);
}
