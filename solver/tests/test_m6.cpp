// Tests for M6: GOH collagen fibres, in-vivo prestretch, undermining.

#include <doctest/doctest.h>

#include <cmath>
#include <random>

#include "skinsim/material.hpp"
#include "skinsim/sheet.hpp"

using namespace skinsim;
using membrane::Fibres;

namespace {

using V = Eigen::Vector2d;

const SheetGeometry kGeometry{100.0, 60.0, 2.5};

Fibres fibres(double kappa, double k2, double angle) {
    Fibres f;
    f.k1 = 0.1;
    f.k2 = k2;
    f.kappa = kappa;
    f.direction = V(std::cos(angle), std::sin(angle));
    return f;
}

void settle(Simulation& sim) {
    for (int frame = 0; frame < 300; ++frame)
        if (sim.step(20).converged) return;
}

// Largest distance between two vertices sharing a rest position (lip gap).
double max_gap(const Sheet& s) {
    const TriMesh& m = s.mesh();
    const auto x = s.simulation().positions();
    double gap = 0.0;
    for (const Edge& e : m.cut_edges)
        for (int v : e)
            for (int w = 0; w < m.vertex_count(); ++w)
                if (w != v && (m.rest_positions.col(w) - m.rest_positions.col(v)).norm() < 1e-9)
                    gap = std::max(gap, (x.col(w) - x.col(v)).norm());
    return gap;
}

// Illustrative in-vivo skin: stiffening matrix, fibres and prestretch along
// horizontal RSTL (angle 0).
TissueParams living_skin() {
    TissueParams p;
    p.fibre_angle = 0.0;
    p.fibre_stiffness = 0.05;
    p.fibre_nonlinearity = 5.0;
    p.fibre_dispersion = 0.15;
    p.prestretch_along = 1.10;
    p.prestretch_across = 1.04;
    return p;
}

}  // namespace

// ---------------------------------------------------------------------------
// GOH fibre law
// ---------------------------------------------------------------------------

TEST_CASE("fibre stress and stiffness match finite differences") {
    const Eigen::Matrix2d Fs[] = {
        (Eigen::Matrix2d() << 1.25, 0.1, -0.05, 0.9).finished(),
        (Eigen::Matrix2d() << 1.1, 0.3, 0.2, 1.15).finished(),
    };
    for (const Eigen::Matrix2d& F : Fs)
        for (double kappa : {0.0, 0.1, 0.25})
            for (double k2 : {0.0, 5.0})
                for (double angle : {0.0, 0.7}) {
                    CAPTURE(kappa);
                    CAPTURE(k2);
                    CAPTURE(angle);
                    const Fibres f = fibres(kappa, k2, angle);
                    REQUIRE(membrane::fibre_energy_density(F, f) > 0.0);  // fibres taut in these states
                    const Eigen::Matrix2d P = membrane::fibre_piola(F, f);
                    const membrane::Mat4 K = membrane::fibre_stiffness(F, f);
                    const double h = 1e-6;
                    for (int i = 0; i < 4; ++i) {
                        Eigen::Matrix2d Fp = F, Fm = F;
                        Fp.data()[i] += h;  // data()[i] is vec(F)[i] (column-major)
                        Fm.data()[i] -= h;
                        const double dW = (membrane::fibre_energy_density(Fp, f) -
                                           membrane::fibre_energy_density(Fm, f)) / (2 * h);
                        CHECK(P.data()[i] == doctest::Approx(dW).epsilon(1e-6).scale(1e-4));
                        const Eigen::Matrix2d dP = (membrane::fibre_piola(Fp, f) - membrane::fibre_piola(Fm, f)) / (2 * h);
                        for (int j = 0; j < 4; ++j)
                            CHECK(K(j, i) == doctest::Approx(dP.data()[j]).epsilon(1e-6).scale(1e-4));
                    }
                    CHECK((K - K.transpose()).norm() < 1e-12);
                }
}

TEST_CASE("fibres carry no compression") {
    // Uniaxial compression along the fibres, perfectly aligned (kappa = 0):
    // E = I4 - 1 < 0, so the fibres are slack.
    Eigen::Matrix2d F = Eigen::Matrix2d::Zero();
    F(0, 0) = 0.9;
    F(1, 1) = 1.0 / std::sqrt(0.9);
    const Fibres f = fibres(0.0, 5.0, 0.0);
    CHECK(membrane::fibre_energy_density(F, f) == 0.0);
    CHECK(membrane::fibre_piola(F, f).norm() == 0.0);
    CHECK(membrane::fibre_stiffness(F, f).norm() == 0.0);
}

TEST_CASE("fully dispersed fibres (kappa = 1/3) are isotropic; any fibres are frame invariant") {
    const Eigen::Matrix2d F = (Eigen::Matrix2d() << 1.2, 0.1, -0.1, 0.95).finished();
    const double W0 = membrane::fibre_energy_density(F, fibres(1.0 / 3.0, 5.0, 0.0));
    for (double angle : {0.4, 1.2, 2.0}) CHECK(membrane::fibre_energy_density(F, fibres(1.0 / 3.0, 5.0, angle)) == doctest::Approx(W0));
    Eigen::Matrix2d R;
    R << std::cos(0.8), -std::sin(0.8), std::sin(0.8), std::cos(0.8);
    const Fibres f = fibres(0.1, 5.0, 0.3);
    CHECK(membrane::fibre_energy_density(R * F, f) == doctest::Approx(membrane::fibre_energy_density(F, f)));
}

// ---------------------------------------------------------------------------
// Assembled simulation with everything switched on
// ---------------------------------------------------------------------------

TEST_CASE("with fibres, prestretch and undermining: gradient and Hessian match finite differences") {
    Sheet s(SheetGeometry{24.0, 16.0, 4.0});
    Simulation& sim = s.simulation();
    TissueParams p = living_skin();
    p.fibre_angle = 25.0;  // not aligned with the mesh or the axes
    p.pin_boundary = false;
    sim.set_params(p);
    REQUIRE(s.undermine_at(V(2, 1), 6.0, true));

    std::mt19937 rng(11);
    std::uniform_real_distribution<double> noise(-0.4, 0.4);
    Eigen::VectorXd x = sim.dofs();
    for (int i = 0; i < x.size(); ++i) x[i] += noise(rng);
    REQUIRE(std::isfinite(sim.energy(x)));
    const Eigen::VectorXd g = sim.gradient(x);
    const Eigen::MatrixXd H = Eigen::MatrixXd(sim.hessian(x, false));
    CHECK((H - H.transpose()).norm() < 1e-9);
    const double h = 1e-6;
    for (int i = 0; i < x.size(); ++i) {
        Eigen::VectorXd xp = x, xm = x;
        xp[i] += h;
        xm[i] -= h;
        CHECK(g[i] == doctest::Approx((sim.energy(xp) - sim.energy(xm)) / (2 * h)).epsilon(1e-5).scale(1e-3));
        if (i % 3 == 0) {
            const Eigen::VectorXd col = (sim.gradient(xp) - sim.gradient(xm)) / (2 * h);
            CHECK((H.col(i) - col).norm() < 1e-5 * (1.0 + col.norm()));
        }
    }
}

TEST_CASE("prestretch: intact skin at rest is in equilibrium under the analytic in-vivo stress") {
    Sheet s(kGeometry);
    Simulation& sim = s.simulation();
    TissueParams p;
    p.stiffening = 0.0;  // Neo-Hookean, for a closed-form check
    p.prestretch_along = 1.10;
    p.prestretch_across = 1.04;
    sim.set_params(p);
    // Nothing moves: the uniform tension is balanced, the border is pinned.
    const StepInfo info = sim.step(10);
    CHECK(info.converged);
    CHECK(info.iterations == 0);
    // Plane-stress Neo-Hookean under F_e = diag(la, lc), thickness 1/(la lc):
    //   sigma_along = mu (la^2 - 1/(la lc)^2)
    const double la = 1.10, lc = 1.04, mu = p.shear_modulus;
    const double expected = mu * (la * la - 1.0 / (la * la * lc * lc));
    const Eigen::VectorXd s1 = sim.triangle_field(Field::MaxPrincipalStress);
    CHECK(s1.minCoeff() == doctest::Approx(expected).epsilon(1e-9));
    CHECK(s1.maxCoeff() == doctest::Approx(expected).epsilon(1e-9));
    MESSAGE("in-vivo tension along RSTL: " << expected * 1000 << " kPa");

    // Same with fibres on, at an angle: still uniform, still in equilibrium.
    TissueParams q = living_skin();
    q.fibre_angle = 30.0;
    sim.set_params(q);
    CHECK(sim.step(10).iterations == 0);
}

TEST_CASE("prestressed incisions gape, more across the RSTL than along them") {
    double gaps[2];
    for (int across = 0; across < 2; ++across) {
        Sheet s(kGeometry);
        s.simulation().set_params(living_skin());  // RSTL horizontal
        const Polyline cut = across ? Polyline{V(0, -15), V(0, 15)} : Polyline{V(-15, 0), V(15, 0)};
        REQUIRE(s.cut_rest(cut) > 0);
        settle(s.simulation());
        CHECK(s.simulation().min_area_ratio() > 0.0);
        gaps[across] = max_gap(s);
    }
    MESSAGE("30 mm incision gapes " << gaps[0] << " mm along the RSTL, " << gaps[1] << " mm across");
    CHECK(gaps[0] > 0.5);
    CHECK(gaps[1] > 1.5 * gaps[0]);
}

TEST_CASE("fibres make the skin stiffer along the RSTL") {
    double moved[2];
    for (int dir = 0; dir < 2; ++dir) {
        Sheet s(kGeometry);
        Simulation& sim = s.simulation();
        TissueParams p = living_skin();
        // Isolate the fibres: no prestretch, and a Neo-Hookean matrix (with
        // strong isotropic stiffening the matrix would dominate both ways).
        p.prestretch_along = p.prestretch_across = 1.0;
        p.stiffening = 0.0;
        p.fibre_stiffness = 0.1;
        p.fibre_dispersion = 0.05;
        sim.set_params(p);
        GrabParams g;
        g.max_force = 2.0;  // the same 2 N pull both ways
        sim.set_grab_params(g);
        REQUIRE(sim.grab(0, V(0, 0)));
        sim.move_grab(0, dir == 0 ? V(30, 0) : V(0, 30));
        settle(sim);
        moved[dir] = sim.grab_info().at(0).point.norm();
    }
    // A forceps pull loads the skin in all directions around it (including
    // shear, which fibres along one axis don't resist), so the difference is
    // smaller than in a pure stretch (next test).
    MESSAGE("2 N pull moves the skin " << moved[0] << " mm along the RSTL, " << moved[1] << " mm across");
    CHECK(moved[0] < 0.9 * moved[1]);
}

TEST_CASE("a uniaxial stretch along the fibres needs far more stress than across") {
    // Same stretch (15%, free sides), once along the fibres and once across.
    const double l = 1.15;
    Eigen::Matrix2d F = Eigen::Matrix2d::Zero();
    F(0, 0) = l;
    F(1, 1) = 1.0 / std::sqrt(l);
    double stress[2];
    for (int across = 0; across < 2; ++across) {
        Simulation sim(make_rectangle_mesh(10.0, 10.0, 2.5));
        TissueParams p = living_skin();
        p.prestretch_along = p.prestretch_across = 1.0;
        p.stiffening = 0.0;
        p.fibre_stiffness = 0.1;
        p.fibre_dispersion = 0.05;
        p.fibre_angle = across ? 90.0 : 0.0;
        p.pin_boundary = false;
        sim.set_params(p);
        Eigen::VectorXd x(sim.dofs().size());
        for (int v = 0; v < sim.mesh().vertex_count(); ++v) x.segment<2>(2 * v) = F * sim.mesh().rest_positions.col(v);
        sim.set_dofs(x);
        stress[across] = sim.triangle_field(Field::MaxPrincipalStress)[0];
    }
    MESSAGE("15% stretch: " << stress[0] * 1000 << " kPa along the fibres, " << stress[1] * 1000 << " kPa across");
    CHECK(stress[0] > 2.0 * stress[1]);
}

// ---------------------------------------------------------------------------
// Undermining
// ---------------------------------------------------------------------------

TEST_CASE("undermining brush: soft-edged stamps in rest coordinates") {
    Sheet s(kGeometry);
    CHECK(s.undermining_at(V(0, 0)) == 0.0);
    REQUIRE(s.undermine_at(V(10, 0), 5.0, true));
    CHECK(s.undermining_at(V(10, 0)) == 1.0);
    CHECK(s.undermining_at(V(12.5, 0)) == 1.0);   // inner 60%: full
    const double mid = s.undermining_at(V(14, 0));
    CHECK(mid > 0.0);
    CHECK(mid < 1.0);
    CHECK(s.undermining_at(V(15.5, 0)) == 0.0);
    CHECK_FALSE(s.undermine_at(V(90, 0), 5.0, true));  // off the sheet
    CHECK(s.undermine_count() == 1);
}

TEST_CASE("undermining lets a wound gape more") {
    double gap[2];
    for (int undermined = 0; undermined < 2; ++undermined) {
        Sheet s(kGeometry);
        TissueParams p = living_skin();
        p.tether_stiffness = 0.005;
        s.simulation().set_params(p);
        REQUIRE(s.cut_rest({V(0, -15), V(0, 15)}) > 0);
        if (undermined)
            for (double y = -20; y <= 20; y += 2) CHECK(s.undermine_at(V(0, y), 15.0, y == -20));
        settle(s.simulation());
        gap[undermined] = max_gap(s);
    }
    MESSAGE("incision gapes " << gap[0] << " mm, after undermining " << gap[1] << " mm");
    CHECK(gap[1] > 1.3 * gap[0]);
}

// Undermining pays off when tissue must be *advanced*: pulled past where it
// lies in the body, as when closing a defect or insetting a flap. The tethers
// to the underlying tissue resist that sliding; undermining releases them.
// (Re-approximating a plain incision just restores the in-vivo state, so its
// closing tension is set by the skin's own prestress, not by the tethers.)
TEST_CASE("undermining reduces the force needed to advance skin") {
    double tension[2];
    for (int undermined = 0; undermined < 2; ++undermined) {
        Sheet s(kGeometry);
        TissueParams p = living_skin();
        p.prestretch_along = p.prestretch_across = 1.0;
        p.tether_stiffness = 0.005;  // well attached skin
        s.simulation().set_params(p);
        REQUIRE(s.cut_rest({V(0, -15), V(0, 15)}) > 0);
        if (undermined)  // the tissue to be advanced: left of the incision
            for (double y = -20; y <= 20; y += 2) CHECK(s.undermine_at(V(-12, y), 14.0, y == -20));
        Simulation& sim = s.simulation();
        // Advance the left side 6 mm: a suture from 8 mm left of the
        // incision to 2 mm right of it, tied so the edges just meet.
        MaterialPoint a, b;
        REQUIRE(sim.material_point_at(V(-8, 0), a));
        REQUIRE(sim.material_point_at(V(2, 0), b));
        REQUIRE(sim.add_suture(a, b, 4.0) > 0);
        settle(sim);
        tension[undermined] = sim.suture_info().at(0).force;
        CHECK(sim.min_area_ratio() > 0.0);
    }
    MESSAGE("advancing 6 mm: " << tension[0] << " N, after undermining " << tension[1] << " N");
    CHECK(tension[1] < 0.7 * tension[0]);
}

TEST_CASE("undermining survives re-meshing, undo and clear") {
    Sheet s(kGeometry);
    REQUIRE(s.undermine_at(V(20, 10), 6.0, true));
    REQUIRE(s.cut_rest({V(-20, -10), V(-20, 10)}) > 0);  // re-mesh
    auto value_near = [&](const V& p) {
        const TriMesh& m = s.mesh();
        int best = 0;
        for (int v = 1; v < m.vertex_count(); ++v)
            if ((m.rest_positions.col(v) - p).norm() < (m.rest_positions.col(best) - p).norm()) best = v;
        return s.simulation().undermining()[best];
    };
    CHECK(value_near(V(20, 10)) == 1.0);
    CHECK(value_near(V(-20, 0)) == 0.0);
    REQUIRE(s.undermine_at(V(-30, -10), 4.0, true));
    CHECK(s.undermine_count() == 2);
    CHECK(s.undo_undermine());
    CHECK(value_near(V(-30, -10)) == 0.0);
    CHECK(value_near(V(20, 10)) == 1.0);
    s.clear_undermining();
    CHECK(s.simulation().undermining().maxCoeff() == 0.0);
}
