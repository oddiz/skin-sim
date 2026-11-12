// Tests for contact between incision lips: the geometry kernels (signed gap,
// continuous collision detection), the assembled energy and its derivatives,
// and the behaviour that motivated it: tight knots squeeze tissue, and lips
// never pass through each other.

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <random>

#include "skinsim/contact.hpp"
#include "skinsim/sheet.hpp"

using namespace skinsim;

namespace {

using V = Eigen::Vector2d;

const SheetGeometry kGeometry{100.0, 60.0, 2.5};

bool settle(Simulation& sim) {
    for (int frame = 0; frame < 300; ++frame)
        if (sim.step(20).converged) return true;
    return false;
}

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

// Which side of the vertical incision a vertex belongs to: -1 left, +1 right.
// On the incision both lips share rest positions, so look at its triangles.
std::vector<int> sides(const TriMesh& m) {
    std::vector<int> side(m.vertex_count(), 0);
    for (const Triangle& t : m.triangles) {
        const double cx = (m.rest_positions(0, t[0]) + m.rest_positions(0, t[1]) + m.rest_positions(0, t[2])) / 3.0;
        for (int v : t)
            if (side[v] == 0) side[v] = cx < 0 ? -1 : 1;
    }
    return side;
}

// How deep the lips have gone into each other, mm: the deepest any lip node
// lies inside a triangle it does not belong to (distance to that triangle's
// nearest edge). Independent of the contact code, so it can judge it.
double deepest_intrusion(const Simulation& sim) {
    const TriMesh& m = sim.mesh();
    const auto x = sim.positions();
    std::vector<char> on_lip(m.vertex_count(), 0);
    for (const Edge& e : m.cut_edges) on_lip[e[0]] = on_lip[e[1]] = 1;
    double deepest = 0.0;
    for (int v = 0; v < m.vertex_count(); ++v) {
        if (!on_lip[v]) continue;
        const V p = x.col(v);
        for (const Triangle& t : m.triangles) {
            if (t[0] == v || t[1] == v || t[2] == v) continue;
            double depth = 1e300;  // distance to the nearest edge, > 0 inside
            for (int k = 0; k < 3; ++k) {
                const V a = x.col(t[k]), b = x.col(t[(k + 1) % 3]);
                const V e = b - a;
                depth = std::min(depth, (e.x() * (p - a).y() - e.y() * (p - a).x()) / e.norm());
            }
            deepest = std::max(deepest, depth);
        }
    }
    return deepest;
}

// The vertical incision's lips overlap by this much, mm: the largest amount
// by which a left-lip node lies to the right of its twin on the right lip
// (twins: the two copies of one rest point). Unlike deepest_intrusion it is
// not limited to the size of one element.
double twin_overlap(const Simulation& sim) {
    const TriMesh& m = sim.mesh();
    const std::vector<int> side = sides(m);
    const auto x = sim.positions();
    double worst = 0.0;
    for (int a = 0; a < m.vertex_count(); ++a) {
        if (side[a] >= 0 || std::abs(m.rest_positions(0, a)) > 1e-9) continue;
        for (int b = 0; b < m.vertex_count(); ++b)
            if (side[b] > 0 && (m.rest_positions.col(b) - m.rest_positions.col(a)).norm() < 1e-9)
                worst = std::max(worst, x(0, a) - x(0, b));
    }
    return worst;
}

// Right lip pushed left into the left one by up to `depth` mm (a smooth bump
// along the incision), plus a little noise: a state with lips overlapping.
Eigen::VectorXd pressed_together(const Simulation& sim, double depth, unsigned seed) {
    const std::vector<int> side = sides(sim.mesh());
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> noise(-0.01, 0.01);
    Eigen::VectorXd x = sim.dofs();
    for (int v = 0; v < sim.mesh().vertex_count(); ++v) {
        const V X = sim.mesh().rest_positions.col(v);
        if (side[v] > 0 && std::abs(X.y()) < 15.0) x[2 * v] -= depth * (1.0 - (X.y() / 15.0) * (X.y() / 15.0));
        x[2 * v] += noise(rng);
        x[2 * v + 1] += noise(rng);
    }
    return x;
}

}  // namespace

// ---------------------------------------------------------------------------
// Geometry kernels
// ---------------------------------------------------------------------------

TEST_CASE("signed gap: sign convention and value") {
    // Segment along +x: tissue on the left (above), wound below.
    const V u(0, 0), v(2, 0);
    CHECK(contact::signed_gap(V(1, -0.3), u, v) == doctest::Approx(0.3));   // in the wound
    CHECK(contact::signed_gap(V(1, 0.2), u, v) == doctest::Approx(-0.2));   // inside the tissue
    CHECK(contact::projection(V(1.5, 0.2), u, v) == doctest::Approx(0.75));
}

TEST_CASE("signed gap: gradient and Hessian match finite differences") {
    std::mt19937 rng(11);
    std::uniform_real_distribution<double> r(-2.0, 2.0);
    for (int trial = 0; trial < 20; ++trial) {
        contact::Vec6 q;
        for (int i = 0; i < 6; ++i) q[i] = r(rng);
        if ((q.segment<2>(4) - q.segment<2>(2)).norm() < 0.5) continue;  // keep the segment non-degenerate
        auto g_of = [](const contact::Vec6& z, contact::Vec6* grad = nullptr) {
            return contact::signed_gap(z.segment<2>(0), z.segment<2>(2), z.segment<2>(4), grad);
        };
        contact::Vec6 grad;
        contact::Mat6 hess;
        contact::signed_gap(q.segment<2>(0), q.segment<2>(2), q.segment<2>(4), &grad, &hess);
        CHECK((hess - hess.transpose()).norm() < 1e-12);
        const double h = 1e-6;
        for (int i = 0; i < 6; ++i) {
            contact::Vec6 qp = q, qm = q;
            qp[i] += h;
            qm[i] -= h;
            CHECK(grad[i] == doctest::Approx((g_of(qp) - g_of(qm)) / (2 * h)).epsilon(1e-6));
            contact::Vec6 gp, gm;
            g_of(qp, &gp);
            g_of(qm, &gm);
            CHECK((hess.col(i) - (gp - gm) / (2 * h)).norm() < 1e-6 * (1.0 + hess.col(i).norm()));
        }
    }
}

TEST_CASE("continuous collision detection: crossings, misses and parallel motion") {
    const V u(0, 0), v(2, 0);  // tissue above (y > 0)
    double t = -1.0;
    // Node rising straight through the middle: crosses inward at t = 0.25.
    CHECK(contact::crosses_inward(V(1, -1), V(1, 3), u, u, v, v, &t));
    CHECK(t == doctest::Approx(0.25));
    // Coming out of the tissue: not an inward crossing.
    CHECK_FALSE(contact::crosses_inward(V(1, 3), V(1, -1), u, u, v, v));
    // Passing the line beyond the segment's end: a miss.
    CHECK_FALSE(contact::crosses_inward(V(3, -1), V(3, 1), u, u, v, v));
    // Sliding parallel to the segment, in front of it: never on the line.
    CHECK_FALSE(contact::crosses_inward(V(-1, -0.5), V(3, -0.5), u, u, v, v));
    // Segment moving down onto a stationary node: same as the node moving up.
    CHECK(contact::crosses_inward(V(1, 0), V(1, 0), V(0, 1), V(0, -1), V(2, 1), V(2, -1), &t));
    CHECK(t == doctest::Approx(0.5));
    // Straight through a 1 mm strip of skin (tissue between y = 0 and 1):
    // in through its lower edge, out through its upper one, in time order.
    const V a(0, 1), b(2, 1);  // upper edge, oriented so the tissue is below
    contact::Crossing c[2];
    REQUIRE(contact::crossings(V(1, -1), V(1, 3), u, u, v, v, c) == 1);
    CHECK(c[0].inward);
    CHECK(c[0].time == doctest::Approx(0.25));
    REQUIRE(contact::crossings(V(1, -1), V(1, 3), b, b, a, a, c) == 1);
    CHECK_FALSE(c[0].inward);
    CHECK(c[0].time == doctest::Approx(0.5));
    // Segment rotating about u past a node: a genuinely quadratic case.
    CHECK(contact::crosses_inward(V(1, 0.5), V(1, 0.5), u, u, V(2, 2), V(2, -2), &t));
    CHECK(t > 0.0);
    CHECK(t < 0.5);
}

// ---------------------------------------------------------------------------
// Assembled energy
// ---------------------------------------------------------------------------

TEST_CASE("lips pressed together: gradient and Hessian match finite differences") {
    Sheet s = vertical_cut();
    Simulation& sim = s.simulation();
    TissueParams p;
    p.pin_boundary = false;
    sim.set_params(p);
    ContactParams cp;
    cp.stiffness = 2.0;  // keeps the numbers comparable to the skin's
    sim.set_contact_params(cp);
    const Eigen::VectorXd x = pressed_together(sim, 0.15, 3);

    const Eigen::VectorXd g = sim.gradient(x);
    const Eigen::MatrixXd H = Eigen::MatrixXd(sim.hessian(x, false));
    CHECK((H - H.transpose()).norm() < 1e-10);
    // The contact really is active in this state.
    ContactParams off = cp;
    off.enabled = false;
    Simulation& s2 = sim;
    const double with_contact = s2.energy(x);
    s2.set_contact_params(off);
    const double without = s2.energy(x);
    s2.set_contact_params(cp);
    CHECK(with_contact > without);

    const double h = 1e-6;
    int checked = 0;
    for (int i = 0; i < x.size(); ++i) {
        if (sim.mesh().rest_positions.col(i / 2).norm() > 8.0) continue;
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

TEST_CASE("touching lips cost nothing: a closed incision is unchanged by contact") {
    Sheet s = vertical_cut();
    Simulation& sim = s.simulation();
    ContactParams off;
    off.enabled = false;
    const double e_on = sim.energy(sim.dofs());
    const Eigen::VectorXd g_on = sim.gradient(sim.dofs());
    sim.set_contact_params(off);
    // Equal up to rounding: the touching lips' gaps are 0 to ~1e-16 mm.
    CHECK(sim.energy(sim.dofs()) == doctest::Approx(e_on).epsilon(1e-14));
    CHECK((sim.gradient(sim.dofs()) - g_on).norm() < 1e-12);
    sim.set_contact_params(ContactParams{});
    CHECK(sim.contact_info().max_pressure < 1e-9);
}

// ---------------------------------------------------------------------------
// Behaviour
// ---------------------------------------------------------------------------

TEST_CASE("tight knots press the lips together and squeeze the tissue in the loop") {
    double previous = 1.0;
    for (double t : {0.0, 0.2, 0.4}) {
        CAPTURE(t);
        for (bool enabled : {false, true}) {
            Sheet s = vertical_cut();
            Simulation& sim = s.simulation();
            ContactParams cp;
            cp.enabled = enabled;
            sim.set_contact_params(cp);
            REQUIRE(sim.add_suture(at(sim, V(-3, 0)), at(sim, V(3, 0)), t) > 0);
            REQUIRE(settle(sim));
            const double overlap = std::max(deepest_intrusion(sim), twin_overlap(sim));
            const double loop = sim.suture_info().at(0).loop_stress;
            const ContactInfo ci = sim.contact_info();
            MESSAGE("tightness " << t * 100 << "%, contact " << (enabled ? "on" : "off") << ": overlap " << overlap
                                 << " mm, loop stress " << loop * 1000 << " kPa, lip pressure "
                                 << ci.max_pressure * 1000 << " kPa, tension " << sim.suture_info().at(0).force
                                 << " N");
            if (!enabled) {
                if (t > 0.0) CHECK(overlap > 1.0);  // without contact the lips pass through each other
                continue;
            }
            // A penalty leaves overlap = pressure / stiffness, and nothing
            // more: no node slipped through.
            CHECK(overlap < 0.2);  // was 2.5 mm at 40% without contact
            CHECK(overlap <= 1.1 * ci.max_overlap + 1e-6);
            CHECK(ci.max_overlap == doctest::Approx(ci.max_pressure / sim.contact_params().stiffness));
            if (t == 0.0) {
                CHECK(std::abs(loop) < 1e-6);
            } else {
                CHECK(loop < previous);  // squeezed, more so when tighter
                CHECK(ci.pairs > 0);
                CHECK(ci.total_force > 0.0);
            }
            previous = loop;
        }
    }
    CHECK(previous < 0.0);
}

TEST_CASE("forceps cannot push one lip through the other") {
    Sheet s = vertical_cut();
    Simulation& sim = s.simulation();
    REQUIRE(sim.grab(0, V(2, 0)));     // right lip
    sim.move_grab(0, V(-10, 0));        // far past the left lip
    settle(sim);
    const double overlap = deepest_intrusion(sim);
    MESSAGE("pushed 12 mm: overlap " << overlap << " mm, lip pressure " << sim.contact_info().max_pressure * 1000
                                     << " kPa");
    CHECK(overlap < 0.1);
    CHECK(sim.min_area_ratio() > 0.0);
    CHECK(sim.contact_info().pairs > 0);
}

TEST_CASE("a step that would jump right through a strip of skin is caught") {
    // A 2 mm strip between two incisions, with the wound on its left held
    // 3 mm open. Contact measures how deep a lip node is inside other skin,
    // so it can only miss a node that went right through the strip and out
    // the other side, into the open wound: "tunnelling".
    Sheet s(kGeometry);
    REQUIRE(s.cut_rest({V(0, -15), V(0, 15)}) > 0);
    REQUIRE(s.cut_rest({V(2, -15), V(2, 15)}) > 0);
    const Simulation& sim = s.simulation();
    const TriMesh& m = sim.mesh();
    std::vector<int> part(m.vertex_count(), 0);  // -1 left, 0 strip, +1 right
    for (const Triangle& t : m.triangles) {
        const double cx = (m.rest_positions(0, t[0]) + m.rest_positions(0, t[1]) + m.rest_positions(0, t[2])) / 3.0;
        for (int v : t) part[v] = cx < 0.0 ? -1 : cx > 2.0 ? 1 : 0;
    }
    Eigen::VectorXd open = sim.dofs();
    for (int v = 0; v < m.vertex_count(); ++v)
        if (part[v] < 0) open[2 * v] -= 3.0;
    auto right_moved = [&](double dx) {
        Eigen::VectorXd x = open;
        for (int v = 0; v < m.vertex_count(); ++v)
            if (part[v] > 0 && std::abs(m.rest_positions(1, v)) < 10.0) x[2 * v] -= dx;
        return x;
    };
    CHECK_FALSE(sim.tunnels(open, right_moved(0.1)));  // pressing into the strip: seen and handled
    CHECK_FALSE(sim.tunnels(open, right_moved(1.0)));  // deep in the strip: still seen
    CHECK(sim.tunnels(open, right_moved(3.5)));        // through the strip and out: caught
}

TEST_CASE("a lip slides along a pressed lip") {
    Sheet s = vertical_cut();
    Simulation& sim = s.simulation();
    for (double y : {-8.0, 0.0, 8.0}) REQUIRE(sim.add_suture(at(sim, V(-3, y)), at(sim, V(3, y)), 0.2) > 0);
    REQUIRE(settle(sim));
    const int before = sim.contact_info().pairs;
    REQUIRE(sim.grab(0, V(-4, 0)));
    for (int k = 1; k <= 10; ++k) {  // drag the left side 5 mm along the incision
        sim.move_grab(0, V(-4, 0.5 * k));
        settle(sim);
    }
    const double overlap = deepest_intrusion(sim);
    MESSAGE("slid 5 mm: overlap " << overlap << " mm, pairs " << before << " -> " << sim.contact_info().pairs);
    CHECK(sim.step(20).converged);
    CHECK(overlap < 0.05);
    CHECK(overlap <= 1.1 * sim.contact_info().max_overlap + 1e-6);
    CHECK(sim.contact_info().pairs > 0);
}

TEST_CASE("contact at its stiffest stays stable") {
    Sheet s = vertical_cut();
    Simulation& sim = s.simulation();
    ContactParams cp;
    cp.stiffness = 1e9;  // clamped to the maximum
    sim.set_contact_params(cp);
    CHECK(sim.contact_params().stiffness == 1e3);
    REQUIRE(sim.add_suture(at(sim, V(-3, 0)), at(sim, V(3, 0)), 0.4) > 0);
    REQUIRE(sim.grab(0, V(3, 5)));
    sim.move_grab(0, V(-5, 5));
    settle(sim);
    CHECK(sim.dofs().allFinite());
    CHECK(sim.min_area_ratio() > 0.0);
    CHECK(deepest_intrusion(sim) < 0.01);
}

TEST_CASE("contact survives re-meshing") {
    Sheet s = vertical_cut();
    ContactParams cp;
    cp.stiffness = 7.0;
    s.simulation().set_contact_params(cp);
    REQUIRE(s.cut_rest({V(30, -10), V(30, 10)}) > 0);
    CHECK(s.simulation().contact_params().stiffness == 7.0);
}

// Opt-in timing check (run with
//   build\skinsim_tests<suffix>.exe --no-skip -tc="*cost of contact*"
// ). A flap-sized scene: several incisions, sutures pressing lips together,
// forceps pulling. Reports the time per Newton iteration with and without
// contact.
TEST_CASE("timing: cost of contact" * doctest::skip()) {
    for (bool enabled : {false, true}) {
        Sheet s(SheetGeometry{100.0, 60.0, 2.5});
        for (double x : {-20.0, 0.0, 20.0}) REQUIRE(s.cut_rest({V(x, -18), V(x + 4, 18)}) > 0);
        REQUIRE(s.cut_rest({V(-35, 0), V(35, 2)}) > 0);
        Simulation& sim = s.simulation();
        ContactParams cp;
        cp.enabled = enabled;
        sim.set_contact_params(cp);
        for (double y : {-12.0, 10.0}) REQUIRE(sim.add_suture(at(sim, V(-2, y)), at(sim, V(3, y)), 0.2) > 0);
        REQUIRE(sim.grab(0, V(12, -8)));
        sim.move_grab(0, V(4, -8));
        const auto t0 = std::chrono::steady_clock::now();
        int iterations = 0, projected = 0, cuts = 0;
        StepInfo info;
        for (int frame = 0; frame < 300; ++frame) {
            info = sim.step(1);
            iterations += info.iterations;
            projected += info.projected;
            cuts += info.line_search_cuts;
            if (info.converged) break;
        }
        MESSAGE("projected " << projected << " of " << iterations << ", line search cuts " << cuts);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        CHECK(info.converged);
        CHECK_FALSE(info.stalled);
        MESSAGE("contact " << int(enabled) << ": " << sim.mesh().vertex_count() << " nodes, "
                           << sim.mesh().cut_edges.size() << " lip edges, " << iterations << " iterations, "
                           << ms / std::max(1, iterations) << " ms per iteration");
    }
}
