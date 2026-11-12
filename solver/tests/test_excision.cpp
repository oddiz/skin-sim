// Tests for excision: fusiform designs, removing pieces of skin cut free,
// the defect that results (gaping with prestress), closing it with sutures,
// dog-ears, and lesion margins.

#include <doctest/doctest.h>

#include <cmath>

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

// The app's tissue: collagen fibres and prestretch along horizontal RSTL.
TissueParams living_skin() {
    TissueParams p;
    p.stiffening = 4.0;
    p.fibre_angle = 0.0;
    p.fibre_stiffness = 0.05;
    p.fibre_nonlinearity = 5.0;
    p.fibre_dispersion = 0.15;
    p.prestretch_along = 1.08;
    p.prestretch_across = 1.03;
    return p;
}

double rest_area(const TriMesh& m) {
    double a = 0.0;
    for (const Triangle& t : m.triangles)
        a += signed_area(m.rest_positions.col(t[0]), m.rest_positions.col(t[1]), m.rest_positions.col(t[2]));
    return a;
}

double polygon_area(const Polyline& p) {  // shoelace formula, closed polyline
    double a = 0.0;
    for (size_t i = 0; i + 1 < p.size(); ++i) a += p[i].x() * p[i + 1].y() - p[i + 1].x() * p[i].y();
    return 0.5 * std::abs(a);
}

MaterialPoint at(const Simulation& sim, const V& p) {
    MaterialPoint m;
    REQUIRE(sim.material_point_at(p, m));
    return m;
}

// Half-width of the classic 30 x 10 mm fusiform along x at position x.
double half_width(double x) {
    const Polyline o = fusiform_outline(V(-15, 0), V(15, 0), 3.0, 30.0, 0.05);
    double best = 0.0, dist = 1e9;
    for (const V& p : o)
        if (p.y() > 0 && std::abs(p.x() - x) < dist) {
            dist = std::abs(p.x() - x);
            best = p.y();
        }
    return best;
}

// Current width of the defect (or closed wound) across x = x0, measured
// between the lip nodes nearest to that line on either side.
double defect_width(const Simulation& sim, double x0) {
    const TriMesh& m = sim.mesh();
    const auto x = sim.positions();
    std::vector<char> lip(m.vertex_count(), 0);
    for (const Edge& e : m.cut_edges) lip[e[0]] = lip[e[1]] = 1;
    int top = -1, bottom = -1;
    for (int v = 0; v < m.vertex_count(); ++v) {
        if (!lip[v]) continue;
        const V X = m.rest_positions.col(v);
        int& best = X.y() > 0 ? top : bottom;
        if (best < 0 || std::abs(X.x() - x0) < std::abs(m.rest_positions(0, best) - x0)) best = v;
    }
    REQUIRE(top >= 0);
    REQUIRE(bottom >= 0);
    return x(1, top) - x(1, bottom);
}

// A sheet with a fusiform excision of the given size, centred at the origin,
// long axis at `angle_deg`, on living skin; sutures placed across it (before
// it gapes) at the given positions along the axis, 3 mm bites.
struct Excision {
    Sheet sheet{kGeometry};
    Excision(double length, double ratio, double apex, double angle_deg, const std::vector<double>& sutures,
             double tightness = 0.0) {
        sheet.simulation().set_params(living_skin());
        const double a = angle_deg * 3.14159265358979323846 / 180.0;
        const V u(std::cos(a), std::sin(a)), n(-u.y(), u.x());
        const Polyline o = fusiform_outline(-0.5 * length * u, 0.5 * length * u, ratio, apex);
        REQUIRE(sheet.cut_rest(o) > 0);
        REQUIRE(sheet.excise_rest(V(0, 0)) == 1);
        Simulation& sim = sheet.simulation();
        for (double s : sutures) {
            // Half-width of this outline at s, from the outline itself.
            double hw = 0.0, dist = 1e9;
            for (const V& p : o) {
                const double along = p.dot(u), across = p.dot(n);
                if (across > 0 && std::abs(along - s) < dist) {
                    dist = std::abs(along - s);
                    hw = across;
                }
            }
            REQUIRE(sim.add_suture(at(sim, s * u + (hw + 3.0) * n), at(sim, s * u - (hw + 3.0) * n), tightness) > 0);
        }
        REQUIRE(settle(sim));
    }
    double total_tension() const {
        double t = 0.0;
        for (const SutureInfo& su : sheet.simulation().suture_info()) t += su.force;
        return t;
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// Design
// ---------------------------------------------------------------------------

TEST_CASE("fusiform outline: closed, 3:1, 30 degree tips") {
    const Polyline o = fusiform_outline(V(-15, 0), V(15, 0), 3.0, 30.0, 0.05);
    REQUIRE(o.size() > 100);
    CHECK((o.front() - o.back()).norm() == 0.0);
    double max_y = 0.0, min_x = 1e9, max_x = -1e9;
    for (const V& p : o) {
        max_y = std::max(max_y, std::abs(p.y()));
        min_x = std::min(min_x, p.x());
        max_x = std::max(max_x, p.x());
    }
    CHECK(max_y == doctest::Approx(5.0).epsilon(1e-6));  // width 10 = 30 / 3
    CHECK(min_x == doctest::Approx(-15.0));
    CHECK(max_x == doctest::Approx(15.0));
    // Tip angle: between the first edge on each side at tip a.
    const V d1 = (o[1] - o[0]).normalized(), d2 = (o[o.size() - 2] - o[0]).normalized();
    const double angle = std::acos(d1.dot(d2)) * 180.0 / 3.14159265358979323846;
    CHECK(angle == doctest::Approx(30.0).epsilon(0.03));
    // Fuller than a diamond with the same tips: the sides bulge.
    CHECK(polygon_area(o) > 0.5 * 30.0 * 10.0 * 1.1);
}

// ---------------------------------------------------------------------------
// Removing a piece of skin
// ---------------------------------------------------------------------------

TEST_CASE("only skin cut free all round can be excised") {
    Sheet s(kGeometry);
    REQUIRE(s.cut_rest({V(-20, 15), V(20, 15)}) > 0);  // a plain incision: nothing cut free
    CHECK(s.excise_rest(V(0, 14)) == -1);
    CHECK(s.excise_rest(V(80, 0)) == 0);  // off the sheet

    const Polyline o = fusiform_outline(V(-15, -5), V(15, -5), 3.0, 30.0);
    REQUIRE(s.cut_rest(o) > 0);
    CHECK(island_at(s.mesh(), V(0, -5)) == 1);
    CHECK(island_at(s.mesh(), V(0, 10)) == 0);
    const double before = rest_area(s.mesh());
    CHECK(before == doctest::Approx(6000.0));
    REQUIRE(s.excise_rest(V(0, -5)) == 1);
    CHECK(s.excision_count() == 1);
    // The piece is gone: its area, and every vertex strictly inside it.
    CHECK(before - rest_area(s.mesh()) == doctest::Approx(polygon_area(o)).epsilon(0.02));
    for (int v = 0; v < s.mesh().vertex_count(); ++v) {
        const V X = s.mesh().rest_positions.col(v) - V(0, -5);
        CHECK_FALSE((std::abs(X.x()) < 12.0 && std::abs(X.y()) < 0.8 * half_width(X.x()) - 0.2));
    }
    for (const Triangle& t : s.mesh().triangles)
        CHECK(signed_area(s.mesh().rest_positions.col(t[0]), s.mesh().rest_positions.col(t[1]),
                          s.mesh().rest_positions.col(t[2])) > 0.0);
    CHECK(s.excise_rest(V(0, -5)) == 0);  // nothing left to cut out there
}

TEST_CASE("an outline is cut and its inside removed in one step, or not at all") {
    Sheet s(kGeometry);
    // Drawn on the skin (current positions; here the sheet is at rest).
    CHECK(s.excise_outline(fusiform_outline(V(-10, 0), V(10, 0), 3.0, 30.0), V(0, 0)) == 1);
    CHECK(s.cut_count() == 1);
    CHECK(s.excision_count() == 1);
    // An open outline encloses nothing: no cut is left behind.
    CHECK(s.excise_outline({V(-10, 15), V(0, 20), V(10, 15)}, V(0, 17)) == -1);
    CHECK(s.cut_count() == 1);
}

TEST_CASE("excisions survive re-meshing, undo and clear") {
    Sheet s(kGeometry);
    REQUIRE(s.excise_outline(fusiform_outline(V(-12, 0), V(12, 0), 3.0, 30.0), V(0, 0)) == 1);
    const double with_hole = rest_area(s.mesh());
    REQUIRE(s.cut_rest({V(25, -15), V(25, 15)}) > 0);  // another incision: re-mesh
    CHECK(rest_area(s.mesh()) == doctest::Approx(with_hole));
    REQUIRE(s.undo_cut());
    CHECK(rest_area(s.mesh()) == doctest::Approx(with_hole));
    REQUIRE(s.undo_excise());
    CHECK(rest_area(s.mesh()) == doctest::Approx(6000.0));
    REQUIRE(s.excise_rest(V(0, 0)) == 1);
    s.clear_cuts();
    CHECK(s.excision_count() == 0);
    CHECK(rest_area(s.mesh()) == doctest::Approx(6000.0));
}

TEST_CASE("sutures and grabs on the removed piece go with it") {
    Sheet s(kGeometry);
    REQUIRE(s.cut_rest(fusiform_outline(V(-15, 0), V(15, 0), 3.0, 30.0)) > 0);
    Simulation& sim = s.simulation();
    REQUIRE(sim.add_suture(at(sim, V(-2, 1)), at(sim, V(2, -1))) > 0);    // inside the piece
    REQUIRE(sim.add_suture(at(sim, V(-30, 5)), at(sim, V(30, 5))) > 0);  // elsewhere
    REQUIRE(sim.grab(0, V(0, 0)));
    REQUIRE(s.excise_rest(V(0, 0)) == 1);
    CHECK(s.simulation().suture_count() == 1);
    CHECK(s.simulation().grab_count() == 0);
}

// ---------------------------------------------------------------------------
// The defect and its closure
// ---------------------------------------------------------------------------

TEST_CASE("prestressed skin retracts: the defect is larger than the skin removed") {
    // The pinned border keeps the sheet's outline, so the defect's current
    // area is the sheet's area minus the skin's current area.
    Excision e(30.0, 3.0, 30.0, 0.0, {});
    const Simulation& sim = e.sheet.simulation();
    const TriMesh& m = sim.mesh();
    const auto x = sim.positions();
    double skin = 0.0;
    for (const Triangle& t : m.triangles) skin += signed_area(x.col(t[0]), x.col(t[1]), x.col(t[2]));
    const double removed = 6000.0 - rest_area(m), defect = 6000.0 - skin;
    // Prestress is mostly along the RSTL (8% vs 3% across), so the defect
    // stretches along them and even narrows a little across, like a hole in
    // a sheet pulled one way; its area grows.
    MESSAGE("skin removed " << removed << " mm2, defect " << defect << " mm2, width at the middle "
                            << defect_width(sim, 0.0) << " mm (cut 10 mm)");
    CHECK(defect > 1.1 * removed);
}

TEST_CASE("sutures close the defect and press its lips together") {
    Excision e(30.0, 3.0, 30.0, 0.0, {-8, -4, 0, 4, 8});
    const Simulation& sim = e.sheet.simulation();
    const double gap = defect_width(sim, 0.0);
    const ContactInfo ci = sim.contact_info();
    MESSAGE("closed: gap " << gap << " mm, total tension " << e.total_tension() << " N, lip pressure "
                           << ci.max_pressure * 1000 << " kPa");
    CHECK(std::abs(gap) < 0.3);
    CHECK(ci.pairs > 0);
    for (const SutureInfo& su : sim.suture_info()) CHECK(su.force > 0.0);
    CHECK(sim.min_area_ratio() > 0.0);
}

TEST_CASE("an ellipse along the RSTL closes with less tension than across") {
    // RSTL horizontal (fibres and the larger prestretch along x). Along: the
    // long axis is horizontal, so the sutures pull across the RSTL.
    Excision along(30.0, 3.0, 30.0, 0.0, {-8, -4, 0, 4, 8});
    Excision across(30.0, 3.0, 30.0, 90.0, {-8, -4, 0, 4, 8});
    MESSAGE("total suture tension: along RSTL " << along.total_tension() << " N, across "
                                                << across.total_tension() << " N");
    CHECK(along.total_tension() < 0.9 * across.total_tension());
}

TEST_CASE("dog-ears: a short, blunt ellipse squeezes the skin at its ends") {
    // Same width (10 mm), closed with sutures over its middle part: a classic
    // 3:1 ellipse with 30 degree tips, and a blunt 1.5:1 one with 90 degree
    // tips. The excess skin at the ends of the blunt one has nowhere to go:
    // in-plane compression there (min principal stress < 0). Real skin
    // buckles out of the plane there instead: a dog-ear.
    auto worst_compression_at_tips = [](const Excision& e, double half_length) {
        const Simulation& sim = e.sheet.simulation();
        const Eigen::VectorXd s2 = sim.vertex_field(Field::MinPrincipalStress);
        double worst = 0.0;
        for (int v = 0; v < sim.mesh().vertex_count(); ++v) {
            const V X = sim.mesh().rest_positions.col(v);
            if ((X - V(half_length, 0)).norm() < 5.0 || (X - V(-half_length, 0)).norm() < 5.0)
                worst = std::min(worst, s2[v]);
        }
        return worst;
    };
    Excision classic(30.0, 3.0, 30.0, 0.0, {-9, -6, -3, 0, 3, 6, 9});
    Excision blunt(15.0, 1.5, 90.0, 0.0, {-4.5, -1.5, 1.5, 4.5});
    const double c = worst_compression_at_tips(classic, 15.0), b = worst_compression_at_tips(blunt, 7.5);
    MESSAGE("compression at the ends: 3:1 ellipse " << c * 1000 << " kPa, 1.5:1 " << b * 1000 << " kPa");
    CHECK(b < 0.0);
    CHECK(b < 1.5 * c);  // markedly more compression (both negative)
}

// ---------------------------------------------------------------------------
// Lesions and margins
// ---------------------------------------------------------------------------

TEST_CASE("lesion margins: complete, incomplete, not excised") {
    Sheet s(kGeometry);
    s.add_lesion_rest(V(0, 0), 6.0);    // inside the ellipse: 5 - 3 = 2 mm margin
    s.add_lesion_rest(V(12, 0), 6.0);   // near the tip: cut through
    s.add_lesion_rest(V(30, 15), 6.0);  // elsewhere
    CHECK_FALSE(s.lesion_status(0).excised);
    REQUIRE(s.excise_outline(fusiform_outline(V(-15, 0), V(15, 0), 3.0, 30.0), V(0, 0)) == 1);
    const Sheet::LesionStatus a = s.lesion_status(0), b = s.lesion_status(1), c = s.lesion_status(2);
    MESSAGE("margins: centred " << a.margin << " mm, near the tip " << b.margin << " mm");
    CHECK(a.excised);
    CHECK(a.complete);
    CHECK(a.margin == doctest::Approx(2.0).epsilon(0.1));
    CHECK(b.excised);
    CHECK_FALSE(b.complete);
    CHECK(b.margin < 0.0);
    CHECK_FALSE(c.excised);
    CHECK(s.undo_lesion());
    CHECK(s.lesions().size() == 2);
}
