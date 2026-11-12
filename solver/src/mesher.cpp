#include "skinsim/mesher.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>

// CDT reports invalid input by throwing. This project is built without C++
// exceptions (like Godot), so CDT_DISABLE_EXCEPTIONS (set in SConstruct) makes
// it call std::terminate() instead, i.e. crash. That is why the input is
// cleaned so carefully (cuts.cpp) and validated again below: CDT must never
// see duplicate points or crossing constraints.
#include <CDT.h>

namespace skinsim {

namespace {

constexpr double kPi = 3.14159265358979323846;

double at_least(double x, double lo) { return (x >= lo) ? x : lo; }

Edge make_edge(int a, int b) { return a < b ? Edge{a, b} : Edge{b, a}; }

int cells_along(double length, double element_size) {
    const double n = std::ceil(length / element_size - 1e-9);
    if (!(n >= 1.0)) return 1;
    if (n > kMaxCellsPerSide) return kMaxCellsPerSide;
    return static_cast<int>(n);
}

struct Triangulated {
    std::vector<Triangle> triangles;
    std::set<Edge> fixed;  // constraint edges as they ended up in the mesh
};

// Run CDT on the points with the given constraint edges, keeping only the
// triangles inside the outer boundary. Returns false if CDT changed the point
// set in an unexpected way (it should not, given clean input).
bool triangulate(std::vector<Eigen::Vector2d>& pts, const std::vector<Edge>& constraints, Triangulated& out) {
    // VertexInsertionOrder::Auto: CDT sorts points spatially (a k-d tree)
    // for fast bulk insertion. TryResolve: if two constraints did cross after
    // all, CDT inserts their intersection point instead of failing.
    CDT::Triangulation<double> cdt(CDT::VertexInsertionOrder::Auto, CDT::IntersectingConstraintEdges::TryResolve,
                                   0.0);
    // CDT accepts any point type through "getter" lambdas, so no copying
    // into its own V2d type is needed.
    cdt.insertVertices(
        pts.begin(), pts.end(), [](const Eigen::Vector2d& p) { return p.x(); },
        [](const Eigen::Vector2d& p) { return p.y(); });
    cdt.insertEdges(
        constraints.begin(), constraints.end(), [](const Edge& e) { return static_cast<CDT::VertInd>(e[0]); },
        [](const Edge& e) { return static_cast<CDT::VertInd>(e[1]); });
    // CDT starts from a huge "super-triangle" around all points. Flood-fill
    // from it and delete everything up to the first constraint: the outline.
    cdt.eraseOuterTriangles();

    // After erasing, vertex i is again input point i. Vertices beyond the
    // input are intersections CDT had to add: keep them (they lie on cuts).
    if (cdt.vertices.size() < pts.size()) return false;
    for (size_t i = 0; i < cdt.vertices.size(); ++i) {
        const Eigen::Vector2d v(cdt.vertices[i].x, cdt.vertices[i].y);
        if (i < pts.size()) {
            if ((v - pts[i]).norm() > 1e-9) return false;
        } else {
            pts.push_back(v);
        }
    }

    out.triangles.clear();
    out.triangles.reserve(cdt.triangles.size());
    for (const CDT::Triangle& t : cdt.triangles) {
        Triangle tri{static_cast<int>(t.vertices[0]), static_cast<int>(t.vertices[1]),
                     static_cast<int>(t.vertices[2])};
        // CDT produces counter-clockwise triangles; enforce it anyway since
        // the FEM code depends on it.
        if (signed_area(pts[tri[0]], pts[tri[1]], pts[tri[2]]) < 0.0) std::swap(tri[1], tri[2]);
        out.triangles.push_back(tri);
    }
    out.fixed.clear();
    for (const CDT::Edge& e : cdt.fixedEdges) out.fixed.insert(make_edge(e.v1(), e.v2()));
    return true;
}

// "Smart" Laplacian smoothing: move each movable point to the average of its
// neighbours (this evens out spacing), but only if that does not make the
// worst triangle around it any worse. Plain Laplacian smoothing can fold
// triangles over near concave corners; the check rules that out.
void smooth(std::vector<Eigen::Vector2d>& pts, const std::vector<char>& movable,
            const std::vector<Triangle>& tris) {
    std::vector<std::vector<int>> around(pts.size());  // vertex -> incident triangles
    for (int t = 0; t < static_cast<int>(tris.size()); ++t)
        for (int v : tris[t]) around[v].push_back(t);

    auto worst_quality = [&](int v) {
        double q = 1.0;
        for (int t : around[v]) q = std::min(q, triangle_quality(pts[tris[t][0]], pts[tris[t][1]], pts[tris[t][2]]));
        return q;
    };

    for (int v = 0; v < static_cast<int>(pts.size()); ++v) {
        if (v >= static_cast<int>(movable.size()) || !movable[v] || around[v].empty()) continue;
        std::set<int> neighbours;
        for (int t : around[v])
            for (int w : tris[t])
                if (w != v) neighbours.insert(w);
        Eigen::Vector2d mean = Eigen::Vector2d::Zero();
        for (int w : neighbours) mean += pts[w];
        mean /= static_cast<double>(neighbours.size());

        const double before = worst_quality(v);
        const Eigen::Vector2d old = pts[v];
        pts[v] = mean;
        if (worst_quality(v) < before) pts[v] = old;
    }
}

// Split the mesh along the cut edges: every vertex on a cut gets one copy per
// "wedge" of triangles around it, where a wedge is a run of triangles
// connected across non-cut edges.
//   - a vertex in the middle of a cut has two wedges (one per lip): 2 copies,
//   - a cut tip has one wedge (you can walk around the tip): it stays single,
//     like the tip of a real incision,
//   - where two cuts cross there are four wedges: 4 copies (four flap corners).
TriMesh split_along_cuts(const std::vector<Eigen::Vector2d>& pts, const std::vector<Triangle>& tris,
                         const std::set<Edge>& cut) {
    const int n = static_cast<int>(pts.size());
    std::vector<char> on_cut(n, 0);
    for (const Edge& e : cut) on_cut[e[0]] = on_cut[e[1]] = 1;

    std::vector<std::vector<int>> around(n);
    for (int t = 0; t < static_cast<int>(tris.size()); ++t)
        for (int v : tris[t]) around[v].push_back(t);

    std::vector<Eigen::Vector2d> out_pts = pts;
    std::vector<Triangle> out_tris = tris;  // corners get renumbered below

    for (int v = 0; v < n; ++v) {
        if (!on_cut[v]) continue;
        const std::vector<int>& fan = around[v];
        // Union-find ("disjoint sets"): each triangle starts in its own group;
        // joining two triangles that share a non-cut edge through v merges groups.
        std::vector<int> parent(fan.size());
        std::iota(parent.begin(), parent.end(), 0);  // 0, 1, 2, ...
        auto find = [&](int i) {
            while (parent[i] != i) i = parent[i] = parent[parent[i]];  // with path halving
            return i;
        };
        for (size_t i = 0; i < fan.size(); ++i) {
            for (size_t j = i + 1; j < fan.size(); ++j) {
                for (int w : tris[fan[i]]) {
                    if (w == v) continue;
                    const Triangle& tj = tris[fan[j]];
                    const bool shared = tj[0] == w || tj[1] == w || tj[2] == w;
                    if (shared && !cut.count(make_edge(v, w))) parent[find(static_cast<int>(i))] = find(static_cast<int>(j));
                }
            }
        }
        // First wedge keeps index v; every other wedge gets a fresh copy.
        std::vector<int> copy_of_root(fan.size(), -1);
        bool first = true;
        for (size_t i = 0; i < fan.size(); ++i) {
            const int root = find(static_cast<int>(i));
            if (copy_of_root[root] < 0) {
                if (first) {
                    copy_of_root[root] = v;
                    first = false;
                } else {
                    copy_of_root[root] = static_cast<int>(out_pts.size());
                    out_pts.push_back(pts[v]);
                }
            }
            for (int k = 0; k < 3; ++k)
                if (tris[fan[i]][k] == v) out_tris[fan[i]][k] = copy_of_root[root];
        }
    }

    TriMesh mesh;
    mesh.rest_positions.resize(2, static_cast<Eigen::Index>(out_pts.size()));
    for (size_t i = 0; i < out_pts.size(); ++i) mesh.rest_positions.col(static_cast<Eigen::Index>(i)) = out_pts[i];
    mesh.triangles = out_tris;

    // Lip edges: the triangle edges that came from cut edges and are now on
    // the boundary (used by one triangle only, since the sides were separated).
    std::set<Edge> boundary;
    for (const Edge& e : boundary_edges(mesh)) boundary.insert(e);
    std::set<Edge> lips;
    for (size_t t = 0; t < tris.size(); ++t)
        for (int k = 0; k < 3; ++k) {
            const Edge before = make_edge(tris[t][k], tris[t][(k + 1) % 3]);
            const Edge after = make_edge(out_tris[t][k], out_tris[t][(k + 1) % 3]);
            if (cut.count(before) && boundary.count(after)) lips.insert(after);
        }
    mesh.cut_edges.assign(lips.begin(), lips.end());
    return mesh;
}

// Drop degenerate triangles and vertices no triangle uses (neither should
// exist, but the simulation must never see them).
void compact(TriMesh& mesh, double min_area) {
    std::vector<Triangle> kept;
    for (const Triangle& t : mesh.triangles)
        if (signed_area(mesh.rest_positions.col(t[0]), mesh.rest_positions.col(t[1]), mesh.rest_positions.col(t[2])) >
            min_area)
            kept.push_back(t);
    std::vector<int> new_index(mesh.vertex_count(), -1);
    int count = 0;
    for (const Triangle& t : kept)
        for (int v : t)
            if (new_index[v] < 0) new_index[v] = count++;
    if (count == mesh.vertex_count() && kept.size() == mesh.triangles.size()) return;

    Eigen::Matrix2Xd pos(2, count);
    for (int v = 0; v < mesh.vertex_count(); ++v)
        if (new_index[v] >= 0) pos.col(new_index[v]) = mesh.rest_positions.col(v);
    for (Triangle& t : kept)
        for (int& v : t) v = new_index[v];
    std::set<Edge> kept_edges;
    for (const Triangle& t : kept)
        for (int k = 0; k < 3; ++k) kept_edges.insert(make_edge(t[k], t[(k + 1) % 3]));
    std::vector<Edge> lips;
    for (const Edge& e : mesh.cut_edges)
        if (new_index[e[0]] >= 0 && new_index[e[1]] >= 0 && kept_edges.count(make_edge(new_index[e[0]], new_index[e[1]])))
            lips.push_back(make_edge(new_index[e[0]], new_index[e[1]]));
    mesh.rest_positions = pos;
    mesh.triangles = kept;
    mesh.cut_edges = lips;
}

}  // namespace

Eigen::AlignedBox2d SheetGeometry::cut_bounds() const {
    const SheetGeometry g = sanitized(*this);
    const double m = g.cut_margin();
    const Eigen::Vector2d half(std::max(0.5 * g.width - m, 0.0), std::max(0.5 * g.height - m, 0.0));
    return Eigen::AlignedBox2d(-half, half);
}

CutOptions SheetGeometry::cut_options() const {
    const SheetGeometry g = sanitized(*this);
    CutOptions o;
    o.spacing = g.cut_spacing();
    o.snap = 0.45 * o.spacing;
    o.endpoint_snap = 1.6 * o.spacing;  // 2 mm at the default element size
    o.bounds = g.cut_bounds();
    return o;
}

SheetGeometry sanitized(SheetGeometry g) {
    g.width = at_least(g.width, 1e-3);
    g.height = at_least(g.height, 1e-3);
    g.element_size = at_least(g.element_size, 1e-3);
    // Same cap as the rectangle mesh: at most kMaxCellsPerSide elements per side.
    g.element_size = std::max(g.element_size, std::max(g.width, g.height) / kMaxCellsPerSide);
    return g;
}

double triangle_quality(const Eigen::Vector2d& a, const Eigen::Vector2d& b, const Eigen::Vector2d& c) {
    const double sum = (b - a).squaredNorm() + (c - b).squaredNorm() + (a - c).squaredNorm();
    if (!(sum > 0.0)) return 0.0;
    return 4.0 * std::sqrt(3.0) * signed_area(a, b, c) / sum;
}

double min_angle_deg(const Eigen::Vector2d& a, const Eigen::Vector2d& b, const Eigen::Vector2d& c) {
    auto angle = [](const Eigen::Vector2d& p, const Eigen::Vector2d& q, const Eigen::Vector2d& r) {
        const Eigen::Vector2d u = q - p, v = r - p;
        return std::atan2(std::abs(u.x() * v.y() - u.y() * v.x()), u.dot(v));
    };
    return std::min({angle(a, b, c), angle(b, c, a), angle(c, a, b)}) * 180.0 / kPi;
}

namespace {

// Connected pieces of the mesh: triangles sharing an edge belong together.
// Union-find ("disjoint set"): each triangle points towards a representative
// of its piece; joining two pieces links their representatives.
std::vector<int> piece_of_triangles(const TriMesh& mesh) {
    const int nt = mesh.triangle_count();
    std::vector<int> parent(nt);
    std::iota(parent.begin(), parent.end(), 0);
    auto find = [&](int t) {
        while (parent[t] != t) t = parent[t] = parent[parent[t]];  // path halving
        return t;
    };
    std::map<Edge, int> first;  // edge -> a triangle using it
    for (int t = 0; t < nt; ++t)
        for (int k = 0; k < 3; ++k) {
            const Edge e = make_edge(mesh.triangles[t][k], mesh.triangles[t][(k + 1) % 3]);
            auto [it, inserted] = first.emplace(e, t);
            if (!inserted) parent[find(t)] = find(it->second);
        }
    for (int t = 0; t < nt; ++t) parent[t] = find(t);
    return parent;
}

// For each piece (by representative): does it touch the outer border?
std::set<int> pieces_on_border(const TriMesh& mesh, const std::vector<int>& piece) {
    const std::set<Edge> lips(mesh.cut_edges.begin(), mesh.cut_edges.end());
    std::map<Edge, int> owner;
    for (int t = 0; t < mesh.triangle_count(); ++t)
        for (int k = 0; k < 3; ++k) owner[make_edge(mesh.triangles[t][k], mesh.triangles[t][(k + 1) % 3])] = t;
    std::set<int> out;
    for (const Edge& e : boundary_edges(mesh))
        if (!lips.count(e)) out.insert(piece[owner[e]]);
    return out;
}

}  // namespace

int island_at(const TriMesh& mesh, const Eigen::Vector2d& p) {
    const TriangleLocator locator(mesh.rest_positions, mesh.triangles);
    const int t = locator.locate(p, nullptr, 1e-9);
    if (t < 0) return -1;
    const std::vector<int> piece = piece_of_triangles(mesh);
    return pieces_on_border(mesh, piece).count(piece[t]) ? 0 : 1;
}

int remove_islands(TriMesh& mesh, const std::vector<Eigen::Vector2d>& points) {
    if (points.empty() || mesh.triangles.empty()) return 0;
    const TriangleLocator locator(mesh.rest_positions, mesh.triangles);
    const std::vector<int> piece = piece_of_triangles(mesh);
    const std::set<int> attached = pieces_on_border(mesh, piece);
    std::set<int> remove;
    for (const Eigen::Vector2d& p : points) {
        const int t = locator.locate(p, nullptr, 1e-9);
        if (t >= 0 && !attached.count(piece[t])) remove.insert(piece[t]);
    }
    if (remove.empty()) return 0;
    std::vector<Triangle> kept;
    for (int t = 0; t < mesh.triangle_count(); ++t)
        if (!remove.count(piece[t])) kept.push_back(mesh.triangles[t]);
    mesh.triangles = std::move(kept);
    compact(mesh, -1.0);  // drop the island's now unused vertices and lip edges
    return static_cast<int>(remove.size());
}

TriMesh mesh_sheet(const SheetGeometry& geometry, const CutGraph& cuts_in, const std::vector<Eigen::Vector2d>& excised) {
    const SheetGeometry g = sanitized(geometry);
    const double W = g.width, H = g.height, h = g.element_size;
    const int nx = cells_along(W, h), ny = cells_along(H, h);
    const double dx = W / nx, dy = H / ny;

    // --- 1. Outline, counter-clockwise from the bottom-left corner. ---------
    std::vector<Eigen::Vector2d> pts;
    for (int i = 0; i < nx; ++i) pts.emplace_back(-0.5 * W + i * dx, -0.5 * H);
    for (int j = 0; j < ny; ++j) pts.emplace_back(0.5 * W, -0.5 * H + j * dy);
    for (int i = nx; i > 0; --i) pts.emplace_back(-0.5 * W + i * dx, 0.5 * H);
    for (int j = ny; j > 0; --j) pts.emplace_back(-0.5 * W, -0.5 * H + j * dy);
    const int n_outline = static_cast<int>(pts.size());
    std::vector<Edge> constraints;
    for (int k = 0; k < n_outline; ++k) constraints.push_back(make_edge(k, (k + 1) % n_outline));

    // --- 2. Incisions. -------------------------------------------------------
    // Re-check the PSLG rules: bad input here would crash inside CDT.
    Eigen::AlignedBox2d bounds = g.cut_bounds();
    bounds.extend(bounds.min() - Eigen::Vector2d::Constant(1e-6 * h));
    bounds.extend(bounds.max() + Eigen::Vector2d::Constant(1e-6 * h));
    const bool cuts_ok = is_valid(cuts_in, bounds, 0.01 * g.cut_spacing());
    const CutGraph empty;
    const CutGraph& cuts = cuts_ok ? cuts_in : empty;

    const int cut_base = static_cast<int>(pts.size());
    for (const Eigen::Vector2d& p : cuts.points) pts.push_back(p);
    std::vector<std::array<Eigen::Vector2d, 2>> cut_segments;  // geometry, for seed clearance
    for (const Edge& e : cuts.segments) {
        // Long segments (e.g. from an API caller rather than add_cut) are
        // subdivided so the lips are resolved at the cut spacing.
        const Eigen::Vector2d a = cuts.points[e[0]], b = cuts.points[e[1]];
        const int pieces = std::max(1, static_cast<int>(std::ceil((b - a).norm() / g.cut_spacing() - 0.25)));
        int prev = cut_base + e[0];
        for (int k = 1; k <= pieces; ++k) {
            int cur = cut_base + e[1];
            if (k < pieces) {
                cur = static_cast<int>(pts.size());
                pts.push_back(a + (b - a) * (static_cast<double>(k) / pieces));
            }
            constraints.push_back(make_edge(prev, cur));
            prev = cur;
        }
        cut_segments.push_back({a, b});
    }
    const int n_fixed = static_cast<int>(pts.size());  // outline + cut points never move

    // --- 3. Hexagonal lattice of interior seeds. ------------------------------
    // Rows dx*sqrt(3)/2 apart, every other row shifted by half a step: the
    // Delaunay triangulation of this is (nearly) equilateral triangles.
    const int rows = std::max(1, static_cast<int>(std::lround(H / (dx * std::sqrt(3.0) / 2.0))));
    const double row_dy = H / rows;
    const double clearance = 0.6 * h;  // seeds this close to a cut are dropped
    for (int r = 1; r < rows; ++r) {
        const double y = -0.5 * H + r * row_dy;
        const double shift = (r % 2) ? 0.5 : 0.0;
        for (int i = 0; i <= nx; ++i) {
            const double x = -0.5 * W + (i + shift) * dx;
            if (x < -0.5 * W + 0.25 * dx || x > 0.5 * W - 0.25 * dx) continue;
            const Eigen::Vector2d p(x, y);
            bool clear = true;
            for (const auto& s : cut_segments)
                if (point_segment_distance(p, s[0], s[1]) < clearance) {
                    clear = false;
                    break;
                }
            if (clear) pts.push_back(p);
        }
    }
    std::vector<char> movable(pts.size(), 0);
    std::fill(movable.begin() + n_fixed, movable.end(), 1);

    // --- 4. Triangulate, smoothing in between. ------------------------------
    // Smoothing moves points, after which a different connectivity may be
    // more Delaunay, so re-triangulate after every pass (CDT is fast: ~1 ms).
    Triangulated tri;
    for (int pass = 0;; ++pass) {
        if (!triangulate(pts, constraints, tri)) {
            // Should be impossible with validated input. Fall back to a sheet
            // without incisions rather than handing the solver garbage.
            return cuts.empty() ? make_rectangle_mesh(W, H, h) : mesh_sheet(g, CutGraph{});
        }
        if (pass == kSmoothingPasses) break;
        smooth(pts, movable, tri.triangles);
    }

    // --- 5. Separate the lips. ----------------------------------------------
    // Cut edges = constraint edges that are not on the outline. (Outline
    // edges border one triangle, cut edges two.)
    std::set<Edge> outline;
    {
        TriMesh tmp;
        tmp.rest_positions.resize(2, static_cast<Eigen::Index>(pts.size()));
        for (size_t i = 0; i < pts.size(); ++i) tmp.rest_positions.col(static_cast<Eigen::Index>(i)) = pts[i];
        tmp.triangles = tri.triangles;
        for (const Edge& e : boundary_edges(tmp)) outline.insert(e);
    }
    std::set<Edge> cut;
    for (const Edge& e : tri.fixed)
        if (!outline.count(e)) cut.insert(e);

    TriMesh mesh = split_along_cuts(pts, tri.triangles, cut);
    compact(mesh, 1e-10 * h * h);
    remove_islands(mesh, excised);
    return mesh;
}

}  // namespace skinsim
