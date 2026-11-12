// Tests for the tension-field (wrinkling) relaxation of the membrane.

#include <doctest/doctest.h>

#include <cmath>

#include <Eigen/Eigenvalues>

#include "skinsim/wrinkling.hpp"

using namespace skinsim::membrane;

namespace {

using Mat2 = Eigen::Matrix2d;

Mat2 rotation(double angle) {
    Mat2 R;
    R << std::cos(angle), -std::sin(angle), std::sin(angle), std::cos(angle);
    return R;
}

Mat2 diag(double a, double b) {
    Mat2 F = Mat2::Zero();
    F(0, 0) = a;
    F(1, 1) = b;
    return F;
}

Material neo_hookean() {
    Material m;
    m.mu = 0.05;
    m.b = 0.0;
    return m;
}

// The app's skin: stiffening matrix plus dispersed fibres at an angle (so the
// wrinkles are not along a principal direction of the deformation).
Material skin() {
    Material m;
    m.mu = 0.05;
    m.b = 4.0;
    m.fibres.k1 = 0.05;
    m.fibres.k2 = 5.0;
    m.fibres.kappa = 0.15;
    m.fibres.direction = Eigen::Vector2d(std::cos(0.3), std::sin(0.3));
    return m;
}

// Deformations that wrinkle: stretched one way, squeezed the other, rotated
// and sheared so nothing lines up with the axes.
std::vector<Mat2> wrinkled_samples() {
    std::vector<Mat2> out;
    for (double a : {0.0, 0.5, 1.3, 2.2}) {
        Mat2 U;
        U << 1.18, 0.06, 0.02, 0.82;
        out.push_back(rotation(a) * U * rotation(-0.4 * a));
    }
    return out;
}

double flat_energy(const Mat2& F, const Material& m) {
    return energy_density(F, m.mu, m.b) + fibre_energy_density(F, m.fibres);
}
Mat2 flat_piola(const Mat2& F, const Material& m) {
    return first_piola(F, m.mu, m.b) + fibre_piola(F, m.fibres);
}
Mat4 flat_stiffness(const Mat2& F, const Material& m) {
    return stiffness(F, m.mu, m.b) + fibre_stiffness(F, m.fibres);
}

}  // namespace

TEST_CASE("the material written in terms of C matches the one written in F") {
    const Material m = skin();
    for (const Mat2& F : wrinkled_samples()) {
        const Mat2 C = F.transpose() * F;
        const MetricDerivatives d = metric_derivatives(C, m);
        CHECK(d.W == doctest::Approx(flat_energy(F, m)).epsilon(1e-12));
        CHECK((2.0 * F * d.S - flat_piola(F, m)).norm() < 1e-12);
        // dW/dC and d2W/dC2 against finite differences (symmetric changes of C).
        const double h = 1e-6;
        for (int i = 0; i < 2; ++i)
            for (int j = i; j < 2; ++j) {
                Mat2 E = Mat2::Zero();
                E(i, j) = E(j, i) = 1.0;
                const MetricDerivatives p = metric_derivatives(C + h * E, m), q = metric_derivatives(C - h * E, m);
                const double fd = (p.W - q.W) / (2 * h);
                CHECK((d.S.cwiseProduct(E)).sum() == doctest::Approx(fd).epsilon(1e-6));
                const Mat2 dS = (p.S - q.S) / (2 * h);
                const Eigen::Map<const Vec4> e(E.data());
                const Vec4 Ke = d.K * e;
                const Eigen::Map<const Vec4> fdS(dS.data());
                for (int r = 0; r < 4; ++r) CHECK(Ke[r] == doctest::Approx(fdS[r]).epsilon(1e-5).scale(1e-6));
            }
    }
}

TEST_CASE("taut skin: the relaxed membrane is the plain membrane") {
    const Material m = skin();
    Mat2 F;
    F << 1.15, 0.05, -0.02, 1.08;  // stretched both ways
    const RelaxedState s = relax(F, m);
    CHECK(s.state == MembraneState::Taut);
    CHECK(relaxed_energy_density(F, m) == doctest::Approx(flat_energy(F, m)).epsilon(1e-14));
    CHECK((relaxed_piola(F, m) - flat_piola(F, m)).norm() < 1e-14);
    CHECK((relaxed_stiffness(F, m) - flat_stiffness(F, m)).norm() < 1e-12);
    CHECK(excess_length(s) == 0.0);
    // The unstressed state is taut too (it keeps its stiffness).
    CHECK(relax(Mat2::Identity(), m).state == MembraneState::Taut);
}

TEST_CASE("uniaxial strip: wrinkles take up the squeeze, stress is purely uniaxial") {
    // A Neo-Hookean strip stretched to 1.2 and squeezed to 0.8 across. A flat
    // membrane pushes back across it; a wrinkled one carries only the
    // tension, exactly like a free strip pulled to 1.2, whose natural width is
    // 1/sqrt(1.2) = 0.913: the 0.913 - 0.8 = 0.113 in between is hidden in
    // wrinkles, an excess of 14%.
    const Material m = neo_hookean();
    const double l = 1.2;
    const Mat2 F = diag(l, 0.8);
    const RelaxedState s = relax(F, m);
    REQUIRE(s.state == MembraneState::Wrinkled);
    const Mat2 sigma = relaxed_piola(F, m) * F.transpose();  // Cauchy stress (incompressible)
    CHECK(sigma(0, 0) == doctest::Approx(m.mu * (l * l - 1.0 / l)).epsilon(1e-10));
    CHECK(std::abs(sigma(1, 1)) < 1e-12);
    CHECK(std::abs(sigma(0, 1)) < 1e-12);
    CHECK(relaxed_energy_density(F, m) == doctest::Approx(0.5 * m.mu * (l * l + 2.0 / l - 3.0)).epsilon(1e-10));
    CHECK(excess_length(s) == doctest::Approx(1.0 / std::sqrt(l) / 0.8 - 1.0).epsilon(1e-9));
    // The flat membrane, for comparison: compression across.
    CHECK((flat_piola(F, m) * F.transpose())(1, 1) < -0.4 * m.mu);
}

TEST_CASE("slack skin: squeezed every way, it lies in folds and carries nothing") {
    const Material m = skin();
    const Mat2 F = rotation(0.7) * diag(0.92, 0.8);
    const RelaxedState s = relax(F, m);
    REQUIRE(s.state == MembraneState::Slack);
    CHECK(relaxed_energy_density(F, m) == 0.0);
    CHECK(relaxed_piola(F, m).norm() == 0.0);
    CHECK(relaxed_stiffness(F, m).norm() == 0.0);
    CHECK(excess_length(s) == doctest::Approx(0.25).epsilon(1e-12));
    // Collapsed or inverted elements stay forbidden.
    CHECK(relaxed_energy_density(diag(-0.5, 0.8), m) == std::numeric_limits<double>::infinity());
}

TEST_CASE("anisotropic wrinkles: optimal, never compressive, and frame invariant") {
    const Material m = skin();
    for (const Mat2& F : wrinkled_samples()) {
        const RelaxedState s = relax(F, m);
        REQUIRE(s.state == MembraneState::Wrinkled);
        const MetricDerivatives d = metric_derivatives(s.C_eff, m);
        // Optimality: no stress across the wrinkles, tension along them.
        CHECK((d.S * s.w).norm() < 1e-10 * m.mu * s.w.norm());
        Eigen::SelfAdjointEigenSolver<Mat2> se(d.S);
        CHECK(se.eigenvalues()[0] > -1e-10 * m.mu);
        CHECK(se.eigenvalues()[1] > 0.0);
        // Never compressive (Cauchy stress), stiffness never negative.
        Eigen::SelfAdjointEigenSolver<Mat2> ce(relaxed_piola(F, m) * F.transpose());
        CHECK(ce.eigenvalues()[0] > -1e-10 * m.mu);
        Eigen::SelfAdjointEigenSolver<Mat4> ke(relaxed_stiffness(F, m));
        CHECK(ke.eigenvalues()[0] > -1e-9 * ke.eigenvalues()[3]);
        // Less energy than the flat membrane, the same after a rigid rotation.
        CHECK(relaxed_energy_density(F, m) < flat_energy(F, m));
        CHECK(relaxed_energy_density(rotation(0.9) * F, m) ==
              doctest::Approx(relaxed_energy_density(F, m)).epsilon(1e-10));
        CHECK(excess_length(s) > 0.0);
    }
}

TEST_CASE("relaxed stress and stiffness are the derivatives of the relaxed energy") {
    for (const Material& m : {neo_hookean(), skin()}) {
        for (const Mat2& F : wrinkled_samples()) {
            const Mat2 P = relaxed_piola(F, m);
            const Mat4 K = relaxed_stiffness(F, m);
            const double h = 1e-6;
            for (int col = 0; col < 4; ++col) {
                Mat2 Fp = F, Fm = F;
                Fp.data()[col] += h;
                Fm.data()[col] -= h;
                const double fd = (relaxed_energy_density(Fp, m) - relaxed_energy_density(Fm, m)) / (2 * h);
                CHECK(P.data()[col] == doctest::Approx(fd).epsilon(1e-6).scale(1e-6));
                const Mat2 dP = (relaxed_piola(Fp, m) - relaxed_piola(Fm, m)) / (2 * h);
                for (int row = 0; row < 4; ++row)
                    CHECK(K(row, col) == doctest::Approx(dP.data()[row]).epsilon(1e-5).scale(1e-6));
            }
        }
    }
}

