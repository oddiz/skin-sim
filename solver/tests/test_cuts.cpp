// Tests for incisions: the cut graph, CDT meshing, splitting the lips, and
// carrying the simulation state across re-meshing.

#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <random>
#include <set>

#include "skinsim/cuts.hpp"
#include "skinsim/mesher.hpp"
#include "skinsim/sheet.hpp"

using namespace skinsim;

namespace {

using V = Eigen::Vector2d;

const SheetGeometry kGeometry{100.0, 60.0, 2.5};

double total_area(const TriMesh& m) {
    double a = 0.0;
    for (const Triangle& t : m.triangles)
        a += signed_area(m.rest_positions.col(t[0]), m.rest_positions.col(t[1]), m.rest_positions.col(t[2]));
    return a;
}

bool all_ccw(const TriMesh& m) {
    for (const Triangle& t : m.triangles)
        if (!(signed_area(m.rest_positions.col(t[0]), m.rest_positions.col(t[1]), m.rest_positions.col(t[2])) > 0.0))
            return false;
    return true;
}

double worst_angle(const TriMesh& m) {
    double worst = 180.0;
    for (const Triangle& t : m.triangles)
        worst = std::min(worst, min_angle_deg(m.rest_positions.col(t[0]), m.rest_positions.col(t[1]),
                                              m.rest_positions.col(t[2])));
    return worst;
}

// V - E + F: 1 for a disk, one less for every hole (an interior slit is a hole).
int euler(const TriMesh& m) {
    return m.vertex_count() - static_cast<int>(unique_edges(m).size()) + m.triangle_count();
}

// How many mesh vertices sit at rest position p (2 = the two lips of a cut).
int copies_at(const TriMesh& m, const V& p) {
    int n = 0;
    for (int v = 0; v < m.vertex_count(); ++v)
        if ((m.rest_positions.col(v) - p).norm() < 1e-9) ++n;
    return n;
}

// Step until the solver reports equilibrium (or give up after many frames).
void settle(Simulation& sim) {
    for (int frame = 0; frame < 100; ++frame)
        if (sim.step(20).converged) return;
}

// Largest distance between two vertices that share a rest position: how far
// apart the lips of the incisions have moved.
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

}  // namespace

TEST_CASE("uncut sheet: CDT mesh is valid, covers the rectangle and has good triangles") {
    const TriMesh m = mesh_sheet(kGeometry, CutGraph{});
    CHECK(all_ccw(m));
    CHECK(total_area(m) == doctest::Approx(100.0 * 60.0).epsilon(1e-12));
    CHECK(euler(m) == 1);  // a disk: no holes, no duplicated or missing triangles
    CHECK(m.cut_edges.empty());
    MESSAGE("uncut: " << m.vertex_count() << " vertices, worst angle " << worst_angle(m) << " deg");
    CHECK(worst_angle(m) > 25.0);
}

TEST_CASE("a straight incision splits the mesh along it") {
    Sheet s(kGeometry);
    const int added = s.cut_rest({V(-15, 0), V(15, 0)});
    CHECK(added == 24);  // 30 mm at 1.25 mm spacing
    const TriMesh& m = s.mesh();
    CHECK(all_ccw(m));
    CHECK(total_area(m) == doctest::Approx(6000.0).epsilon(1e-12));
    CHECK(euler(m) == 0);  // the slit is a hole
    CHECK(m.cut_edges.size() == 48);  // each cut segment has two lips
    // The tips stay single vertices; every point in between has two copies.
    CHECK(copies_at(m, V(-15, 0)) == 1);
    CHECK(copies_at(m, V(15, 0)) == 1);
    for (double x = -13.75; x < 14.0; x += 1.25) CHECK(copies_at(m, V(x, 0)) == 2);
    MESSAGE("cut: worst angle " << worst_angle(m) << " deg");
    CHECK(worst_angle(m) > 20.0);
}

TEST_CASE("pulling across an incision opens it; the same pull leaves uncut skin closed") {
    Sheet cut(kGeometry);
    REQUIRE(cut.cut_rest({V(-15, 0), V(15, 0)}) > 0);
    Sheet whole(kGeometry);
    for (Sheet* s : {&cut, &whole}) {
        REQUIRE(s->simulation().grab(0, V(0, 4)));
        s->simulation().move_grab(0, V(0, 10));
        settle(s->simulation());
        CHECK(s->simulation().min_area_ratio() > 0.0);
    }
    MESSAGE("gap after 6 mm pull: " << max_gap(cut) << " mm");
    CHECK(max_gap(cut) > 1.0);
    CHECK(max_gap(whole) == 0.0);  // no lips at all
}

TEST_CASE("forceps next to an incision only pull their own lip") {
    Sheet s(kGeometry);
    REQUIRE(s.cut_rest({V(-15, 0), V(15, 0)}) > 0);
    GrabParams gp;
    gp.radius = 3.0;  // reaches across the incision
    s.simulation().set_grab_params(gp);
    REQUIRE(s.simulation().grab(0, V(0, 1.0)));
    s.simulation().move_grab(0, V(0, 4.0));
    settle(s.simulation());

    const TriMesh& m = s.mesh();
    const auto x = s.simulation().positions();
    // Two copies at the origin: one moved with the forceps, one barely.
    double moved_most = 0.0, moved_least = 1e9;
    for (int v = 0; v < m.vertex_count(); ++v) {
        if ((m.rest_positions.col(v) - V(0, 0)).norm() > 1e-9) continue;
        const double d = (x.col(v) - m.rest_positions.col(v)).norm();
        moved_most = std::max(moved_most, d);
        moved_least = std::min(moved_least, d);
    }
    MESSAGE("grabbed lip moved " << moved_most << " mm, other lip " << moved_least << " mm");
    CHECK(moved_most > 1.0);
    CHECK(moved_least < 0.2 * moved_most);
}

TEST_CASE("crossing incisions meet at a shared point with four flap corners") {
    Sheet s(kGeometry);
    REQUIRE(s.cut_rest({V(-10, 0), V(10, 0)}) > 0);
    REQUIRE(s.cut_rest({V(0, -10), V(0, 10)}) > 0);
    const TriMesh& m = s.mesh();
    CHECK(all_ccw(m));
    CHECK(total_area(m) == doctest::Approx(6000.0).epsilon(1e-12));
    CHECK(copies_at(m, V(0, 0)) == 4);
    CHECK(is_valid(s.cuts(), kGeometry.cut_bounds(), 0.01));
}

TEST_CASE("a stroke starting near the end of a cut snaps onto it (Z-plasty limb)") {
    Sheet s(kGeometry);
    REQUIRE(s.cut_rest({V(-10, 0), V(10, 0)}) > 0);
    // Start 0.9 mm away from the end of the first cut.
    REQUIRE(s.cut_rest({V(10.6, 0.7), V(17, 8)}) > 0);
    // The old tip is now a corner joining two limbs: one vertex inside the
    // angle between them, one outside.
    CHECK(copies_at(s.mesh(), V(10, 0)) == 2);
    CHECK(copies_at(s.mesh(), V(10.6, 0.7)) == 0);
}

TEST_CASE("incisions are clipped to stay inside the sheet") {
    Sheet s(kGeometry);
    REQUIRE(s.cut_rest({V(-80, 0), V(0, 0)}) > 0);
    for (const V& p : s.cuts().points) CHECK(p.x() >= -50.0 + kGeometry.cut_margin() - 1e-9);
}

TEST_CASE("taps, empty, invalid and off-sheet strokes cut nothing") {
    Sheet s(kGeometry);
    const int n0 = s.mesh().vertex_count();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    CHECK(s.cut_rest({}) == 0);
    CHECK(s.cut_rest({V(1, 1)}) == 0);
    CHECK(s.cut_rest({V(1, 1), V(1.5, 1)}) == 0);  // shorter than the cut spacing
    CHECK(s.cut_rest({V(nan, 0), V(nan, 5)}) == 0);
    CHECK(s.cut_rest({V(60, -40), V(90, 40)}) == 0);  // entirely off the sheet
    CHECK(s.cut({V(200, 0), V(300, 0)}) == 0);
    CHECK(s.mesh().vertex_count() == n0);
    CHECK(s.cut_count() == 0);
    // A NaN in the middle just splits the stroke.
    CHECK(s.cut_rest({V(-20, 5), V(-10, 5), V(nan, nan), V(10, 5), V(20, 5)}) > 0);
}

TEST_CASE("retracing an existing incision adds nothing") {
    Sheet s(kGeometry);
    REQUIRE(s.cut_rest({V(-15, 0), V(15, 0)}) > 0);
    CHECK(s.cut_rest({V(-10, 0.2), V(10, -0.2)}) == 0);
    CHECK(s.cut_count() == 1);
}

TEST_CASE("undo and clear restore earlier meshes") {
    Sheet s(kGeometry);
    const int n0 = s.mesh().vertex_count();
    REQUIRE(s.cut_rest({V(-15, 0), V(15, 0)}) > 0);
    const int n1 = s.mesh().vertex_count();
    REQUIRE(s.cut_rest({V(-15, 10), V(15, 10)}) > 0);
    CHECK(s.cut_count() == 2);
    CHECK(s.undo_cut());
    CHECK(s.mesh().vertex_count() == n1);
    s.clear_cuts();
    CHECK(s.mesh().vertex_count() == n0);
    CHECK(s.cut_count() == 0);
    CHECK_FALSE(s.undo_cut());
}

TEST_CASE("random strokes always give a valid mesh and a solvable simulation") {
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> px(-58.0, 58.0), py(-36.0, 36.0), step(-15.0, 15.0);
    std::uniform_int_distribution<int> count(2, 7);
    Sheet s(kGeometry);
    double worst = 180.0;
    for (int stroke = 0; stroke < 25; ++stroke) {
        CAPTURE(stroke);
        Polyline path{V(px(rng), py(rng))};
        const int n = count(rng);
        for (int k = 1; k < n; ++k) path.push_back(path.back() + V(step(rng), step(rng)));
        s.cut_rest(path);
        const TriMesh& m = s.mesh();
        REQUIRE(all_ccw(m));
        CHECK(total_area(m) == doctest::Approx(6000.0).epsilon(1e-9));
        CHECK(is_valid(s.cuts(), kGeometry.cut_bounds(), 0.01));
        CHECK(std::isfinite(s.simulation().energy(s.simulation().dofs())));
        worst = std::min(worst, worst_angle(m));
    }
    MESSAGE(s.cut_count() << " strokes cut, " << s.mesh().vertex_count() << " vertices, worst angle " << worst
                          << " deg");
    CHECK(worst > 5.0);
    // Pull on the shredded sheet: must stay finite and uninverted.
    REQUIRE(s.simulation().grab(0, V(0.3, 0.2)));
    s.simulation().move_grab(0, V(8, 6));
    settle(s.simulation());
    CHECK(s.simulation().min_area_ratio() > 0.0);
}

TEST_CASE("re-meshing carries an affine deformation over exactly") {
    Sheet s(kGeometry);
    TissueParams p;
    p.pin_boundary = false;  // pins would hold the border at rest
    s.simulation().set_params(p);
    REQUIRE(s.cut_rest({V(-15, 0), V(15, 0)}) > 0);

    Eigen::Matrix2d A;
    A << 1.1, 0.2, -0.05, 0.95;
    const V b(3.0, -2.0);
    auto affine = [&](const TriMesh& m) {
        Eigen::VectorXd x(2 * m.vertex_count());
        for (int v = 0; v < m.vertex_count(); ++v) x.segment<2>(2 * v) = A * m.rest_positions.col(v) + b;
        return x;
    };
    s.simulation().set_dofs(affine(s.mesh()));
    REQUIRE(s.cut_rest({V(-10, -15), V(10, 15)}) > 0);  // crosses the first one
    CHECK((s.simulation().dofs() - affine(s.mesh())).cwiseAbs().maxCoeff() < 1e-9);
}

TEST_CASE("a stroke drawn on deformed skin is mapped back to rest coordinates") {
    Sheet s(kGeometry);
    TissueParams p;
    p.pin_boundary = false;
    s.simulation().set_params(p);
    Eigen::Matrix2d A;
    A << 1.2, 0.1, 0.0, 0.9;
    Eigen::VectorXd x(2 * s.mesh().vertex_count());
    for (int v = 0; v < s.mesh().vertex_count(); ++v) x.segment<2>(2 * v) = A * s.mesh().rest_positions.col(v);
    s.simulation().set_dofs(x);

    // A straight stroke on the stretched skin along the image of the x axis.
    REQUIRE(s.cut({A * V(-10, 0), A * V(10, 0)}) > 0);
    for (const V& q : s.cuts().points) {
        CHECK(std::abs(q.y()) < 1e-9);
        CHECK(std::abs(q.x()) <= 10.0 + 1e-9);
    }
}

TEST_CASE("a held grab survives a cut elsewhere") {
    Sheet s(kGeometry);
    Simulation* sim = &s.simulation();
    REQUIRE(sim->grab(3, V(20, 0)));
    sim->move_grab(3, V(25, 0));
    settle(*sim);
    const double before = sim->grab_info().at(0).force;

    REQUIRE(s.cut_rest({V(-20, -10), V(-20, 10)}) > 0);
    sim = &s.simulation();  // a new simulation now (new mesh)
    REQUIRE(sim->grab_count() == 1);
    settle(*sim);
    const double after = sim->grab_info().at(0).force;
    MESSAGE("grab force before cut " << before << " N, after " << after << " N");
    CHECK(after == doctest::Approx(before).epsilon(0.05));

    // The finger keeps working: pulling further pulls harder.
    sim->move_grab(3, V(28, 0));
    settle(*sim);
    CHECK(sim->grab_info().at(0).force > after);
}

TEST_CASE("cut graph: crossing strokes are split at the crossing") {
    CutGraph g;
    CutOptions o;
    o.spacing = 1.0;
    o.snap = 0.4;
    o.endpoint_snap = 1.5;
    REQUIRE(add_cut(g, {V(-5, 0), V(5, 0)}, o) > 0);
    REQUIRE(add_cut(g, {V(0.3, -5), V(0.3, 5)}, o) > 0);  // crosses near, not at, a point
    CHECK(is_valid(g, o.bounds, 0.01));
    // A point at the crossing exists and both strokes pass through it.
    int at_crossing = -1;
    for (int k = 0; k < static_cast<int>(g.points.size()); ++k)
        if ((g.points[k] - V(0.3, 0)).norm() < 0.45) at_crossing = k;
    REQUIRE(at_crossing >= 0);
    int degree = 0;
    for (const Edge& e : g.segments) degree += (e[0] == at_crossing || e[1] == at_crossing);
    CHECK(degree == 4);
}

TEST_CASE("simplify keeps corners and drops jitter") {
    Polyline zigzag;
    for (int i = 0; i <= 20; ++i) zigzag.emplace_back(i, (i % 2) * 0.01);  // jittery straight line
    for (int i = 1; i <= 10; ++i) zigzag.emplace_back(20, i);              // then a right angle
    const Polyline s = simplify(zigzag, 0.1);
    REQUIRE(s.size() == 3);
    CHECK((s[1] - V(20, 0)).norm() < 0.02);
}

// Opt-in stress test (skipped by default; run with
//   build\skinsim_tests<suffix>.exe --no-skip -tc="*adversarial*"
// ). Throws thousands of adversarial strokes at the cutter: hairpins,
// near-retraces of earlier cuts, tiny offsets, closed loops, all while the
// skin is being pulled. CDT terminates the program on bad input, so
// "finishes at all" is the main check.
TEST_CASE("stress: adversarial strokes never break the mesher" * doctest::skip()) {
    int total = 0;
    for (int seed = 0; seed < 40; ++seed) {
        std::mt19937 rng(1000 + seed);
        std::uniform_real_distribution<double> u(-1.0, 1.0);
        std::uniform_int_distribution<int> kind(0, 4), count(2, 12);
        const double h = (seed % 3 == 0) ? 1.5 : (seed % 3 == 1 ? 2.5 : 4.0);
        Sheet s(SheetGeometry{80.0, 50.0, h});
        if (seed % 2) {
            s.simulation().grab(0, V(5 * u(rng), 5 * u(rng)));
            s.simulation().move_grab(0, V(10 * u(rng), 10 * u(rng)));
            s.simulation().step(10);
        }
        for (int stroke = 0; stroke < 40; ++stroke) {
            CAPTURE(seed);
            CAPTURE(stroke);
            Polyline path;
            const int k = kind(rng);
            if (k == 0 || s.cuts().points.empty()) {  // random wiggle
                path.push_back(V(45 * u(rng), 30 * u(rng)));
                for (int i = 1; i < count(rng); ++i) path.push_back(path.back() + V(8 * u(rng), 8 * u(rng)));
            } else if (k == 1) {  // retrace an existing cut with a tiny offset
                const double off = 0.05 * h * u(rng);
                for (int i = 0; i < 6; ++i) {
                    const V p = s.cuts().points[(stroke * 7 + i * 3) % s.cuts().points.size()];
                    path.push_back(p + V(off, -off));
                }
            } else if (k == 2) {  // hairpin
                const V a(40 * u(rng), 25 * u(rng)), d(10 * u(rng), 10 * u(rng));
                path = {a, a + d, a + 0.02 * V(-d.y(), d.x()), a + d * 0.5};
            } else if (k == 3) {  // closed loop
                const V c(35 * u(rng), 20 * u(rng));
                const double r = 2 + 6 * std::abs(u(rng));
                for (int i = 0; i <= 24; ++i)
                    path.push_back(c + r * V(std::cos(i * 0.2618), std::sin(i * 0.2618)));
            } else {  // start/end near existing points, at many scales
                const V p = s.cuts().points[stroke % s.cuts().points.size()];
                path = {p + V(h * u(rng), h * u(rng)), p + V(12 * u(rng), 12 * u(rng))};
            }
            if (stroke % 2) s.cut(path); else s.cut_rest(path);
            if (stroke % 9 == 8) s.undo_cut();
            const TriMesh& m = s.mesh();
            REQUIRE(all_ccw(m));
            REQUIRE(total_area(m) == doctest::Approx(80.0 * 50.0).epsilon(1e-9));
            REQUIRE(std::isfinite(s.simulation().energy(s.simulation().dofs())));
            s.simulation().step(3);
            ++total;
        }
    }
    MESSAGE(total << " strokes survived");
}
