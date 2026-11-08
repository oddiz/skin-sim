// Tests for the assembled simulation: derivatives, Newton behaviour, robustness.

#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <random>

#include "skinsim/simulation.hpp"

using namespace skinsim;

namespace {

constexpr double kUnlimited = std::numeric_limits<double>::infinity();

// The three grab regimes the derivative tests must cover. The finger sits
// ~2.4 mm from the patch, far from the spring/constant-force switch-over
// (finite differences across that kink would be meaningless).
GrabParams grab_regime(int which) {
    GrabParams g;
    switch (which) {
        case 0: g.radius = 0.0; g.max_force = kUnlimited; break;  // point grab, spring
        case 1: g.radius = 6.0; g.max_force = kUnlimited; break;  // spread patch, spring
        default: g.radius = 6.0; g.max_force = 0.5; break;        // spread patch, force-capped
    }
    return g;
}

// A small sheet in a non-trivial state: interior nodes jiggled randomly so
// every element has a different, generic deformation. Stiffening is on.
Simulation jiggled_sheet(double amplitude, const GrabParams& grab = GrabParams{}) {
    Simulation sim(make_rectangle_mesh(24.0, 16.0, 4.0));
    TissueParams p;
    p.tether_stiffness = 0.002;
    p.pin_boundary = false;
    sim.set_params(p);
    sim.set_grab_params(grab);

    std::mt19937 rng(42);  // fixed seed: the test is identical every run
    std::uniform_real_distribution<double> noise(-amplitude, amplitude);
    Eigen::VectorXd x = sim.dofs();
    for (int i = 0; i < x.size(); ++i) x[i] += noise(rng);
    sim.set_dofs(x);
    REQUIRE(sim.grab(7, Eigen::Vector2d(1.3, -0.7)));
    sim.move_grab(7, Eigen::Vector2d(3.0, 1.0));
    return sim;
}

}  // namespace

TEST_CASE("assembled gradient matches finite differences of energy") {
    for (int regime = 0; regime < 3; ++regime) {
        CAPTURE(regime);  // doctest prints this if a check below fails
        const Simulation sim = jiggled_sheet(0.4, grab_regime(regime));
        const Eigen::VectorXd x = sim.dofs();
        const Eigen::VectorXd g = sim.gradient(x);
        const double h = 1e-6;
        for (int i = 0; i < x.size(); ++i) {
            Eigen::VectorXd xp = x, xm = x;
            xp[i] += h;
            xm[i] -= h;
            const double fd = (sim.energy(xp) - sim.energy(xm)) / (2 * h);
            CHECK(g[i] == doctest::Approx(fd).epsilon(1e-5).scale(1e-3));
        }
    }
}

TEST_CASE("assembled Hessian matches finite differences of gradient") {
  for (int regime = 0; regime < 3; ++regime) {
    CAPTURE(regime);
    const Simulation sim = jiggled_sheet(0.4, grab_regime(regime));
    const Eigen::VectorXd x = sim.dofs();
    const Eigen::MatrixXd H = Eigen::MatrixXd(sim.hessian(x, /*project_psd=*/false));
    CHECK((H - H.transpose()).norm() < 1e-10);
    const double h = 1e-6;
    for (int col = 0; col < x.size(); col += 3) {  // every third column keeps it quick
        Eigen::VectorXd xp = x, xm = x;
        xp[col] += h;
        xm[col] -= h;
        const Eigen::VectorXd fd = (sim.gradient(xp) - sim.gradient(xm)) / (2 * h);
        CHECK((H.col(col) - fd).norm() <= 1e-5 * (1.0 + fd.norm()));
    }
  }
}

TEST_CASE("sheet at rest is in equilibrium with zero energy") {
    Simulation sim(make_rectangle_mesh(40.0, 30.0, 5.0));
    const StepInfo info = sim.step(5);
    CHECK(info.converged);
    CHECK(info.iterations == 0);
    CHECK(info.energy == doctest::Approx(0.0));
}

TEST_CASE("pulling the centre: energy decreases monotonically and Newton converges") {
    // Pure Neo-Hookean, plain point spring: the setting the bounds below were derived for.
    Simulation sim(make_rectangle_mesh(40.0, 40.0, 4.0));
    TissueParams p;
    p.stiffening = 0.0;
    sim.set_params(p);
    GrabParams gp;
    gp.radius = 0.0;
    gp.max_force = kUnlimited;
    sim.set_grab_params(gp);
    REQUIRE(sim.grab(0, Eigen::Vector2d(0.0, 0.0)));
    sim.move_grab(0, Eigen::Vector2d(8.0, 0.0));

    double previous = sim.energy(sim.dofs());
    StepInfo info;
    int total_iterations = 0;
    for (int i = 0; i < 100 && !info.converged; ++i) {
        info = sim.step(1);  // one iteration at a time to watch the energy
        total_iterations += info.iterations;
        CHECK(info.energy <= previous + 1e-12);
        previous = info.energy;
    }
    CHECK(info.converged);
    CHECK_FALSE(info.stalled);
    CHECK(info.residual <= sim.residual_tolerance());
    // 8 mm pull, 2 mm max step per iteration -> at least 4 clamped iterations,
    // then a few quadratic ones. Far more would mean something is wrong.
    CHECK(total_iterations < 30);
    CHECK(sim.min_area_ratio() > 0.0);

    // The grabbed node moved towards the target but not all the way: the sheet
    // pulls back on the finger spring.
    const Eigen::Vector2d centre = sim.positions().col(sim.mesh().vertex_count() / 2);
    CHECK(centre.x() > 4.0);
    CHECK(centre.x() < 8.0);
    CHECK(std::abs(centre.y()) < 0.5);

    // Pinned boundary nodes have not moved.
    CHECK(sim.positions().col(0).isApprox(sim.mesh().rest_positions.col(0)));
}

TEST_CASE("releasing the grab returns the sheet to rest") {
    Simulation sim(make_rectangle_mesh(30.0, 30.0, 5.0));
    REQUIRE(sim.grab(0, Eigen::Vector2d(0.0, 0.0)));
    sim.move_grab(0, Eigen::Vector2d(5.0, 5.0));
    for (int guard = 0; guard < 100 && !sim.step(10).converged; ++guard) {
    }
    sim.release_grab(0);
    StepInfo info;
    for (int i = 0; i < 20 && !info.converged; ++i) info = sim.step(10);
    CHECK(info.converged);
    CHECK((sim.dofs() - Eigen::Map<const Eigen::VectorXd>(sim.mesh().rest_positions.data(),
                                                         sim.dofs().size()))
              .cwiseAbs()
              .maxCoeff() < 1e-6);
}

TEST_CASE("grabbing outside the sheet fails cleanly") {
    Simulation sim(make_rectangle_mesh(30.0, 30.0, 5.0));
    CHECK_FALSE(sim.grab(0, Eigen::Vector2d(100.0, 0.0)));
    CHECK(sim.grab_count() == 0);
}

TEST_CASE("extreme parameters and drags: no NaN, no inverted elements") {
    struct Case {
        double mu, tether, pull;
        bool pins;
    };
    const Case cases[] = {
        {0.05, 0.001, 200.0, true},    // absurd drag far off the sheet
        {1e3, 0.001, 30.0, true},      // extremely stiff skin
        {0.0, 0.0, 30.0, false},       // no material, no tethers, no pins (clamped floors)
        {0.05, 10.0, 30.0, true},      // glued-down skin
        {-1.0, -1.0, 30.0, true},      // nonsense (negative) input
    };
    for (const Case& c : cases) {
        Simulation sim(make_rectangle_mesh(40.0, 40.0, 4.0));
        TissueParams p;
        p.shear_modulus = c.mu;
        p.tether_stiffness = c.tether;
        p.pin_boundary = c.pins;
        sim.set_params(p);
        REQUIRE(sim.grab(0, Eigen::Vector2d(2.0, 2.0)));
        sim.move_grab(0, Eigen::Vector2d(2.0 + c.pull, 2.0));
        for (int frame = 0; frame < 60; ++frame) sim.step(5);  // one second at 60 fps

        CAPTURE(c.mu);  // doctest prints these values if a check below fails
        CAPTURE(c.pull);
        CHECK(sim.dofs().allFinite());
        CHECK(sim.min_area_ratio() > 0.0);
        CHECK(std::isfinite(sim.energy(sim.dofs())));
    }
}

TEST_CASE("changing parameters mid-simulation relaxes to the new equilibrium") {
    Simulation sim(make_rectangle_mesh(40.0, 40.0, 4.0));
    REQUIRE(sim.grab(0, Eigen::Vector2d(0.0, 0.0)));
    sim.move_grab(0, Eigen::Vector2d(6.0, 0.0));
    for (int guard = 0; guard < 100 && !sim.step(10).converged; ++guard) {
    }
    const int c = sim.mesh().vertex_count() / 2;
    const double soft_x = sim.positions()(0, c);

    TissueParams stiff = sim.params();
    stiff.shear_modulus *= 10.0;  // ten times stiffer skin
    sim.set_params(stiff);
    StepInfo info;
    for (int i = 0; i < 50 && !info.converged; ++i) info = sim.step(5);
    CHECK(info.converged);
    // Stiffer skin resists the same finger spring more: the point moves less.
    CHECK(sim.positions()(0, c) < soft_x);
}

namespace {

// Pull the centre of a 30 x 30 mm sheet with a constant force (the finger is
// placed far away, so the grab spring is saturated at max_force) and return
// how far the grabbed patch moved once at equilibrium.
double force_controlled_pull(double element_size, double force, double stiffening, double radius) {
    Simulation sim(make_rectangle_mesh(30.0, 30.0, element_size));
    TissueParams p;
    p.stiffening = stiffening;
    sim.set_params(p);
    GrabParams gp;
    gp.radius = radius;
    gp.max_force = force;
    sim.set_grab_params(gp);
    REQUIRE(sim.grab(0, Eigen::Vector2d(0.0, 0.0)));
    const Eigen::Vector2d start = sim.grab_info()[0].point;
    sim.move_grab(0, Eigen::Vector2d(1000.0, 0.0));  // far away: constant pull
    StepInfo info;
    for (int i = 0; i < 100 && !info.converged; ++i) info = sim.step(10);
    CHECK(info.converged);
    CHECK(sim.min_area_ratio() > 0.0);
    const GrabInfo g = sim.grab_info()[0];
    CHECK(g.force_limited);
    CHECK(g.force == doctest::Approx(force));
    return (g.point - start).norm();
}

}  // namespace

TEST_CASE("force cap: the finger pulls with at most max_force") {
    // Doubling the allowed force moves the skin further, but finitely.
    const double u1 = force_controlled_pull(2.0, 1.0, 8.0, 3.0);
    const double u2 = force_controlled_pull(2.0, 2.0, 8.0, 3.0);
    CHECK(u1 > 0.0);
    CHECK(u2 > u1);
    CHECK(u2 < 30.0);
}

TEST_CASE("stiffening limits how far the same force can stretch the skin") {
    const double force = 3.0;  // N
    const double u_nh = force_controlled_pull(2.0, force, 0.0, 3.0);
    const double u_exp = force_controlled_pull(2.0, force, 8.0, 3.0);
    MESSAGE("3 N pull: Neo-Hookean " << u_nh << " mm, stiffening (b=8) " << u_exp << " mm");
    CHECK(u_exp < 0.8 * u_nh);
}

TEST_CASE("spreading the grab over an area makes results converge with mesh refinement") {
    // A point force in a 2D continuum gives infinite displacement under the
    // point, so refining the mesh would keep changing the answer. A pull spread
    // over a fixed physical radius must converge: successive refinements should
    // change the result less and less.
    const double u_coarse = force_controlled_pull(2.5, 1.0, 8.0, 5.0);
    const double u_mid = force_controlled_pull(1.25, 1.0, 8.0, 5.0);
    const double u_fine = force_controlled_pull(0.625, 1.0, 8.0, 5.0);
    MESSAGE("patch pull displacement: " << u_coarse << " / " << u_mid << " / " << u_fine << " mm");
    CHECK(std::abs(u_fine - u_mid) < 0.6 * std::abs(u_mid - u_coarse));
    CHECK(std::abs(u_fine - u_mid) < 0.05 * u_fine);  // within 5% at the finest level
}

TEST_CASE("grab patch: weights sum to one and the skin doesn't jump when grabbed") {
    Simulation sim(make_rectangle_mesh(30.0, 30.0, 1.0));
    GrabParams gp;
    gp.radius = 3.0;
    sim.set_grab_params(gp);
    REQUIRE(sim.grab(0, Eigen::Vector2d(0.3, -0.2)));
    const GrabInfo g = sim.grab_info()[0];
    CHECK(g.radius == doctest::Approx(3.0));
    CHECK(g.force == doctest::Approx(0.0));  // no pull until the finger moves
    const StepInfo info = sim.step(5);
    CHECK(info.converged);
    CHECK(info.iterations == 0);
}

TEST_CASE("Woodbury grab solve keeps Newton's quadratic convergence") {
    // Small pull (no step clamping, no force cap) with a large patch: if the
    // low-rank grab correction in step() were wrong, Newton would still creep
    // towards the answer but would need many iterations instead of a few.
    Simulation sim(make_rectangle_mesh(30.0, 30.0, 1.0));
    GrabParams gp;
    gp.radius = 6.0;  // ~110 patch nodes
    gp.max_force = kUnlimited;
    sim.set_grab_params(gp);
    REQUIRE(sim.grab(0, Eigen::Vector2d(0.0, 0.0)));
    sim.move_grab(0, Eigen::Vector2d(0.3, 0.0));
    int iterations = 0;
    StepInfo info;
    for (int i = 0; i < 50 && !info.converged; ++i) {
        info = sim.step(1);
        iterations += info.iterations;
    }
    CHECK(info.converged);
    CHECK_FALSE(info.projected);
    CHECK(iterations <= 6);
}
