#include "skinsim/mesh.hpp"

#include <algorithm>
#include <cmath>

namespace skinsim {

namespace {  // anonymous namespace: these helpers are private to this .cpp file

constexpr double kMinLength = 1e-3;  // mm; smallest accepted width/height/element size

// Clamp to [lo, inf). Written as !(x >= lo) so NaN (for which every comparison
// is false) also gets replaced, which std::max would not guarantee.
double at_least(double x, double lo) { return (x >= lo) ? x : lo; }

int cells_along(double length, double element_size) {
    // ceil: elements are never *larger* than requested. The small epsilon stops
    // 100 / 5 = 20.000000001 from becoming 21 because of floating-point rounding.
    const double n = std::ceil(length / element_size - 1e-9);
    if (!(n >= 1.0)) return 1;
    if (n > kMaxCellsPerSide) return kMaxCellsPerSide;
    return static_cast<int>(n);
}

// Every edge of every triangle (shared edges appear twice), sorted so that
// equal edges sit next to each other.
std::vector<Edge> all_edges_sorted(const TriMesh& mesh) {
    std::vector<Edge> edges;
    edges.reserve(mesh.triangles.size() * 3);
    for (const Triangle& t : mesh.triangles) {
        for (int k = 0; k < 3; ++k) {
            const int a = t[k];
            const int b = t[(k + 1) % 3];
            edges.push_back(a < b ? Edge{a, b} : Edge{b, a});
        }
    }
    std::sort(edges.begin(), edges.end());
    return edges;
}

}  // namespace

TriMesh make_rectangle_mesh(double width, double height, double element_size) {
    width = at_least(width, kMinLength);
    height = at_least(height, kMinLength);
    element_size = at_least(element_size, kMinLength);

    const int nx = cells_along(width, element_size);
    const int ny = cells_along(height, element_size);

    TriMesh mesh;

    // Vertices on an (nx+1) x (ny+1) grid, numbered row by row from bottom-left.
    mesh.rest_positions.resize(2, (nx + 1) * (ny + 1));
    // A lambda is a small inline function; [nx] "captures" nx so the body can use it.
    auto vertex_id = [nx](int i, int j) { return j * (nx + 1) + i; };
    for (int j = 0; j <= ny; ++j) {
        for (int i = 0; i <= nx; ++i) {
            const double x = -0.5 * width + width * i / nx;
            const double y = -0.5 * height + height * j / ny;
            mesh.rest_positions.col(vertex_id(i, j)) << x, y;
        }
    }

    // Two triangles per cell, both CCW. Cell corners:
    //   v01 --- v11
    //    |       |
    //   v00 --- v10
    mesh.triangles.reserve(2 * nx * ny);
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            const int v00 = vertex_id(i, j), v10 = vertex_id(i + 1, j);
            const int v01 = vertex_id(i, j + 1), v11 = vertex_id(i + 1, j + 1);
            if ((i + j) % 2 == 0) {  // diagonal v00-v11  ( / )
                mesh.triangles.push_back({v00, v10, v11});
                mesh.triangles.push_back({v00, v11, v01});
            } else {                 // diagonal v10-v01  ( \ )
                mesh.triangles.push_back({v00, v10, v01});
                mesh.triangles.push_back({v10, v11, v01});
            }
        }
    }
    return mesh;
}

double signed_area(const Eigen::Vector2d& a, const Eigen::Vector2d& b, const Eigen::Vector2d& c) {
    const Eigen::Vector2d ab = b - a;
    const Eigen::Vector2d ac = c - a;
    return 0.5 * (ab.x() * ac.y() - ab.y() * ac.x());
}

Eigen::Vector3d barycentric(const Eigen::Vector2d& a, const Eigen::Vector2d& b, const Eigen::Vector2d& c,
                            const Eigen::Vector2d& p) {
    // The weight of a vertex is the area of the sub-triangle opposite it,
    // divided by the whole: p at vertex a gives (1, 0, 0).
    const double total = signed_area(a, b, c);
    return Eigen::Vector3d(signed_area(p, b, c), signed_area(a, p, c), signed_area(a, b, p)) / total;
}

TriangleLocator::TriangleLocator(const Eigen::Matrix2Xd& positions, const std::vector<Triangle>& triangles)
    : positions_(positions), triangles_(triangles) {
    if (positions_.cols() == 0 || triangles_.empty()) return;
    const Eigen::Vector2d lo = positions_.rowwise().minCoeff();
    const Eigen::Vector2d hi = positions_.rowwise().maxCoeff();
    const Eigen::Vector2d size = (hi - lo).cwiseMax(1e-9);
    // About two triangles per bucket on average: cell area ~ 2x triangle area.
    cell_ = std::sqrt(2.0 * size.x() * size.y() / static_cast<double>(triangles_.size()));
    if (!(cell_ > 0.0) || !std::isfinite(cell_)) cell_ = std::max(size.x(), size.y());
    // Cap the grid so a degenerate shape can't allocate millions of buckets.
    cell_ = std::max(cell_, std::max(size.x(), size.y()) / 1000.0);
    origin_ = lo;
    nx_ = static_cast<int>(size.x() / cell_) + 1;
    ny_ = static_cast<int>(size.y() / cell_) + 1;
    buckets_.assign(static_cast<size_t>(nx_) * ny_, {});

    for (int t = 0; t < static_cast<int>(triangles_.size()); ++t) {
        // Put the triangle in every bucket its bounding box touches.
        Eigen::Vector2d tlo = positions_.col(triangles_[t][0]), thi = tlo;
        for (int k = 1; k < 3; ++k) {
            tlo = tlo.cwiseMin(positions_.col(triangles_[t][k]));
            thi = thi.cwiseMax(positions_.col(triangles_[t][k]));
        }
        const int i0 = std::clamp(static_cast<int>((tlo.x() - origin_.x()) / cell_), 0, nx_ - 1);
        const int i1 = std::clamp(static_cast<int>((thi.x() - origin_.x()) / cell_), 0, nx_ - 1);
        const int j0 = std::clamp(static_cast<int>((tlo.y() - origin_.y()) / cell_), 0, ny_ - 1);
        const int j1 = std::clamp(static_cast<int>((thi.y() - origin_.y()) / cell_), 0, ny_ - 1);
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) buckets_[static_cast<size_t>(j) * nx_ + i].push_back(t);
    }
}

int TriangleLocator::locate(const Eigen::Vector2d& p, Eigen::Vector3d* bary, double tolerance) const {
    if (buckets_.empty() || !p.allFinite()) return -1;
    const double fi = (p.x() - origin_.x()) / cell_;
    const double fj = (p.y() - origin_.y()) / cell_;
    // A point just outside the grid can still be within `tolerance` of a
    // triangle on its edge, so clamp rather than reject (the test below decides).
    if (fi < -1.0 || fj < -1.0 || fi > nx_ + 1.0 || fj > ny_ + 1.0) return -1;
    const int i = std::clamp(static_cast<int>(fi), 0, nx_ - 1);
    const int j = std::clamp(static_cast<int>(fj), 0, ny_ - 1);

    int best = -1;
    double best_inside = -tolerance;  // how far inside: the smallest barycentric weight
    Eigen::Vector3d best_bary;
    for (int t : buckets_[static_cast<size_t>(j) * nx_ + i]) {
        const Eigen::Vector2d a = positions_.col(triangles_[t][0]);
        const Eigen::Vector2d b = positions_.col(triangles_[t][1]);
        const Eigen::Vector2d c = positions_.col(triangles_[t][2]);
        if (!(signed_area(a, b, c) > 0.0)) continue;  // inverted/degenerate: no inside
        const Eigen::Vector3d w = barycentric(a, b, c, p);
        if (w.minCoeff() >= best_inside) {
            best_inside = w.minCoeff();
            best = t;
            best_bary = w;
        }
    }
    if (best >= 0 && bary != nullptr) *bary = best_bary;
    return best;
}

std::vector<Edge> unique_edges(const TriMesh& mesh) {
    std::vector<Edge> edges = all_edges_sorted(mesh);
    // std::unique moves duplicates to the end and returns where they start; erase drops them.
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    return edges;
}

std::vector<Edge> boundary_edges(const TriMesh& mesh) {
    const std::vector<Edge> edges = all_edges_sorted(mesh);
    std::vector<Edge> boundary;
    // Walk runs of equal edges; a run of length 1 is an edge only one triangle uses.
    for (size_t i = 0; i < edges.size();) {
        size_t j = i + 1;
        while (j < edges.size() && edges[j] == edges[i]) ++j;
        if (j - i == 1) boundary.push_back(edges[i]);
        i = j;
    }
    return boundary;
}

}  // namespace skinsim
