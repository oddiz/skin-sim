// Tests for local flaps: the designs' geometry (flaps.hpp), carrying a design
// out on the sheet in one undoable step (Sheet::make_flap), and the
// mechanics surgeons know from experience.

#include <doctest/doctest.h>

#include <cmath>

#include "skinsim/sheet.hpp"

using namespace skinsim;

namespace {

using V = Eigen::Vector2d;
constexpr double kPi = 3.14159265358979323846;

// Smallest distance from p to the polyline's segments.
double edge_distance(const Polyline& poly, const V& p) {
    double d = 1e300;
    for (size_t i = 0; i + 1 < poly.size(); ++i) d = std::min(d, point_segment_distance(p, poly[i], poly[i + 1]));
    return d;
}

V rotate_about(const V& p, const V& centre, double deg) {
    const double a = deg * kPi / 180.0;
    const Eigen::Matrix2d R = (Eigen::Matrix2d() << std::cos(a), -std::sin(a), std::sin(a), std::cos(a)).finished();
    return centre + R * (p - centre);
}

// Every flap type.
std::vector<FlapType> all_types() {
    std::vector<FlapType> t;
    for (int i = 0; i < kFlapTypeCount; ++i) t.push_back(static_cast<FlapType>(i));
    return t;
}

bool segments_cross(const V& a, const V& b, const V& c, const V& d) {
    auto cross = [](const V& p, const V& q) { return p.x() * q.y() - p.y() * q.x(); };
    const double d1 = cross(b - a, c - a), d2 = cross(b - a, d - a);
    const double d3 = cross(d - c, a - c), d4 = cross(d - c, b - c);
    return ((d1 > 1e-9 && d2 < -1e-9) || (d1 < -1e-9 && d2 > 1e-9)) &&
           ((d3 > 1e-9 && d4 < -1e-9) || (d3 < -1e-9 && d4 > 1e-9));
}

// A closed polygon whose edges do not cross each other.
bool simple(const Polyline& poly) {
    const size_t n = poly.size() - 1;
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 2; j < n; ++j) {
            if (i == 0 && j == n - 1) continue;  // neighbours through the closing point
            if (segments_cross(poly[i], poly[i + 1], poly[j], poly[j + 1])) return false;
        }
    return true;
}

double area(const Polyline& poly) {
    double a = 0.0;
    for (size_t i = 0; i + 1 < poly.size(); ++i) a += poly[i].x() * poly[i + 1].y() - poly[i + 1].x() * poly[i].y();
    return 0.5 * std::abs(a);
}

V vertex_mean(const Polyline& closed) {
    V g = V::Zero();
    for (size_t i = 0; i + 1 < closed.size(); ++i) g += closed[i];
    return g / static_cast<double>(closed.size() - 1);
}

void settle(Simulation& sim) {
    for (int frame = 0; frame < 300; ++frame)
        if (sim.step(20).converged) return;
}

// The app's tissue: prestressed skin with RSTL fibres along x (as in the
// presets), wrinkling on, lip contact on.
TissueParams app_tissue() {
    TissueParams p;
    p.fibre_stiffness = 0.05;
    p.fibre_nonlinearity = 5.0;
    p.fibre_dispersion = 0.1;
    p.prestretch_along = 1.08;
    p.prestretch_across = 1.03;
    p.wrinkling = 0.9;
    return p;
}

// Make a flap on a fresh sheet, tie its key sutures, let it settle.
// Returns the largest key-suture closing force, N.
double close_with_flap(FlapType type, const FlapShape& shape, const V& direction, double diameter = 10.0) {
    Sheet s(SheetGeometry{120.0, 90.0, 2.0});
    s.simulation().set_params(app_tissue());
    const FlapDesign d = design_flap(type, V(0, 0), direction, diameter, shape, 2.0);
    REQUIRE(s.make_flap(d, 6.0) == 1);
    settle(s.simulation());
    const std::vector<int> ids = s.tie_key_sutures(0, 0.0, 0);
    REQUIRE(ids.size() == d.key_sutures.size());
    settle(s.simulation());
    CHECK(s.simulation().min_area_ratio() > 0.0);
    double worst = 0.0;
    for (const SutureInfo& su : s.simulation().suture_info()) worst = std::max(worst, su.force);
    return worst;
}

}  // namespace

TEST_CASE("flap designs: the defect contains the lesion circle; outlines are simple; degenerate requests are refused") {
    for (FlapType type : all_types()) {
        CAPTURE(static_cast<int>(type));
        const V c(3, -2);
        const FlapDesign d = design_flap(type, c, V(10, 10), 10.0);
        REQUIRE(d.valid);
        CHECK_FALSE(d.incisions.empty());
        CHECK_FALSE(d.key_sutures.empty());
        REQUIRE_FALSE(d.flaps.empty());
        for (const Polyline& f : d.flaps) {
            CHECK((f.front() - f.back()).norm() < 1e-12);
            CHECK(simple(f));
            CHECK(area(f) > 10.0);
        }
        for (const Polyline& b : d.burow) CHECK(simple(b));
        // All the lines that will be cut.
        std::vector<const Polyline*> cuts;
        if (!d.defect.empty()) cuts.push_back(&d.defect);
        for (const Polyline& p : d.incisions) cuts.push_back(&p);
        for (const Polyline& p : d.burow) cuts.push_back(&p);
        auto distance_to_cuts = [&](const V& p) {
            double m = 1e300;
            for (const Polyline* poly : cuts) m = std::min(m, edge_distance(*poly, p));
            return m;
        };
        // Key sutures: both needle holes on skin that stays (outside the
        // defect and the triangles), a good part of a bite from every cut.
        for (const auto& [a, b] : d.key_sutures) {
            for (const V& p : {a, b}) {
                CAPTURE(p.transpose());
                if (!d.defect.empty()) CHECK_FALSE(inside_polygon(d.defect, p));
                for (const Polyline& t : d.burow) CHECK_FALSE(inside_polygon(t, p));
                CHECK(distance_to_cuts(p) > 0.9);
            }
        }
        if (type == FlapType::ZPlasty) {
            CHECK(d.defect.empty());
            CHECK_FALSE(design_flap(type, c, V(0, 0), 10.0).valid);
            continue;
        }
        CHECK(simple(d.defect));
        CHECK(inside_polygon(d.defect, c));
        CHECK(edge_distance(d.defect, c) >= 5.0 - 1e-9);  // the whole circle fits
        CHECK((d.defect.front() - d.defect.back()).norm() < 1e-12);  // closed
        // The flap lies on the side it was asked for (towards +x+y), for
        // the one-sided designs (the rotation pivots sideways).
        const bool one_sided = type == FlapType::Advancement || type == FlapType::Rhomboid ||
                               type == FlapType::VYIsland || type == FlapType::Keystone ||
                               type == FlapType::Dufourmentel || type == FlapType::Bilobed;
        if (one_sided) CHECK((vertex_mean(d.flaps[0]) - c).dot(V(1, 1)) > 0.0);
        CHECK_FALSE(design_flap(type, c, V(0, 0), 10.0).valid);
        CHECK_FALSE(design_flap(type, c, V(1, 0), 0.0).valid);
    }
}

TEST_CASE("advancement flap: square defect, limbs length x width long") {
    FlapShape shape;
    shape.length_to_width = 2.5;
    const FlapDesign d = design_flap(FlapType::Advancement, V(0, 0), V(0, 1), 10.0, shape);
    REQUIRE(d.incisions.size() == 2);
    for (const Polyline& limb : d.incisions) {
        CHECK((limb.back() - limb.front()).norm() == doctest::Approx(25.0));
        CHECK(std::abs((limb.back() - limb.front()).normalized().y()) == doctest::Approx(1.0));  // along the drag
    }
    CHECK(d.length_to_width == doctest::Approx(2.5));
    CHECK(edge_distance(d.defect, V(0, 0)) == doctest::Approx(5.0));
    CHECK(d.burow.empty());
    // With Burow's triangles: one beside each limb's end, outside the flap,
    // the limb stopping where its triangle starts.
    shape.burow = true;
    const FlapDesign b = design_flap(FlapType::Advancement, V(0, 0), V(0, 1), 10.0, shape);
    REQUIRE(b.burow.size() == 2);
    for (size_t i = 0; i < 2; ++i) {
        CHECK((b.incisions[i].back() - b.burow[i][0]).norm() < 1e-12);
        CHECK_FALSE(inside_polygon(b.flaps[0], vertex_mean(b.burow[i])));
    }
}

TEST_CASE("rotation flap: turning the sector about the apex closes the triangle") {
    FlapShape shape;
    shape.arc_angle_deg = 110.0;
    const FlapDesign d = design_flap(FlapType::Rotation, V(0, 0), V(1, 0), 10.0, shape);
    REQUIRE(d.incisions.size() == 1);
    const Polyline& arc = d.incisions[0];
    const V A = d.pivot;
    const double r = (arc.front() - A).norm();
    for (const V& p : arc) CHECK((p - A).norm() == doctest::Approx(r));
    // The arc starts at a base corner of the triangle; turning it by the
    // apex angle about A lands it on the other base corner, closing the defect.
    const V B = arc.front();
    bool lands = false;
    for (double sign : {1.0, -1.0}) {
        const V moved = rotate_about(B, A, sign * d.turn_deg);
        for (const V& corner : d.defect)
            if ((moved - corner).norm() < 1e-9 && (corner - B).norm() > 1e-6) lands = true;
    }
    CHECK(lands);
    CHECK(d.turn_deg > 10.0);
    CHECK(d.turn_deg < 60.0);
}

TEST_CASE("rhomboid flap: 60/120 degree rhombus, and the flap is a copy that turns 60 degrees into it") {
    const FlapDesign d = design_flap(FlapType::Rhomboid, V(0, 0), V(0, 1), 10.0);
    REQUIRE(d.defect.size() == 5);
    // Sides equal, angles 60 / 120.
    const double s = (d.defect[1] - d.defect[0]).norm();
    for (int i = 0; i < 4; ++i) {
        const V a = d.defect[(i + 3) % 4], b = d.defect[i], c = d.defect[(i + 1) % 4];
        CHECK((c - b).norm() == doctest::Approx(s));
        const double angle = std::acos((a - b).normalized().dot((c - b).normalized())) * 180.0 / kPi;
        CHECK((std::abs(angle - 60.0) < 1e-6 || std::abs(angle - 120.0) < 1e-6));
    }
    CHECK(edge_distance(d.defect, V(0, 0)) == doctest::Approx(5.0));  // inscribed circle
    // Turning the flap about the pivot by 60 degrees (one way or the other)
    // puts every corner of the flap onto a corner of the defect.
    bool fits = false;
    for (double sign : {1.0, -1.0}) {
        int matched = 0;
        const Polyline& flap = d.flaps[0];
        for (size_t i = 0; i + 1 < flap.size(); ++i) {
            const V moved = rotate_about(flap[i], d.pivot, sign * d.turn_deg);
            for (size_t j = 0; j + 1 < d.defect.size(); ++j)
                if ((moved - d.defect[j]).norm() < 1e-9) ++matched;
        }
        if (matched == 4) fits = true;
    }
    CHECK(fits);
}

TEST_CASE("Dufourmentel: rhombus of the chosen angle, a smaller turn than Limberg's 60 degrees") {
    for (double alpha : {60.0, 70.0, 80.0}) {
        FlapShape shape;
        shape.rhombus_angle_deg = alpha;
        const FlapDesign d = design_flap(FlapType::Dufourmentel, V(0, 0), V(0, 1), 10.0, shape);
        const V A = d.defect[0], B = d.defect[1], D = d.defect[3];
        const double at_A = std::acos((B - A).normalized().dot((D - A).normalized())) * 180.0 / kPi;
        CHECK(at_A == doctest::Approx(alpha));
        MESSAGE("Dufourmentel " << alpha << " deg: turns " << d.turn_deg << " deg");
        CHECK(d.turn_deg < 60.0);
        CHECK(edge_distance(d.defect, V(0, 0)) == doctest::Approx(5.0));
    }
}

TEST_CASE("Z-plasty: lengthening grows with the limb angle (about 25 / 50 / 75 / 100%)") {
    double previous = 0.0;
    for (double angle : {30.0, 45.0, 60.0, 75.0, 90.0}) {
        FlapShape shape;
        shape.z_angle_deg = angle;
        const FlapDesign d = design_flap(FlapType::ZPlasty, V(0, 0), V(20, 0), 0.0, shape);
        REQUIRE(d.valid);
        MESSAGE("Z-plasty " << angle << " deg: lengthening " << d.lengthening * 100 << "%");
        CHECK(d.lengthening > previous);
        previous = d.lengthening;
        // The three limbs are equally long.
        const Polyline& z = d.incisions[0];
        CHECK((z[1] - z[0]).norm() == doctest::Approx(20.0));
        CHECK((z[2] - z[1]).norm() == doctest::Approx(20.0));
        CHECK((z[3] - z[2]).norm() == doctest::Approx(20.0));
    }
    FlapShape sixty;
    CHECK(design_flap(FlapType::ZPlasty, V(0, 0), V(20, 0), 0.0, sixty).lengthening ==
          doctest::Approx(0.732).epsilon(0.01));
}

TEST_CASE("make_flap: cuts, removes the defect and undermines in one step; undo_flap reverses it") {
    Sheet s(SheetGeometry{120.0, 90.0, 2.0});
    for (FlapType type : all_types()) {
        CAPTURE(static_cast<int>(type));
        FlapShape shape;
        shape.burow = true;  // (advancement and H-plasty)
        shape.length_to_width = 2.0;
        const FlapDesign d = design_flap(type, V(0, 0), V(12, 0), 10.0, shape);
        const int vertices_before = s.mesh().vertex_count();
        REQUIRE(s.make_flap(d, 6.0) == 1);
        CHECK(s.cut_count() == 1);
        const int removed = (d.defect.empty() ? 0 : 1) + static_cast<int>(d.burow.size());
        CHECK(s.excision_count() == removed);
        CHECK(s.flap_count() == 1);
        if (!d.defect.empty()) CHECK(island_at(s.mesh(), V(0, 0)) == -1);  // the defect's skin is gone
        for (const Polyline& t : d.burow) CHECK(island_at(s.mesh(), vertex_mean(t)) == -1);  // the triangles too
        // Island flaps are still there, cut free all round, not undermined
        // in the middle (their pedicle); other flaps are undermined.
        for (const Polyline& f : d.flaps) {
            const V g = vertex_mean(f);
            if (!inside_polygon(f, g)) continue;  // (a crescent's centroid may lie outside)
            if (d.islands.empty()) {
                CHECK(s.undermining_at(g) > 0.5);
            } else {
                CHECK(island_at(s.mesh(), g) == 1);
                CHECK(s.undermining_at(g) < 0.5);
            }
        }
        CHECK(s.undermining_at(V(57, 42)) == 0.0);  // far away
        REQUIRE(s.undo_flap());
        CHECK(s.cut_count() == 0);
        CHECK(s.excision_count() == 0);
        CHECK(s.flap_count() == 0);
        CHECK(s.undermine_count() == 0);
        CHECK(s.mesh().cut_edges.empty());
        CHECK(s.mesh().vertex_count() == vertices_before);
    }
    CHECK_FALSE(s.undo_flap());
    // A design off the sheet cuts nothing.
    CHECK(s.make_flap(design_flap(FlapType::Rhomboid, V(500, 500), V(1, 0), 10.0), 6.0) == 0);
    CHECK(s.cut_count() == 0);
}

TEST_CASE("flaps close their defects with the key sutures") {
    for (FlapType type : all_types()) {
        CAPTURE(static_cast<int>(type));
        Sheet s(SheetGeometry{120.0, 90.0, 2.0});
        s.simulation().set_params(app_tissue());
        FlapShape shape;
        shape.length_to_width = 2.0;
        const FlapDesign d = design_flap(type, V(0, 0), V(12, 0), 10.0, shape);
        REQUIRE(s.make_flap(d, 6.0) == 1);
        settle(s.simulation());
        const std::vector<int> ids = s.tie_key_sutures(0, 0.0, 4);
        REQUIRE(ids.size() == d.key_sutures.size());
        settle(s.simulation());
        Simulation& sim = s.simulation();
        CHECK(sim.min_area_ratio() > 0.0);
        // Each key suture has brought its needle holes together: the gap
        // left is about the two bites (2 + 2 mm), from several mm apart.
        // (One may end up slack, the others having done its work.)
        double most = 0.0;
        for (const SutureInfo& su : sim.suture_info()) {
            CAPTURE(su.id);
            MESSAGE("type " << static_cast<int>(type) << " key suture " << su.id << ": closing " << su.force
                            << " N, needle holes " << (su.a - su.b).norm() << " mm apart, load " << su.load * 100
                            << "% of 4-0");
            CHECK((su.a - su.b).norm() < 6.0);
            CHECK(su.force >= 0.0);
            most = std::max(most, su.force);
        }
        CHECK(most > 0.0);
        MESSAGE("type " << static_cast<int>(type) << " flap base stress " << s.flap_base_stress(0) * 1000.0
                        << " kPa");
        CHECK(std::isfinite(s.flap_base_stress(0)));
    }
}

TEST_CASE("surgeons' rules of thumb: a longer arc and a longer advancement flap close with less tension") {
    FlapShape short_arc, long_arc;
    short_arc.arc_angle_deg = 70.0;
    long_arc.arc_angle_deg = 150.0;
    const double f_short = close_with_flap(FlapType::Rotation, short_arc, V(1, 0));
    const double f_long = close_with_flap(FlapType::Rotation, long_arc, V(1, 0));
    MESSAGE("rotation flap: arc 70 deg -> " << f_short << " N, arc 150 deg -> " << f_long << " N");
    CHECK(f_long < f_short);

    FlapShape short_flap, long_flap;
    short_flap.length_to_width = 1.0;
    long_flap.length_to_width = 3.0;
    const double a_short = close_with_flap(FlapType::Advancement, short_flap, V(0, 1));
    const double a_long = close_with_flap(FlapType::Advancement, long_flap, V(0, 1));
    MESSAGE("advancement flap: 1:1 -> " << a_short << " N, 3:1 -> " << a_long << " N");
    CHECK(a_long < a_short);
}

TEST_CASE("relative stress: intact prestressed skin reads zero; a pulled sheet does not") {
    Sheet s(SheetGeometry{60.0, 40.0, 3.0});
    s.simulation().set_params(app_tissue());
    settle(s.simulation());
    const Eigen::VectorXd absolute = s.simulation().vertex_field(Field::MaxPrincipalStress);
    const Eigen::VectorXd relative = s.simulation().vertex_field(Field::MaxPrincipalStress, true);
    CHECK(absolute.minCoeff() > 1e-3);           // prestress: tension everywhere
    CHECK(relative.cwiseAbs().maxCoeff() < 1e-6);  // but nothing added
    // The resting stress is the prestress: along the RSTL more than across.
    const Eigen::Matrix2d rest = s.simulation().resting_stress();
    CHECK(rest(0, 0) > rest(1, 1));
    CHECK(rest(1, 1) > 0.0);
    // Principal directions: the largest along x (the RSTL) at rest.
    const std::vector<PrincipalStress> p = s.simulation().triangle_principal(false);
    CHECK(std::abs(p[0].dir1.x()) == doctest::Approx(1.0).epsilon(1e-6));
}

