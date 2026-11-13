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

// Smallest distance from p to the closed polygon's edges.
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

TEST_CASE("flap designs: the defect contains the lesion circle; degenerate requests are refused") {
    for (FlapType type : {FlapType::Advancement, FlapType::Rotation, FlapType::Rhomboid}) {
        CAPTURE(static_cast<int>(type));
        const V c(3, -2);
        const FlapDesign d = design_flap(type, c, V(1, 1), 10.0);
        REQUIRE(d.valid);
        CHECK(inside_polygon(d.defect, c));
        CHECK(edge_distance(d.defect, c) >= 5.0 - 1e-9);  // the whole circle fits
        CHECK((d.defect.front() - d.defect.back()).norm() < 1e-12);  // closed
        CHECK_FALSE(d.incisions.empty());
        CHECK_FALSE(d.key_sutures.empty());
        // The flap lies on the side it was asked for (towards +x+y).
        V centroid = V::Zero();
        for (size_t i = 0; i + 1 < d.flap.size(); ++i) centroid += d.flap[i];
        centroid /= static_cast<double>(d.flap.size() - 1);
        if (type != FlapType::Rotation) CHECK((centroid - c).dot(V(1, 1)) > 0.0);
        // Key sutures: the entry on the flap, the exit outside the defect,
        // both a bite away from the outline.
        for (const auto& [entry, exit] : d.key_sutures) {
            CHECK_FALSE(inside_polygon(d.defect, entry));
            CHECK_FALSE(inside_polygon(d.defect, exit));
            CHECK(edge_distance(d.defect, exit) > 1.0);
        }
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
        for (size_t i = 0; i + 1 < d.flap.size(); ++i) {
            const V moved = rotate_about(d.flap[i], d.pivot, sign * d.turn_deg);
            for (size_t j = 0; j + 1 < d.defect.size(); ++j)
                if ((moved - d.defect[j]).norm() < 1e-9) ++matched;
        }
        if (matched == 4) fits = true;
    }
    CHECK(fits);
}

TEST_CASE("make_flap: cuts, removes the defect and undermines in one step; undo_flap reverses it") {
    Sheet s(SheetGeometry{120.0, 90.0, 2.0});
    for (FlapType type : {FlapType::Advancement, FlapType::Rotation, FlapType::Rhomboid}) {
        CAPTURE(static_cast<int>(type));
        const FlapDesign d = design_flap(type, V(0, 0), V(1, 0), 10.0);
        const int vertices_before = s.mesh().vertex_count();
        REQUIRE(s.make_flap(d, 6.0) == 1);
        CHECK(s.cut_count() == 1);
        CHECK(s.excision_count() == 1);
        CHECK(s.flap_count() == 1);
        CHECK(island_at(s.mesh(), V(0, 0)) == -1);  // the defect's skin is gone
        CHECK(s.undermining_at(d.pivot + 0.5 * (d.flap[1] - d.pivot)) == doctest::Approx(1.0));  // the flap
        CHECK(s.undermining_at(V(55, 40)) == 0.0);  // far away
        CHECK(s.mesh().cut_edges.size() > 0);
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
    for (FlapType type : {FlapType::Advancement, FlapType::Rotation, FlapType::Rhomboid}) {
        CAPTURE(static_cast<int>(type));
        Sheet s(SheetGeometry{120.0, 90.0, 2.0});
        s.simulation().set_params(app_tissue());
        const FlapDesign d = design_flap(type, V(0, 0), V(1, 0), 10.0);
        REQUIRE(s.make_flap(d, 6.0) == 1);
        settle(s.simulation());
        const std::vector<int> ids = s.tie_key_sutures(0, 0.0, 4);
        REQUIRE(ids.size() == d.key_sutures.size());
        settle(s.simulation());
        Simulation& sim = s.simulation();
        CHECK(sim.min_area_ratio() > 0.0);
        // Each key suture has brought its needle holes together: the gap
        // left is about the two bites (2 + 2 mm), from several mm apart.
        for (const SutureInfo& su : sim.suture_info()) {
            CAPTURE(su.id);
            MESSAGE("key suture " << su.id << ": closing " << su.force << " N, needle holes " << (su.a - su.b).norm()
                                  << " mm apart, load " << su.load * 100 << "% of 4-0");
            CHECK((su.a - su.b).norm() < 6.0);
            CHECK(su.force > 0.0);
        }
        MESSAGE("flap base stress " << s.flap_base_stress(0) * 1000.0 << " kPa");
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
