// Triangle mesh of the skin sheet, plus generators and topology helpers.
//
// Conventions used throughout the solver:
//   - 2D, units of millimetres. x to the right, y "up" (towards the top of the screen).
//   - Triangles are listed counter-clockwise (CCW), so their signed area is positive.
//     The FEM code relies on this: a triangle whose signed area turns negative
//     during a simulation has been turned inside out, which we must detect.
#pragma once

#include <array>
#include <vector>

#include <Eigen/Core>

namespace skinsim {

// Three vertex indices (columns of TriMesh::rest_positions), counter-clockwise.
using Triangle = std::array<int, 3>;

// Two vertex indices, always stored with [0] < [1] so each edge has one spelling.
using Edge = std::array<int, 2>;

struct TriMesh {
    // 2 x N matrix; column i holds (x, y) of vertex i in the undeformed ("rest",
    // or "reference") configuration. Eigen stores matrices column by column, so
    // the memory reads x0, y0, x1, y1, ...: exactly the layout of the 2N-long
    // unknowns vector the FEM solver will work with, without copying.
    Eigen::Matrix2Xd rest_positions;

    std::vector<Triangle> triangles;

    // Incision lips: edges created by cutting. Each side of an incision has
    // its own copy of the vertices along it (same rest position, separate
    // nodes), so the two lips can move apart. These edges are part of the
    // mesh boundary but, unlike the outer border, are never pinned.
    std::vector<Edge> cut_edges;

    int vertex_count() const { return static_cast<int>(rest_positions.cols()); }
    int triangle_count() const { return static_cast<int>(triangles.size()); }
};

// Upper limit on cells along one side. Guards against a slider (or a typo)
// requesting millions of elements: the generator clamps instead of failing.
inline constexpr int kMaxCellsPerSide = 200;

// Rectangle of width x height (mm) centred on the origin, divided into roughly
// square cells of side <= element_size, each cell split into two triangles.
//
// The diagonal direction alternates in a checkerboard pattern. If every cell
// were split along the same diagonal, the mesh would be stiffer when sheared one
// way than the other (a purely numerical anisotropy). That would be a bad
// artefact in a simulator whose later goal is to model *real* anisotropy (RSTL fibres).
//
// Invalid inputs (zero, negative, NaN) are clamped to sane values; this never fails.
TriMesh make_rectangle_mesh(double width, double height, double element_size);

// Twice the signed area is the 2D cross product (b - a) x (c - a). Positive means a, b, c are CCW.
double signed_area(const Eigen::Vector2d& a, const Eigen::Vector2d& b, const Eigen::Vector2d& c);

// Barycentric coordinates of p in triangle (a, b, c): the weights (w0, w1, w2),
// summing to 1, with p = w0 a + w1 b + w2 c. All >= 0 means p is inside.
Eigen::Vector3d barycentric(const Eigen::Vector2d& a, const Eigen::Vector2d& b, const Eigen::Vector2d& c,
                            const Eigen::Vector2d& p);

// Answers "which triangle contains this point?" quickly. Testing every
// triangle would be fine once, but cutting asks for thousands of points.
// The triangles are sorted into a uniform grid of square "buckets"; a query
// only tests the few triangles in its own bucket.
class TriangleLocator {
public:
    // positions: 2 x N vertex positions (rest or current shape). Copied, so
    // the locator stays valid if the caller's positions change later.
    TriangleLocator(const Eigen::Matrix2Xd& positions, const std::vector<Triangle>& triangles);

    // Index of the triangle containing p, or -1. If several qualify (p on a
    // shared edge), the one p is most "inside" wins. `tolerance` accepts points
    // slightly outside a triangle (in barycentric units: 1e-3 = 0.1% of its size).
    // Inverted or degenerate triangles are never returned.
    int locate(const Eigen::Vector2d& p, Eigen::Vector3d* bary = nullptr, double tolerance = 1e-9) const;

private:
    Eigen::Matrix2Xd positions_;
    std::vector<Triangle> triangles_;
    Eigen::Vector2d origin_;
    double cell_ = 1.0;
    int nx_ = 1, ny_ = 1;
    std::vector<std::vector<int>> buckets_;  // triangle indices per grid cell
};

// Every edge of the mesh exactly once, sorted.
std::vector<Edge> unique_edges(const TriMesh& mesh);

// Edges used by exactly one triangle: the outline of the sheet and the lips of
// incisions. Interior edges are shared by exactly two triangles.
std::vector<Edge> boundary_edges(const TriMesh& mesh);

}  // namespace skinsim
