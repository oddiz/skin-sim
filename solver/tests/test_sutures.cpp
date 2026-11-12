// Tests for sutures (energy/derivatives, closing wounds, picking, surviving
// re-meshing) and for the stress/strain output fields used by the heatmap.

#include <doctest/doctest.h>

#include <cmath>
#include <random>

#include "skinsim/sheet.hpp"

using namespace skinsim;

namespace {

using V = Eigen::Vector2d;

const SheetGeometry kGeometry{100.0, 60.0, 2.5};

void settle(Simulation& sim) {
    for (int frame = 0; frame < 200; ++frame)
        if (sim.step(20).converged) return;
}

// Put every node at F X (a homogeneous deformation), pins off.
Simulation homogeneous(const Eigen::Matrix2d& F, double b) {
    Simulation sim(make_rectangle_mesh(10.0, 10.0, 2.5));
    TissueParams p;
    p.pin_boundary = false;
    p.stiffening = b;
    sim.set_params(p);
    Eigen::VectorXd x(sim.dofs().size());
    for (int v = 0; v < sim.mesh().vertex_count(); ++v) x.segment<2>(2 * v) = F * sim.mesh().rest_positions.col(v);
    sim.set_dofs(x);
    return sim;
}

// A point on the lip of the horizontal incision through y = 0, on the upper
// (side > 0) or lower (side < 0) lip. Both lips share rest positions, so
// "nearest" alone can't tell them apart: check the triangle's side.
MaterialPoint lip_point(const Simulation& sim, double x, int side) {
    MaterialPoint p, q;
    REQUIRE(sim.nearest_lip_point(V(x, 0.0), 1.0, p));
    const Triangle& t = sim.mesh().triangles[p.triangle];
    const double cy = (sim.mesh().rest_positions(1, t[0]) + sim.mesh().rest_positions(1, t[1]) +
                       sim.mesh().rest_positions(1, t[2])) / 3.0;
    if ((cy > 0) == (side > 0)) return p;
    REQUIRE(sim.opposite_lip_point(p, q));
    return q;
}

Sheet cut_sheet() {
    Sheet s(kGeometry);
    REQUIRE(s.cut_rest({V(-15, 0), V(15, 0)}) > 0);
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// Output fields
// ---------------------------------------------------------------------------

TEST_CASE("Cauchy stress matches the analytic uniaxial solution") {
    // Uniaxial stretch l with free sides: F = diag(l, l^-1/2) (incompressible,
    // the width and thickness contract equally). Plane-stress Neo-Hookean:
    //   sigma_11 = mu (l^2 - 1/l),  sigma_22 = 0.
    // Stiffening multiplies the stress by exp(b Q), Q = l^2 + 2/l - 3.
    const double mu = TissueParams{}.shear_modulus;
    for (double b : {0.0, 8.0}) {
        for (double l : {1.05, 1.2, 1.4}) {
            CAPTURE(b);
            CAPTURE(l);
            Eigen::Matrix2d F = Eigen::Matrix2d::Zero();
            F(0, 0) = l;
            F(1, 1) = 1.0 / std::sqrt(l);
            const Simulation sim = homogeneous(F, b);
            const double expected = mu * (l * l - 1.0 / l) * std::exp(b * (l * l + 2.0 / l - 3.0));
            const Eigen::VectorXd s1 = sim.triangle_field(Field::MaxPrincipalStress);
            const Eigen::VectorXd vm = sim.triangle_field(Field::VonMisesStress);
            const Eigen::VectorXd J = sim.triangle_field(Field::AreaChange);
            for (int t = 0; t < s1.size(); ++t) {
                CHECK(s1[t] == doctest::Approx(expected).epsilon(1e-9));
                CHECK(vm[t] == doctest::Approx(expected).epsilon(1e-9));  // sigma_22 = 0: von Mises = sigma_11
                CHECK(J[t] == doctest::Approx(std::sqrt(l)).epsilon(1e-12));
            }
        }
    }
}

TEST_CASE("pure rotation carries no stress; compression is negative") {
    const double a = 0.7;
    Eigen::Matrix2d R;
    R << std::cos(a), -std::sin(a), std::sin(a), std::cos(a);
    const Simulation rotated = homogeneous(R, 8.0);
    CHECK(rotated.triangle_field(Field::VonMisesStress).cwiseAbs().maxCoeff() < 1e-12);

    // Uniaxial *compression*: the largest principal stress is the free
    // direction (0); the compressive one is negative. Check via the minimum:
    // with F = diag(l, l^-1/2), l < 1, sigma_11 < 0 and sigma_22 = 0.
    Eigen::Matrix2d F = Eigen::Matrix2d::Zero();
    F(0, 0) = 0.9;
    F(1, 1) = 1.0 / std::sqrt(0.9);
    const Simulation squeezed = homogeneous(F, 0.0);
    CHECK(squeezed.triangle_field(Field::MaxPrincipalStress).cwiseAbs().maxCoeff() < 1e-12);
    CHECK(squeezed.triangle_field(Field::VonMisesStress).minCoeff() > 0.0);
}

TEST_CASE("vertex averaging preserves a uniform field") {
    Eigen::Matrix2d F;
    F << 1.1, 0.05, 0.0, 0.95;
    const Simulation sim = homogeneous(F, 4.0);
    const Eigen::VectorXd t = sim.triangle_field(Field::VonMisesStress);
    const Eigen::VectorXd v = sim.vertex_field(Field::VonMisesStress);
    CHECK(v.size() == sim.mesh().vertex_count());
    CHECK((v.array() - t[0]).abs().maxCoeff() < 1e-12);
}

// ---------------------------------------------------------------------------
// Sutures
// ---------------------------------------------------------------------------

TEST_CASE("suture energy: gradient and Hessian match finite differences") {
    Sheet s = cut_sheet();
    Simulation& sim = s.simulation();
    TissueParams p;
    p.pin_boundary = false;
    sim.set_params(p);
    SutureParams sp;
    sp.stiffness = 3.0;  // moderate, so finite differences are well conditioned
    sim.set_suture_params(sp);
    // Off-node points (weights strictly between 0 and 1), not twins.
    REQUIRE(sim.add_suture(lip_point(sim, -3.3, +1), lip_point(sim, 2.1, -1)) > 0);

    std::mt19937 rng(3);
    std::uniform_real_distribution<double> noise(-0.3, 0.3);
    Eigen::VectorXd x = sim.dofs();
    for (int i = 0; i < x.size(); ++i) x[i] += noise(rng);

    // Only check the dofs of nodes near the suture (all others: same code as M2/M3).
    const Eigen::VectorXd g = sim.gradient(x);
    const Eigen::MatrixXd H = Eigen::MatrixXd(sim.hessian(x, false));
    const double h = 1e-6;
    int checked = 0;
    for (int i = 0; i < x.size(); ++i) {
        const V rest = sim.mesh().rest_positions.col(i / 2);
        if ((rest - V(-0.6, 0)).norm() > 5.0) continue;
        Eigen::VectorXd xp = x, xm = x;
        xp[i] += h;
        xm[i] -= h;
        CHECK(g[i] == doctest::Approx((sim.energy(xp) - sim.energy(xm)) / (2 * h)).epsilon(1e-5).scale(1e-3));
        const Eigen::VectorXd col = (sim.gradient(xp) - sim.gradient(xm)) / (2 * h);
        CHECK((H.col(i) - col).norm() < 1e-5 * (1.0 + col.norm()));
        ++checked;
    }
    CHECK(checked > 20);
}

TEST_CASE("a suture closes an incision held open") {
    Sheet s(kGeometry);
    REQUIRE(s.cut_rest({V(0, -15), V(0, 15)}) > 0);  // vertical incision
    Simulation& sim = s.simulation();
    REQUIRE(sim.grab(0, V(3, 0)));
    sim.move_grab(0, V(10, 0));  // pull the right lip away and hold
    settle(sim);

    MaterialPoint a, b;
    REQUIRE(sim.nearest_lip_point(V(0, 0), 3.0, a));  // finds a lip even though it moved
    REQUIRE(sim.opposite_lip_point(a, b));
    const double gap_before = (sim.position_of(a) - sim.position_of(b)).norm();
    REQUIRE(sim.add_suture(a, b) > 0);
    settle(sim);
    const SutureInfo info = sim.suture_info().at(0);
    const double gap_after = (info.a - info.b).norm();
    MESSAGE("gap " << gap_before << " mm -> " << gap_after << " mm, suture tension " << info.force << " N");
    CHECK(gap_before > 2.0);
    // Not exactly 0: the thread pulls on the strip of tissue in each bite,
    // which stretches a little under ~1.8 N, as real wound edges do.
    CHECK(gap_after < 0.3);
    CHECK(info.force > 0.1);
    CHECK(sim.min_area_ratio() > 0.0);
}

TEST_CASE("sutures pulling lips out of register need more tension") {
    // Joining the upper lip at -d to the lower lip at +d shears the wound
    // edges: the larger the mismatch, the more tension the closure needs.
    double previous = 0.0;
    for (double d : {1.0, 3.0, 6.0}) {
        CAPTURE(d);
        Sheet s = cut_sheet();
        Simulation& sim = s.simulation();
        REQUIRE(sim.add_suture(lip_point(sim, -d, +1), lip_point(sim, d, -1)) > 0);
        settle(sim);
        const double force = sim.suture_info().at(0).force;
        MESSAGE("mismatch " << 2 * d << " mm: tension " << force << " N");
        CHECK(force > previous);
        CHECK(sim.min_area_ratio() > 0.0);
        previous = force;
    }
}

TEST_CASE("suture placement: lips only, no partner at a cut's tip") {
    Sheet s = cut_sheet();
    const Simulation& sim = s.simulation();
    MaterialPoint p, q;
    CHECK_FALSE(sim.nearest_lip_point(V(0, 10), 2.0, p));  // nowhere near the incision
    REQUIRE(sim.nearest_lip_point(V(-15.2, 0.1), 1.0, p));  // at the tip
    CHECK_FALSE(sim.opposite_lip_point(p, q));
    REQUIRE(sim.nearest_lip_point(V(4.4, 0.0), 1.0, p));
    REQUIRE(sim.opposite_lip_point(p, q));
    CHECK(q.triangle != p.triangle);
    // Same material point at rest: the twin.
    CHECK((sim.position_of(p) - sim.position_of(q)).norm() < 1e-9);
}

TEST_CASE("sutures stay attached to the same tissue across a later cut") {
    Sheet s = cut_sheet();
    Simulation* sim = &s.simulation();
    REQUIRE(sim->add_suture(lip_point(*sim, -4, +1), lip_point(*sim, 4, -1)) > 0);
    settle(*sim);
    const SutureInfo before = sim->suture_info().at(0);

    REQUIRE(s.cut_rest({V(30, -10), V(30, 10)}) > 0);  // elsewhere
    sim = &s.simulation();
    REQUIRE(sim->suture_count() == 1);
    settle(*sim);
    const SutureInfo after = sim->suture_info().at(0);
    CHECK(after.id == before.id);
    CHECK((after.a - before.a).norm() < 0.1);
    CHECK(after.force == doctest::Approx(before.force).epsilon(0.05));

    // Undoing the sutured incision merges its lips; the suture still exists
    // (now joining two points of intact skin) and the solver stays valid.
    REQUIRE(s.undo_cut());
    REQUIRE(s.undo_cut());
    sim = &s.simulation();
    CHECK(sim->suture_count() == 1);
    settle(*sim);
    CHECK(sim->min_area_ratio() > 0.0);
}

TEST_CASE("an offset closure concentrates stress around the suture") {
    Sheet s = cut_sheet();
    Simulation& sim = s.simulation();
    REQUIRE(sim.add_suture(lip_point(sim, -4, +1), lip_point(sim, 4, -1)) > 0);
    settle(sim);
    const Eigen::VectorXd vm = sim.vertex_field(Field::VonMisesStress);
    double near = 0.0, far = 0.0;
    for (int v = 0; v < sim.mesh().vertex_count(); ++v) {
        const double r = sim.mesh().rest_positions.col(v).norm();
        if (r < 6.0) near = std::max(near, vm[v]);
        if (r > 35.0) far = std::max(far, vm[v]);
    }
    MESSAGE("von Mises near suture " << near * 1000 << " kPa, far field " << far * 1000 << " kPa");
    CHECK(near > 10.0 * far);
}

TEST_CASE("invalid sutures are refused; removing and clearing work") {
    Sheet s = cut_sheet();
    Simulation& sim = s.simulation();
    MaterialPoint bad;  // triangle -1
    CHECK(sim.add_suture(bad, lip_point(sim, 0, 1)) == -1);
    const int id = sim.add_suture(lip_point(sim, -2, 1), lip_point(sim, -2, -1));
    REQUIRE(id > 0);
    sim.add_suture(lip_point(sim, 2, 1), lip_point(sim, 2, -1));
    CHECK(sim.suture_count() == 2);
    CHECK(sim.remove_suture(id));
    CHECK_FALSE(sim.remove_suture(id));
    sim.clear_sutures();
    CHECK(sim.suture_count() == 0);
}

// ---------------------------------------------------------------------------
// Sutures tied to a length (entry/exit bites, knot tightness)
// ---------------------------------------------------------------------------

namespace {

Sheet vertical_cut() {
    Sheet s(kGeometry);
    REQUIRE(s.cut_rest({V(0, -15), V(0, 15)}) > 0);
    return s;
}

MaterialPoint at(const Simulation& sim, const V& p) {
    MaterialPoint m;
    REQUIRE(sim.material_point_at(p, m));
    return m;
}

}  // namespace

TEST_CASE("taut suture with a tied length: gradient and Hessian match finite differences") {
    Sheet s = vertical_cut();
    Simulation& sim = s.simulation();
    TissueParams p;
    p.pin_boundary = false;
    sim.set_params(p);
    SutureParams sp;
    sp.stiffness = 3.0;
    sim.set_suture_params(sp);
    const MaterialPoint a = at(sim, V(-2.7, 1.1)), b = at(sim, V(3.4, -0.6));
    // Tied 30% tight: well inside the taut regime (the slack/taut switch is
    // a kink where finite differences don't apply).
    REQUIRE(sim.add_suture(a, b, 0.3) > 0);

    // Small noise: the triangles along the incision are only 1.25 mm, and an
    // inverted one would make the energy infinite (nothing to differentiate).
    std::mt19937 rng(5);
    std::uniform_real_distribution<double> noise(-0.1, 0.1);
    Eigen::VectorXd x = sim.dofs();
    for (int i = 0; i < x.size(); ++i) x[i] += noise(rng);
    const Eigen::VectorXd g = sim.gradient(x);
    const Eigen::MatrixXd H = Eigen::MatrixXd(sim.hessian(x, false));
    CHECK((H - H.transpose()).norm() < 1e-10);
    const double h = 1e-6;
    int checked = 0;
    for (int i = 0; i < x.size(); ++i) {
        if (sim.mesh().rest_positions.col(i / 2).norm() > 6.0) continue;
        Eigen::VectorXd xp = x, xm = x;
        xp[i] += h;
        xm[i] -= h;
        CHECK(g[i] == doctest::Approx((sim.energy(xp) - sim.energy(xm)) / (2 * h)).epsilon(1e-5).scale(1e-3));
        const Eigen::VectorXd col = (sim.gradient(xp) - sim.gradient(xm)) / (2 * h);
        CHECK((H.col(i) - col).norm() < 1e-5 * (1.0 + col.norm()));
        ++checked;
    }
    CHECK(checked > 20);
}

TEST_CASE("a slack suture exerts no force") {
    Sheet s = vertical_cut();
    Simulation& sim = s.simulation();
    const Eigen::VectorXd before = sim.gradient(sim.dofs());
    REQUIRE(sim.add_suture(at(sim, V(-3, 0)), at(sim, V(3, 0)), -1.0) > 0);  // 6 mm of extra thread
    CHECK((sim.gradient(sim.dofs()) - before).norm() == 0.0);
    CHECK(sim.suture_info().at(0).force == 0.0);
}

TEST_CASE("bite: distance of a suture's entry point from the incision") {
    Sheet s = vertical_cut();
    const Simulation& sim = s.simulation();
    CHECK(sim.bite(at(sim, V(-3, 5))) == doctest::Approx(3.0));
    CHECK(sim.bite(at(sim, V(2, 0))) == doctest::Approx(2.0));
    CHECK(sim.bite(at(sim, V(0, 20))) == doctest::Approx(5.0));  // beyond the tip
    Sheet uncut(kGeometry);
    CHECK(uncut.simulation().bite(at(uncut.simulation(), V(1, 1))) == 0.0);
    MaterialPoint m;
    CHECK_FALSE(uncut.simulation().material_point_at(V(80, 0), m));  // off the sheet
}

TEST_CASE("knot tightness: edges just meet at the bite length; tighter means more tension") {
    // Entry 3 mm left of the incision, exit 3 mm right: the edges meet when
    // the thread is 6 mm. Tying shorter squeezes the tissue in the loop.
    double previous = -1.0;
    for (double tightness : {0.0, 0.2, 0.4}) {
        CAPTURE(tightness);
        Sheet s = vertical_cut();
        Simulation& sim = s.simulation();
        const MaterialPoint a = at(sim, V(-3, 0)), b = at(sim, V(3, 0));
        const double bites = sim.bite(a) + sim.bite(b);
        CHECK(bites == doctest::Approx(6.0));
        REQUIRE(sim.add_suture(a, b, tightness) > 0);
        settle(sim);
        const double force = sim.suture_info().at(0).force;
        MESSAGE("tightness " << tightness * 100 << "%: tension " << force << " N");
        if (tightness == 0.0) CHECK(force < 1e-9);
        CHECK(force > previous);
        CHECK(sim.min_area_ratio() > 0.0);
        previous = force;
    }
}

TEST_CASE("a loose knot leaves a gap; re-tying closes it") {
    Sheet s = vertical_cut();
    Simulation& sim = s.simulation();
    const MaterialPoint a = at(sim, V(-3, 0)), b = at(sim, V(3, 0));
    const int id = sim.add_suture(a, b, -0.5);  // loose: 3 mm more thread than the bites
    REQUIRE(id > 0);
    REQUIRE(sim.grab(0, V(6, 0)));
    sim.move_grab(0, V(14, 0));  // pull the right side away and hold
    settle(sim);

    MaterialPoint lip, twin;
    REQUIRE(sim.nearest_lip_point(V(-0.1, 0), 3.0, lip));
    REQUIRE(sim.opposite_lip_point(lip, twin));
    const double loose_gap = (sim.position_of(lip) - sim.position_of(twin)).norm();
    const double loose_force = sim.suture_info().at(0).force;

    REQUIRE(sim.set_suture_tightness(id, 0.0));  // re-tie: edges should meet
    settle(sim);
    const double tied_gap = (sim.position_of(lip) - sim.position_of(twin)).norm();
    const double tied_force = sim.suture_info().at(0).force;
    MESSAGE("loose knot: gap " << loose_gap << " mm, " << loose_force << " N; re-tied: gap " << tied_gap << " mm, "
                               << tied_force << " N");
    CHECK(loose_gap > 2.0);
    CHECK(tied_gap < 0.5);
    CHECK(tied_force > loose_force);
}

TEST_CASE("the knot's tightness survives re-meshing") {
    Sheet s = vertical_cut();
    REQUIRE(s.simulation().add_suture(at(s.simulation(), V(-3, 2)), at(s.simulation(), V(3, 2)), 0.25) > 0);
    REQUIRE(s.cut_rest({V(30, -10), V(30, 10)}) > 0);
    CHECK(s.simulation().suture_info().at(0).tightness == 0.25);
}

// ---------------------------------------------------------------------------
// Bite strips: distributed suture load, loop stress
// ---------------------------------------------------------------------------

namespace {

// A prestressed sheet (RSTL horizontal) with a vertical incision, which
// gapes; one suture across its middle, placed before the wound opens.
struct ClosedWound {
    Sheet sheet;
    int id = -1;
    ClosedWound(double element_size, double bite_mm, double strip_width, double tightness)
        : sheet(SheetGeometry{60.0, 40.0, element_size}) {
        TissueParams p;
        p.prestretch_along = 1.10;
        p.prestretch_across = 1.04;
        sheet.simulation().set_params(p);
        REQUIRE(sheet.cut_rest({V(0, -12), V(0, 12)}) > 0);
        Simulation& sim = sheet.simulation();
        SutureParams sp;
        sp.strip_width = strip_width;
        sim.set_suture_params(sp);
        MaterialPoint a, b;
        REQUIRE(sim.material_point_at(V(-bite_mm, 0), a));
        REQUIRE(sim.material_point_at(V(bite_mm, 0), b));
        id = sim.add_suture(a, b, tightness);
        REQUIRE(id > 0);
        settle(sim);
    }
    // Largest von Mises stress at vertices within r of the suture (rest).
    double peak_near_suture(double r) const {
        const Simulation& sim = sheet.simulation();
        const Eigen::VectorXd vm = sim.vertex_field(Field::VonMisesStress);
        double peak = 0.0;
        for (int v = 0; v < sim.mesh().vertex_count(); ++v)
            if (sim.mesh().rest_positions.col(v).norm() < r) peak = std::max(peak, vm[v]);
        return peak;
    }
    SutureInfo info() const { return sheet.simulation().suture_info().at(0); }
};

}  // namespace

// Where the thread pulls on the tissue (around the needle holes), a load
// spread over a strip gives a finite stress that settles as the mesh is
// refined; a near-point load (a 0.05 mm strip, like the old model) keeps
// rising. (Right at the wound edge, where the closed point meets the still
// gaping wound on either side, the edges form a sharp notch: the stress there
// is singular in any continuum model, so it is not used for this check.)
TEST_CASE("bite strips: stress around the needle holes converges as the mesh is refined") {
    auto needle_peak = [](const ClosedWound& w) {
        const Simulation& sim = w.sheet.simulation();
        const Eigen::VectorXd vm = sim.vertex_field(Field::VonMisesStress);
        double peak = 0.0;
        for (int v = 0; v < sim.mesh().vertex_count(); ++v)
            if ((sim.mesh().rest_positions.col(v) - V(-3.5, 0)).norm() < 1.2) peak = std::max(peak, vm[v]);
        return peak;
    };
    double strip[2], thin[2], loop[2], tension[2];
    const double sizes[2] = {1.0, 0.7};
    for (int i = 0; i < 2; ++i) {
        const ClosedWound w(sizes[i], 3.0, 2.0, 0.0);
        strip[i] = needle_peak(w);
        loop[i] = w.info().loop_stress;
        tension[i] = w.info().force;
        thin[i] = needle_peak(ClosedWound(sizes[i], 3.0, 0.05, 0.0));
        MESSAGE("element " << sizes[i] << " mm: needle-hole peak " << strip[i] * 1000 << " kPa (2 mm strips), "
                           << thin[i] * 1000 << " kPa (0.05 mm strips); loop stress " << loop[i] * 1000
                           << " kPa, tension " << tension[i] << " N");
    }
    CHECK(std::abs(strip[1] / strip[0] - 1.0) < 0.08);
    CHECK(thin[1] / thin[0] > 1.10);  // the near-point load does not converge
    CHECK(loop[1] == doctest::Approx(loop[0]).epsilon(0.05));
    CHECK(tension[1] == doctest::Approx(tension[0]).epsilon(0.03));
}

TEST_CASE("bite strips: wider bites spread the load and lower the stress") {
    double prev_peak = 1e9, prev_loop = 1e9;
    for (double bite : {1.5, 3.0, 5.0}) {
        CAPTURE(bite);
        const ClosedWound w(1.6, bite, 2.0, 0.0);
        const double peak = w.peak_near_suture(bite + 3.0), loop = w.info().loop_stress;
        MESSAGE("bites " << bite << " mm: tension " << w.info().force << " N, peak " << peak * 1000
                         << " kPa, loop stress " << loop * 1000 << " kPa");
        CHECK(peak < prev_peak);
        CHECK(loop < prev_loop);
        prev_peak = peak;
        prev_loop = loop;
    }
}

// How the loop stress responds to tightness (squeezing, once the lips press
// on each other) is tested in test_contact.cpp.

TEST_CASE("bite strips stay on their own side of the incision") {
    Sheet s = vertical_cut();
    Simulation& sim = s.simulation();
    const int id = sim.add_suture(at(sim, V(-3, 1)), at(sim, V(3, 1)), 0.0);
    REQUIRE(id > 0);
    const TriMesh& m = sim.mesh();
    for (int side = 0; side < 2; ++side) {
        std::vector<int> nodes;
        std::vector<double> w;
        REQUIRE(sim.suture_strip(id, side, nodes, w));
        CHECK(nodes.size() > 4);
        double total = 0.0;
        for (double x : w) total += x;
        CHECK(total == doctest::Approx(1.0));
        const double sign = side == 0 ? -1.0 : 1.0;
        for (int v : nodes) {
            CHECK(sign * m.rest_positions(0, v) >= -1e-9);  // never on the far side
            if (std::abs(m.rest_positions(0, v)) > 1e-9) continue;
            // On the incision: must be this side's copy (its triangles lie on this side).
            bool own = false;
            for (const Triangle& t : m.triangles)
                if (t[0] == v || t[1] == v || t[2] == v)
                    own = own || sign * (m.rest_positions(0, t[0]) + m.rest_positions(0, t[1]) + m.rest_positions(0, t[2])) > 0.0;
            CHECK(own);
        }
    }
}

