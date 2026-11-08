// Tests for the incompressible (strain-stiffening) membrane law.

#include <doctest/doctest.h>

#include <cmath>

#include <Eigen/Eigenvalues>

#include "skinsim/material.hpp"

using namespace skinsim::membrane;

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

// Uniaxial tension of a free strip: transverse stretch 1/sqrt(lambda), so
// that width and thickness share the incompressibility equally.
Eigen::Matrix2d uniaxial(double lambda) {
    Eigen::Matrix2d F = Eigen::Matrix2d::Zero();
    F(0, 0) = lambda;
    F(1, 1) = 1.0 / std::sqrt(lambda);
    return F;
}
}  // namespace

TEST_CASE("undeformed state: zero energy, zero stress") {
    const Eigen::Matrix2d I = Eigen::Matrix2d::Identity();
    for (double b : {0.0, 8.0}) {
        CHECK(energy_density(I, kMu, b) == doctest::Approx(0.0));
        CHECK(first_piola(I, kMu, b).norm() == doctest::Approx(0.0));
    }
}

TEST_CASE("rigid rotation stores no energy (frame invariance)") {
    for (double b : {0.0, 8.0}) {
        for (double angle : {0.3, 1.0, 2.5, -1.2}) {
            CHECK(energy_density(rotation(angle), kMu, b) == doctest::Approx(0.0).epsilon(1e-12));
            // Rotating a deformed state must not change its energy either.
            const Eigen::Matrix2d F = sample_F();
            CHECK(energy_density(rotation(angle) * F, kMu, b) ==
                  doctest::Approx(energy_density(F, kMu, b)).epsilon(1e-12));
        }
    }
}

TEST_CASE("inverted or collapsed elements have infinite energy") {
    Eigen::Matrix2d F;
    F << 1.0, 0.0, 0.0, -0.5;  // mirror image: J < 0
    CHECK(std::isinf(energy_density(F, kMu, 8.0)));
    F << 1.0, 0.0, 0.0, 0.0;  // flattened to a line: J = 0
    CHECK(std::isinf(energy_density(F, kMu, 0.0)));
}

TEST_CASE("uniaxial tension matches the analytic solution") {
    // Textbook results (free sides, so P22 = 0):
    //   Neo-Hookean:  P11 = mu (lambda - 1/lambda^2)
    //   Exponential:  P11 = exp(b Q) mu (lambda - 1/lambda^2),  Q = lambda^2 + 2/lambda - 3
    for (double b : {0.0, 8.0}) {
        for (double lambda : {1.1, 1.5, 2.0, 0.8}) {
            const Eigen::Matrix2d P = first_piola(uniaxial(lambda), kMu, b);
            const double Q = lambda * lambda + 2.0 / lambda - 3.0;
            const double expected = std::exp(b * Q) * kMu * (lambda - 1.0 / (lambda * lambda));
            CHECK(P(0, 0) == doctest::Approx(expected));
            // "Zero" relative to the stress magnitude (exp(bQ) can be ~1e7 here).
            CHECK(std::abs(P(1, 1)) <= 1e-12 * std::abs(expected));
            CHECK(P(0, 1) == doctest::Approx(0.0));
            CHECK(P(1, 0) == doctest::Approx(0.0));
        }
    }
}

TEST_CASE("stiffening: J-shaped curve, and b -> 0 recovers Neo-Hookean") {
    // The tangent stiffness dP11/dlambda grows much faster with stiffening.
    auto tangent = [](double lambda, double b) {
        const double h = 1e-6;
        return (first_piola(uniaxial(lambda + h), kMu, b)(0, 0) -
                first_piola(uniaxial(lambda - h), kMu, b)(0, 0)) / (2 * h);
    };
    const double nh_ratio = tangent(1.5, 0.0) / tangent(1.0, 0.0);
    const double exp_ratio = tangent(1.5, 8.0) / tangent(1.0, 8.0);
    CHECK(nh_ratio < 1.0);      // Neo-Hookean actually *softens* in uniaxial tension
    CHECK(exp_ratio > 50.0);    // the exponential model locks up (here ~250x stiffer)
    // At small strain, both models agree: same initial stiffness.
    CHECK(tangent(1.0, 8.0) == doctest::Approx(tangent(1.0, 0.0)).epsilon(1e-6));
    // Continuity: a tiny b gives (nearly) the Neo-Hookean energy.
    const Eigen::Matrix2d F = sample_F();
    CHECK(energy_density(F, kMu, 1e-9) == doctest::Approx(energy_density(F, kMu, 0.0)).epsilon(1e-8));
}

TEST_CASE("absurd stretch with stiffening overflows to infinity, not NaN") {
    const double W = energy_density(uniaxial(50.0), kMu, 100.0);
    CHECK(std::isinf(W));
    CHECK(W > 0.0);
}

TEST_CASE("stress is the derivative of energy (finite differences)") {
    // Central difference: dW/dF_ij ~ (W(F + h e_ij) - W(F - h e_ij)) / 2h.
    // Error ~ h^2, so h = 1e-6 gives ~1e-12 truncation and a few 1e-10 of roundoff.
    for (double b : {0.0, 8.0}) {
        const Eigen::Matrix2d F = sample_F();
        const Eigen::Matrix2d P = first_piola(F, kMu, b);
        const double h = 1e-6;
        for (int i = 0; i < 2; ++i) {
            for (int j = 0; j < 2; ++j) {
                Eigen::Matrix2d Fp = F, Fm = F;
                Fp(i, j) += h;
                Fm(i, j) -= h;
                const double fd = (energy_density(Fp, kMu, b) - energy_density(Fm, kMu, b)) / (2 * h);
                CHECK(P(i, j) == doctest::Approx(fd).epsilon(1e-6));
            }
        }
    }
}

TEST_CASE("stiffness is the derivative of stress (finite differences)") {
    for (double b : {0.0, 8.0}) {
        const Eigen::Matrix2d F = sample_F();
        const Mat4 K = stiffness(F, kMu, b);
        CHECK((K - K.transpose()).norm() < 1e-12 * K.norm());  // second derivatives commute
        const double h = 1e-6;
        for (int col = 0; col < 4; ++col) {
            Eigen::Matrix2d Fp = F, Fm = F;
            Fp.data()[col] += h;  // .data()[k] walks entries in vec() order
            Fm.data()[col] -= h;
            const Eigen::Matrix2d dP = (first_piola(Fp, kMu, b) - first_piola(Fm, kMu, b)) / (2 * h);
            const Eigen::Map<const Vec4> fd(dP.data());
            for (int row = 0; row < 4; ++row) {
                CHECK(K(row, col) == doctest::Approx(fd[row]).epsilon(1e-6));
            }
        }
    }
}

TEST_CASE("PSD projection: keeps PSD matrices, removes negative directions") {
    const Mat4 K = stiffness(Eigen::Matrix2d::Identity(), kMu, 0.0);  // PSD at rest
    CHECK((project_psd(K) - K).norm() < 1e-12);

    // Under strong compression the exact stiffness can be indefinite.
    Eigen::Matrix2d F;
    F << 0.4, 0.0, 0.0, 1.0;
    Eigen::SelfAdjointEigenSolver<Mat4> before(stiffness(F, kMu, 0.0));
    Eigen::SelfAdjointEigenSolver<Mat4> after(project_psd(stiffness(F, kMu, 0.0)));
    CHECK(after.eigenvalues().minCoeff() >= -1e-12);
    // (Documenting the reason the projection exists:)
    CHECK(before.eigenvalues().minCoeff() < 0.0);
}
