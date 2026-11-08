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

// Every edge of the mesh exactly once, sorted.
std::vector<Edge> unique_edges(const TriMesh& mesh);

// Edges used by exactly one triangle: the outline of the sheet (and later, the
// lips of incisions). Interior edges are shared by exactly two triangles.
std::vector<Edge> boundary_edges(const TriMesh& mesh);

}  // namespace skinsim
