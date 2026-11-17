// Tests for markings (marks.cpp): lines drawn on the skin at rest, mapped to
// where that tissue is now. They must follow the skin, stop at incision lips
// and never be drawn across a wound or over an excised hole.

#include <doctest/doctest.h>

#include <cmath>

#include "skinsim/excision.hpp"
#include "skinsim/sheet.hpp"

using namespace skinsim;

namespace {

using V = Eigen::Vector2d;

const SheetGeometry kGeometry{100.0, 60.0, 2.0};

void settle(Simulation& sim) {
    for (int frame = 0; frame < 300; ++frame)
        if (sim.step(20).converged) return;
}

size_t point_count(const std::vector<Polyline>& pieces) {
    size_t n = 0;
    for (const Polyline& p : pieces) n += p.size();
    return n;
}

}  // namespace

TEST_CASE("markings: uncut stretched skin gives one dense piece on the tissue") {
    Sheet s(kGeometry);
    Simulation& sim = s.simulation();
    REQUIRE(sim.grab(0, V(0, 0)));
    sim.move_grab(0, V(6, 4));
    settle(sim);
    const Polyline line{V(-20, -10), V(20, 10)};
    const auto pieces = sim.map_rest_polyline(line, 0.5);
    REQUIRE(pieces.size() == 1);
    CHECK(pieces[0].size() >= 89);  // 44.7 mm in steps of 0.5 mm
    // Every sample is exactly where the tissue at that rest point is now.
    const V d = line[1] - line[0];
    for (size_t k = 0; k < pieces[0].size(); ++k) {
        const double t = static_cast<double>(k) / (pieces[0].size() - 1);
        V now;
        REQUIRE(sim.current_position_at_rest(line[0] + t * d, now));
        CHECK((pieces[0][k] - now).norm() < 1e-9);
    }
    // The ends sit where the end points are now.
    V a;
    REQUIRE(sim.current_position_at_rest(line[0], a));
    CHECK((pieces[0].front() - a).norm() < 1e-9);
}

TEST_CASE("markings: a line across an open incision splits at the lips") {
    Sheet s(kGeometry);
    REQUIRE(s.cut_rest({V(-15, 0), V(15, 0)}) > 0);
    Simulation& sim = s.simulation();
    REQUIRE(sim.grab(0, V(0, 4)));
    sim.move_grab(0, V(0, 10));  // pull the upper lip up: the wound gapes
    settle(sim);
    const Polyline line{V(2, -10), V(2, 10)};
    const auto pieces = sim.map_rest_polyline(line, 0.5);
    REQUIRE(pieces.size() == 2);
    // One piece ends on the lower lip, the other starts on the upper lip:
    // the gap between them is the open wound, and nothing is drawn in it.
    const V lower_end = pieces[0].back(), upper_start = pieces[1].front();
    MESSAGE("lips at x=2: " << lower_end.y() << " and " << upper_start.y());
    CHECK(upper_start.y() - lower_end.y() > 1.0);
    for (const Polyline& p : pieces)
        for (const V& q : p) CHECK_FALSE((q.y() > lower_end.y() + 1e-6 && q.y() < upper_start.y() - 1e-6));
    // The lip ends are the tissue right at the incision, each on its own side.
    CHECK(pieces[0].front().y() < pieces[0].back().y());
    CHECK(pieces[1].front().y() < pieces[1].back().y());
}

TEST_CASE("markings: a drawn point exactly on an incision splits the line there") {
    Sheet s(kGeometry);
    REQUIRE(s.cut_rest({V(-15, 0), V(15, 0)}) > 0);
    Simulation& sim = s.simulation();
    REQUIRE(sim.grab(0, V(0, 4)));
    sim.move_grab(0, V(0, 10));
    settle(sim);
    // The middle point is on the cut (as when a finger's sample lands on it).
    const auto pieces = sim.map_rest_polyline({V(2, -10), V(2, 0), V(2, 10)}, 0.5);
    REQUIRE(pieces.size() == 2);
    CHECK(pieces[1].front().y() - pieces[0].back().y() > 1.0);
}

TEST_CASE("markings: a line along an incision keeps to one lip (planned with the marker, then cut)") {
    Sheet s(kGeometry);
    REQUIRE(s.cut_rest({V(-15, 0), V(15, 0)}) > 0);
    Simulation& sim = s.simulation();
    REQUIRE(sim.grab(0, V(0, 4)));
    sim.move_grab(0, V(0, 10));
    settle(sim);
    // Drawn left to right exactly on the cut: it follows the lip on its left
    // (the upper one), never zigzagging across the open wound.
    const auto pieces = sim.map_rest_polyline({V(-10, 0), V(10, 0)}, 0.5);
    REQUIRE(pieces.size() == 1);
    double worst = 1e9;
    for (const V& q : pieces[0])
        if (std::abs(q.x()) < 5.0) worst = std::min(worst, q.y());
    MESSAGE("lowest point of the line over the middle of the wound: " << worst);
    CHECK(worst > 2.0);  // the upper lip has been pulled about 4.6 mm up there
}

TEST_CASE("markings: a line past the tip of an incision is not split") {
    Sheet s(kGeometry);
    REQUIRE(s.cut_rest({V(-15, 0), V(15, 0)}) > 0);
    const auto pieces = s.simulation().map_rest_polyline({V(20, -10), V(20, 10)}, 0.5);
    CHECK(pieces.size() == 1);
}

TEST_CASE("markings: a line across an excised ellipse has no points over the hole") {
    Sheet s(kGeometry);
    REQUIRE(s.excise_outline(fusiform_outline(V(-15, 0), V(15, 0), 3.0, 30.0), V(0, 0)) == 1);
    settle(s.simulation());
    const auto pieces = s.simulation().map_rest_polyline({V(-30, 0), V(30, 0)}, 0.5);
    REQUIRE(pieces.size() == 2);
    // Each piece stops at the defect's tip (|x| about 15 at rest).
    CHECK(pieces[0].back().x() < -13.0);
    CHECK(pieces[1].front().x() > 13.0);
}

TEST_CASE("markings: a circle crossed by an incision becomes two arcs") {
    Sheet s(kGeometry);
    REQUIRE(s.cut_rest({V(-20, 0), V(20, 0)}) > 0);
    Polyline circle;
    for (int k = 0; k <= 64; ++k) {
        const double a = 2.0 * 3.14159265358979 * k / 64.0 + 0.3;  // the closing point repeats the first
        circle.push_back(V(8.0 * std::cos(a), 8.0 * std::sin(a)));
    }
    const auto pieces = s.simulation().map_rest_polyline(circle, 0.5);
    // Crossed twice: upper arc, lower arc, plus the start of the upper arc
    // again after the second crossing (the polyline starts mid-arc).
    CHECK(pieces.size() == 3);
    CHECK(point_count(pieces) > 100);
}

TEST_CASE("markings: degenerate input gives nothing, and results are cached") {
    Sheet s(kGeometry);
    const Simulation& sim = s.simulation();
    CHECK(sim.map_rest_polyline({}, 0.5).empty());
    CHECK(sim.map_rest_polyline({V(1, 1)}, 0.5).empty());
    CHECK(sim.map_rest_polyline({V(80, 0), V(90, 0)}, 0.5).empty());  // off the sheet
    const Polyline line{V(-5, 0), V(5, 0)};
    const auto& a = sim.rest_polyline_points(line, 0.5);
    const auto& b = sim.rest_polyline_points(line, 0.5);
    CHECK(&a == &b);
}
