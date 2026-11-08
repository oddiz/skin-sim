// Tests for the rectangle mesh generator and topology helpers.
// These check geometric and topological invariants that later physics relies on.

#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "skinsim/mesh.hpp"

using namespace skinsim;

namespace {
Eigen::Vector2d pos(const TriMesh& m, int v) { return m.rest_positions.col(v); }
}  // namespace

TEST_CASE("rectangle mesh has the expected counts") {
    // 100 x 60 mm at 5 mm -> 20 x 12 cells.
    const TriMesh m = make_rectangle_mesh(100.0, 60.0, 5.0);
    CHECK(m.vertex_count() == 21 * 13);
    CHECK(m.triangle_count() == 2 * 20 * 12);
}

TEST_CASE("element size is an upper bound (cells round up)") {
    // 100 / 7 = 14.3 -> 15 cells of 6.67 mm.
    const TriMesh m = make_rectangle_mesh(100.0, 7.0, 7.0);
    CHECK(m.vertex_count() == 16 * 2);
}

TEST_CASE("all triangles are counter-clockwise and tile the rectangle exactly") {
    const double w = 80.0, h = 50.0;
    const TriMesh m = make_rectangle_mesh(w, h, 3.0);
    double total = 0.0;
    for (const Triangle& t : m.triangles) {
        const double a = signed_area(pos(m, t[0]), pos(m, t[1]), pos(m, t[2]));
        CHECK(a > 0.0);
        total += a;
    }
    CHECK(total == doctest::Approx(w * h).epsilon(1e-12));
}

TEST_CASE("mesh is centred on the origin") {
    const TriMesh m = make_rectangle_mesh(100.0, 60.0, 5.0);
    const Eigen::Vector2d lo = m.rest_positions.rowwise().minCoeff();
    const Eigen::Vector2d hi = m.rest_positions.rowwise().maxCoeff();
    CHECK(lo.x() == doctest::Approx(-50.0));
    CHECK(hi.x() == doctest::Approx(50.0));
    CHECK(lo.y() == doctest::Approx(-30.0));
    CHECK(hi.y() == doctest::Approx(30.0));
}

TEST_CASE("edge topology: Euler characteristic and boundary") {
    const TriMesh m = make_rectangle_mesh(100.0, 60.0, 5.0);
    const int nx = 20, ny = 12;
    const auto edges = unique_edges(m);
    const auto boundary = boundary_edges(m);

    // Euler's formula for a mesh that is topologically a disk: V - E + F = 1.
    // If it fails, triangles are duplicated, missing, or not sharing edges properly.
    CHECK(m.vertex_count() - static_cast<int>(edges.size()) + m.triangle_count() == 1);

    // The outline consists of nx edges along top and bottom, ny along each side.
    CHECK(boundary.size() == static_cast<size_t>(2 * (nx + ny)));

    // Every boundary vertex lies on the rectangle's border.
    for (const Edge& e : boundary) {
        for (int v : e) {
            const Eigen::Vector2d p = pos(m, v);
            const bool on_border = std::abs(std::abs(p.x()) - 50.0) < 1e-9 ||
                                   std::abs(std::abs(p.y()) - 30.0) < 1e-9;
            CHECK(on_border);
        }
    }
}

TEST_CASE("diagonals alternate (no directional mesh bias)") {
    // Count cells split along "/" vs "\": a checkerboard gives an equal split.
    const TriMesh m = make_rectangle_mesh(40.0, 40.0, 10.0);  // 4 x 4 cells
    int slash = 0, backslash = 0;
    for (const Edge& e : unique_edges(m)) {
        const Eigen::Vector2d d = pos(m, e[1]) - pos(m, e[0]);
        if (std::abs(d.x()) > 1e-9 && std::abs(d.y()) > 1e-9) {
            (d.x() * d.y() > 0 ? slash : backslash)++;
        }
    }
    CHECK(slash == 8);
    CHECK(backslash == 8);
}

TEST_CASE("invalid inputs are clamped, never crash") {
    const double nan = std::numeric_limits<double>::quiet_NaN();

    const TriMesh tiny = make_rectangle_mesh(0.0, -5.0, nan);
    CHECK(tiny.vertex_count() >= 4);
    CHECK(tiny.triangle_count() >= 2);

    // Asking for absurdly small elements hits the per-side cap.
    const TriMesh capped = make_rectangle_mesh(1000.0, 1000.0, 1e-6);
    CHECK(capped.vertex_count() == (kMaxCellsPerSide + 1) * (kMaxCellsPerSide + 1));
}
