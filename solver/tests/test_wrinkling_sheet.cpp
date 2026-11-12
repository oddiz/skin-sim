// Wrinkling in the assembled simulation: derivatives, and what it changes
// (and does not change) in incisions, excisions, dog-ears and sutures.

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <random>

#include "skinsim/excision.hpp"
#include "skinsim/sheet.hpp"

using namespace skinsim;

namespace {

using V = Eigen::Vector2d;

const SheetGeometry kGeometry{100.0, 60.0, 2.0};

bool settle(Simulation& sim) {
    for (int frame = 0; frame < 300; ++frame)
        if (sim.step(20).converged) return true;
    return false;
}

TissueParams living_skin(double wrinkling) {
    TissueParams p;
    p.stiffening = 4.0;
    p.fibre_angle = 0.0;
    p.fibre_stiffness = 0.05;
    p.fibre_nonlinearity = 5.0;
    p.fibre_dispersion = 0.15;
    p.prestretch_along = 1.08;
    p.prestretch_across = 1.03;
    p.wrinkling = wrinkling;
    return p;
}

MaterialPoint at(const Simulation& sim, const V& p) {
    MaterialPoint m;
    REQUIRE(sim.material_point_at(p, m));
    return m;
}

// A fusiform excision along the RSTL, closed by sutures across it (3 mm bites).
struct Closure {
    Sheet sheet{kGeometry};
    double settle_ms = 0.0;
    Closure(double length, double ratio, double apex, const std::vector<double>& sutures, double wrinkling) {
        sheet.simulation().set_params(living_skin(wrinkling));
        const Polyline o = fusiform_outline(V(-0.5 * length, 0), V(0.5 * length, 0), ratio, apex);
        REQUIRE(sheet.cut_rest(o) > 0);
        REQUIRE(sheet.excise_rest(V(0, 0)) == 1);
        Simulation& sim = sheet.simulation();
        for (double s : sutures) {
            double hw = 0.0, dist = 1e9;
            for (const V& p : o)
                if (p.y() > 0 && std::abs(p.x() - s) < dist) {
                    dist = std::abs(p.x() - s);
                    hw = p.y();
                }
            REQUIRE(sim.add_suture(at(sim, V(s, hw + 3.0)), at(sim, V(s, -hw - 3.0)), 0.0) > 0);
        }
        const auto t0 = std::chrono::steady_clock::now();
        REQUIRE(settle(sim));
        settle_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    // Most extreme value of a vertex field within 5 mm of either tip (rest positions).
    double at_tips(Field f, double half_length, bool lowest) const {
        const Simulation& sim = sheet.simulation();
        const Eigen::VectorXd v = sim.vertex_field(f);
        double worst = 0.0;
        for (int i = 0; i < sim.mesh().vertex_count(); ++i) {
            const V X = sim.mesh().rest_positions.col(i);
            if ((X - V(half_length, 0)).norm() < 5.0 || (X - V(-half_length, 0)).norm() < 5.0)
                worst = lowest ? std::min(worst, v[i]) : std::max(worst, v[i]);
        }
        return worst;
    }
    double total_tension() const {
        double t = 0.0;
        for (const SutureInfo& su : sheet.simulation().suture_info()) t += su.force;
        return t;
    }
};

}  // namespace

TEST_CASE("assembled derivatives with wrinkling (squeezed, jiggled sheet)") {
    // Squeeze a small sheet to 90% of its width, stretch it to 108% of its
    // height, and jiggle it: a mix of taut and wrinkled elements.
    Simulation sim(make_rectangle_mesh(24.0, 16.0, 4.0));
    TissueParams p = living_skin(0.9);
    p.prestretch_along = p.prestretch_across = 1.0;
    p.tether_stiffness = 0.002;
    p.pin_boundary = false;
    sim.set_params(p);
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> noise(-0.3, 0.3);
    Eigen::VectorXd x = sim.dofs();
    for (int i = 0; i < x.size(); ++i) x[i] = (i % 2 == 0 ? 0.9 : 1.08) * x[i] + noise(rng);
    sim.set_dofs(x);
    const Eigen::VectorXd g = sim.gradient(x);
    const Eigen::MatrixXd H = Eigen::MatrixXd(sim.hessian(x, /*project_psd=*/false));
    CHECK((H - H.transpose()).norm() < 1e-10);
    const double h = 1e-6;
    for (int i = 0; i < x.size(); ++i) {
        Eigen::VectorXd xp = x, xm = x;
        xp[i] += h;
        xm[i] -= h;
        CHECK(g[i] == doctest::Approx((sim.energy(xp) - sim.energy(xm)) / (2 * h)).epsilon(1e-5).scale(1e-3));
        const Eigen::VectorXd fd = (sim.gradient(xp) - sim.gradient(xm)) / (2 * h);
        CHECK((H.col(i) - fd).norm() <= 1e-5 * (1.0 + fd.norm()));
    }
    // Elements of both kinds were actually tested.
    const Eigen::VectorXd excess = sim.triangle_field(Field::ExcessSkin);
    MESSAGE((excess.array() > 0.0).count() << " of " << excess.size() << " elements wrinkled");
    CHECK((excess.array() > 0.0).count() > excess.size() / 4);
    CHECK((excess.array() == 0.0).count() > excess.size() / 10);
}

TEST_CASE("wrinkling leaves taut skin alone: an incision gapes the same") {
    double gape[2];
    for (int k = 0; k < 2; ++k) {
        Sheet s(kGeometry);
        s.simulation().set_params(living_skin(k == 0 ? 0.0 : 0.9));
        REQUIRE(s.cut_rest({V(0, -10), V(0, 10)}) > 0);
        REQUIRE(settle(s.simulation()));
        // Gap between the lips across the middle of the incision.
        const auto x = s.simulation().positions();
        const TriMesh& m = s.simulation().mesh();
        double lo = 1e9, hi = -1e9;
        for (const Edge& e : m.cut_edges)
            for (int v : e) {
                if (std::abs(m.rest_positions(1, v)) > 1.0) continue;
                lo = std::min(lo, x(0, v));
                hi = std::max(hi, x(0, v));
            }
        gape[k] = hi - lo;
    }
    MESSAGE("gape: flat " << gape[0] << " mm, wrinkling " << gape[1] << " mm");
    CHECK(gape[0] > 0.5);
    CHECK(gape[1] == doctest::Approx(gape[0]).epsilon(0.02));
}

TEST_CASE("dog-ears: wrinkling relieves the compression and shows the excess skin") {
    // The M7 dog-ear comparison again, flat membrane vs wrinkling membrane.
    const Closure classic_flat(30.0, 3.0, 30.0, {-9, -6, -3, 0, 3, 6, 9}, 0.0);
    const Closure blunt_flat(15.0, 1.5, 90.0, {-4.5, -1.5, 1.5, 4.5}, 0.0);
    const Closure classic(30.0, 3.0, 30.0, {-9, -6, -3, 0, 3, 6, 9}, 0.9);
    const Closure blunt(15.0, 1.5, 90.0, {-4.5, -1.5, 1.5, 4.5}, 0.9);
    const double s_flat = blunt_flat.at_tips(Field::MinPrincipalStress, 7.5, true);
    const double s_wr = blunt.at_tips(Field::MinPrincipalStress, 7.5, true);
    const double ex_classic = classic.at_tips(Field::ExcessSkin, 15.0, false);
    const double ex_blunt = blunt.at_tips(Field::ExcessSkin, 7.5, false);
    MESSAGE("blunt ellipse, compression at the ends: flat " << s_flat * 1000 << " kPa, wrinkling " << s_wr * 1000
                                                             << " kPa");
    MESSAGE("excess skin at the ends: 3:1 " << ex_classic * 100 << "%, 1.5:1 " << ex_blunt * 100 << "%");
    MESSAGE("closure tension: 3:1 flat " << classic_flat.total_tension() << " N, wrinkling "
                                         << classic.total_tension() << " N; 1.5:1 flat "
                                         << blunt_flat.total_tension() << " N, wrinkling " << blunt.total_tension()
                                         << " N");
    MESSAGE("settling time: flat " << blunt_flat.settle_ms << " ms, wrinkling " << blunt.settle_ms << " ms");
    CHECK(s_wr > 0.3 * s_flat);    // much less compression (both negative)
    CHECK(ex_blunt > ex_classic);  // the blunt ellipse bunches more skin at its ends
    CHECK(ex_blunt > 0.0);
    // The closure itself is tension: wrinkling barely changes it.
    CHECK(classic.total_tension() == doctest::Approx(classic_flat.total_tension()).epsilon(0.1));
}

TEST_CASE("tight knot: the loop squeezes less once the skin can wrinkle") {
    // Less, but still a lot: the knot imposes the squeeze (the tissue must
    // fit in a loop 40% too short), so even the 10% of compressive stiffness
    // that buckled skin keeps meets very large strains. Tissue inside a
    // strangulating loop really is crushed.
    double loop[2], tension[2];
    for (int k = 0; k < 2; ++k) {
        Sheet s(kGeometry);
        TissueParams p;  // plain isotropic skin, as in the contact test
        p.wrinkling = k == 0 ? 0.0 : 0.9;
        s.simulation().set_params(p);
        REQUIRE(s.cut_rest({V(0, -15), V(0, 15)}) > 0);
        Simulation& sim = s.simulation();
        REQUIRE(sim.add_suture(at(sim, V(-3, 0)), at(sim, V(3, 0)), 0.4) > 0);
        REQUIRE(settle(sim));
        loop[k] = sim.suture_info().at(0).loop_stress;
        tension[k] = sim.suture_info().at(0).force;
    }
    MESSAGE("40% knot, loop stress: flat " << loop[0] * 1000 << " kPa, wrinkling " << loop[1] * 1000
                                           << " kPa; tension " << tension[0] << " N vs " << tension[1] << " N");
    CHECK(loop[0] < 0.0);
    CHECK(loop[1] > loop[0]);
    CHECK(tension[1] < tension[0]);
}

TEST_CASE("maximum wrinkling under a hard squeeze: converges, nothing inverts") {
    Sheet s(kGeometry);
    TissueParams p = living_skin(1.0);  // clamped to 0.99
    p.prestretch_along = p.prestretch_across = 1.0;
    Simulation& sim = s.simulation();
    sim.set_params(p);
    CHECK(sim.params().wrinkling == doctest::Approx(0.99));
    GrabParams gp;
    gp.radius = 5.0;
    gp.max_force = 1e9;
    sim.set_grab_params(gp);
    REQUIRE(sim.grab(1, V(-10, 0)));
    REQUIRE(sim.grab(2, V(10, 0)));
    sim.move_grab(1, V(-2, 0));  // push two patches towards each other
    sim.move_grab(2, V(2, 0));
    CHECK(settle(sim));
    CHECK(sim.min_area_ratio() > 0.0);
    CHECK(sim.dofs().allFinite());
    const Eigen::VectorXd excess = sim.vertex_field(Field::ExcessSkin);
    MESSAGE("hard squeeze: min area ratio " << sim.min_area_ratio() << ", max excess skin "
                                            << excess.maxCoeff() * 100 << "%");
    CHECK(excess.maxCoeff() > 0.1);
}
