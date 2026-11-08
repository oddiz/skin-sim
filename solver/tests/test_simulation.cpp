// Tests for the assembled simulation: derivatives, Newton behaviour, robustness.

#include <doctest/doctest.h>

#include <cmath>
#include <random>

#include "skinsim/simulation.hpp"

using namespace skinsim;

namespace {

// A small sheet in a non-trivial state: interior nodes jiggled randomly so
// every element has a different, generic deformation.
Simulation jiggled_sheet(double amplitude) {
    Simulation sim(make_rectangle_mesh(24.0, 16.0, 4.0));
    TissueParams p;
    p.tether_stiffness = 0.002;
    p.pin_boundary = false;
    sim.set_params(p);

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
    const Simulation sim = jiggled_sheet(0.4);
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

TEST_CASE("assembled Hessian matches finite differences of gradient") {
    const Simulation sim = jiggled_sheet(0.4);
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

TEST_CASE("sheet at rest is in equilibrium with zero energy") {
    Simulation sim(make_rectangle_mesh(40.0, 30.0, 5.0));
    const StepInfo info = sim.step(5);
    CHECK(info.converged);
    CHECK(info.iterations == 0);
    CHECK(info.energy == doctest::Approx(0.0));
}

TEST_CASE("pulling the centre: energy decreases monotonically and Newton converges") {
    Simulation sim(make_rectangle_mesh(40.0, 40.0, 4.0));
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
