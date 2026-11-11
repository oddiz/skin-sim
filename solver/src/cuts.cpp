#include "skinsim/cuts.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace skinsim {

namespace {

double cross(const Eigen::Vector2d& a, const Eigen::Vector2d& b) { return a.x() * b.y() - a.y() * b.x(); }

Edge make_edge(int a, int b) { return a < b ? Edge{a, b} : Edge{b, a}; }

// Clip a polyline to an axis-aligned box. Segment by segment with the
// Liang-Barsky method: write the segment as p + t (q - p), t in [0, 1], and
// shrink [t0, t1] against each of the four box sides. A stroke that leaves
// the box and comes back yields several separate pieces.
std::vector<Polyline> clip_to_box(const Polyline& path, const Eigen::AlignedBox2d& box) {
    std::vector<Polyline> pieces;
    Polyline current;
    auto flush = [&] {
        if (current.size() >= 2) pieces.push_back(current);
        current.clear();
    };
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        const Eigen::Vector2d p = path[i], d = path[i + 1] - path[i];
        double t0 = 0.0, t1 = 1.0;
        bool inside = true;
        for (int axis = 0; axis < 2 && inside; ++axis) {
            const double lo = box.min()[axis], hi = box.max()[axis];
            if (d[axis] == 0.0) {
                inside = p[axis] >= lo && p[axis] <= hi;
                continue;
            }
            double ta = (lo - p[axis]) / d[axis], tb = (hi - p[axis]) / d[axis];
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
            inside = t0 <= t1;
        }
        if (!inside) {
            flush();
            continue;
        }
        const Eigen::Vector2d a = p + t0 * d, b = p + t1 * d;
        if (!current.empty() && (current.back() - a).norm() > 1e-12) flush();  // re-entered: new piece
        if (current.empty()) current.push_back(a);
        current.push_back(b);
        if (t1 < 1.0) flush();  // left the box
    }
    flush();
    return pieces;
}

// Simplify, then subdivide every remaining straight run into equal steps of
// at most `spacing`. Corners survive because simplification keeps them.
Polyline resample(const Polyline& piece, double spacing) {
    const Polyline key = simplify(piece, 0.15 * spacing);
    Polyline out;
    out.push_back(key.front());
    for (size_t i = 0; i + 1 < key.size(); ++i) {
        const double len = (key[i + 1] - key[i]).norm();
        const int n = std::max(1, static_cast<int>(std::ceil(len / spacing - 1e-9)));
        for (int k = 1; k <= n; ++k) out.push_back(key[i] + (key[i + 1] - key[i]) * (static_cast<double>(k) / n));
    }
    // At least two segments: the lips separate only at vertices *between* the
    // two tips (tips stay single), so a one-segment cut could never open.
    if (out.size() == 2) out.insert(out.begin() + 1, 0.5 * (out[0] + out[1]));
    return out;
}

double length(const Polyline& p) {
    double L = 0.0;
    for (size_t i = 0; i + 1 < p.size(); ++i) L += (p[i + 1] - p[i]).norm();
    return L;
}

// Adds one stroke to a graph, keeping it a valid PSLG. "Old" geometry is what
// existed before this stroke; endpoint snapping only looks at old geometry,
// otherwise the end of a stroke would snap back onto its own previous point.
class StrokeInserter {
public:
    StrokeInserter(CutGraph& g, const CutOptions& o) : g_(g), o_(o) {
        old_point_.assign(g_.points.size(), 1);
        old_segment_.assign(g_.segments.size(), 1);
        for (const Edge& e : g_.segments) existing_.insert(e);
    }

    int added() const { return added_; }

    void insert(const Polyline& stroke) {
        const int first = insert_point(stroke.front(), o_.endpoint_snap, /*old_only=*/true);
        int prev = first;
        for (size_t i = 1; i < stroke.size(); ++i) {
            const bool last = i + 1 == stroke.size();
            int cur;
            if (last && (stroke.back() - g_.points[first]).norm() < o_.endpoint_snap &&
                length(stroke) > 3.0 * o_.endpoint_snap) {
                cur = first;  // the stroke ends where it began: close the loop
            } else {
                cur = insert_point(stroke[i], last ? o_.endpoint_snap : o_.snap, /*old_only=*/last);
            }
            add_segment(prev, cur);
            prev = cur;
        }
    }

private:
    // Returns the index of an existing point within r of p, else splits an
    // existing segment within r (at the closest point), else adds a new point.
    int insert_point(const Eigen::Vector2d& p, double r, bool old_only) {
        int best = -1;
        double best_d = r;
        for (int k = 0; k < static_cast<int>(g_.points.size()); ++k) {
            if (old_only && !old_point_[k]) continue;
            const double d = (g_.points[k] - p).norm();
            if (d < best_d) best_d = d, best = k;
        }
        if (best >= 0) return best;

        int seg = -1;
        double seg_t = 0.0;
        best_d = r;
        for (int s = 0; s < static_cast<int>(g_.segments.size()); ++s) {
            if (old_only && !old_segment_[s]) continue;
            double t;
            const double d = point_segment_distance(p, g_.points[g_.segments[s][0]], g_.points[g_.segments[s][1]], &t);
            if (d < best_d) best_d = d, seg = s, seg_t = t;
        }
        if (seg >= 0) {
            const Eigen::Vector2d a = g_.points[g_.segments[seg][0]], b = g_.points[g_.segments[seg][1]];
            return split_segment(seg, a + seg_t * (b - a));
        }
        return new_point(p, false);
    }

    int new_point(const Eigen::Vector2d& p, bool old) {
        g_.points.push_back(p);
        old_point_.push_back(old ? 1 : 0);
        return static_cast<int>(g_.points.size()) - 1;
    }

    // Replace segment a-b by a-k and k-b, with k a new point at p (on a-b).
    int split_segment(int s, const Eigen::Vector2d& p) {
        const Edge e = g_.segments[s];
        const bool old = old_segment_[s];
        const int k = new_point(p, old);  // a point on old geometry counts as old
        existing_.erase(e);
        g_.segments[s] = make_edge(e[0], k);
        g_.segments.push_back(make_edge(k, e[1]));
        old_segment_.push_back(old);
        existing_.insert(g_.segments[s]);
        existing_.insert(g_.segments.back());
        return k;
    }

    // Add the straight segment a-b, routing it through every existing point
    // that lies (almost) on it and through every crossing with another segment.
    void add_segment(int a, int b) {
        if (a == b) return;
        const Eigen::Vector2d pa = g_.points[a], pb = g_.points[b];
        std::vector<std::pair<double, int>> via;  // (position along a-b, point index)

        for (int k = 0; k < static_cast<int>(g_.points.size()); ++k) {
            if (k == a || k == b) continue;
            double t;
            if (point_segment_distance(g_.points[k], pa, pb, &t) < o_.snap && t > 1e-9 && t < 1.0 - 1e-9)
                via.emplace_back(t, k);
        }

        struct Crossing { int segment; double t; Eigen::Vector2d p; };
        std::vector<Crossing> crossings;
        const Eigen::Vector2d r = pb - pa;
        for (int s = 0; s < static_cast<int>(g_.segments.size()); ++s) {
            const int c = g_.segments[s][0], d = g_.segments[s][1];
            if (c == a || c == b || d == a || d == b) continue;
            const Eigen::Vector2d q = g_.points[c], sv = g_.points[d] - q;
            const double denom = cross(r, sv);
            if (std::abs(denom) <= 1e-14 * r.norm() * sv.norm()) continue;  // parallel
            const double t = cross(q - pa, sv) / denom;  // along a-b
            const double u = cross(q - pa, r) / denom;   // along c-d
            if (!(t > 1e-9 && t < 1.0 - 1e-9 && u > 1e-9 && u < 1.0 - 1e-9)) continue;
            const Eigen::Vector2d x = pa + t * r;
            // A crossing right next to an endpoint of c-d: route through that
            // endpoint instead (it's already in `via` as a near point).
            if ((x - q).norm() < o_.snap || (x - g_.points[d]).norm() < o_.snap) continue;
            crossings.push_back({s, t, x});
        }
        for (const Crossing& c : crossings) via.emplace_back(c.t, split_segment(c.segment, c.p));

        std::sort(via.begin(), via.end());
        int prev = a;
        for (const auto& [t, k] : via) {
            link(prev, k);
            prev = k;
        }
        link(prev, b);
    }

    void link(int i, int j) {
        if (i == j) return;
        const Edge e = make_edge(i, j);
        if (!existing_.insert(e).second) return;  // already there (retraced cut)
        g_.segments.push_back(e);
        old_segment_.push_back(0);
        ++added_;
    }

    CutGraph& g_;
    const CutOptions& o_;
    std::vector<char> old_point_, old_segment_;
    std::set<Edge> existing_;
    int added_ = 0;
};

}  // namespace

double point_segment_distance(const Eigen::Vector2d& p, const Eigen::Vector2d& a, const Eigen::Vector2d& b,
                              double* t_out) {
    const Eigen::Vector2d ab = b - a;
    const double len2 = ab.squaredNorm();
    double t = len2 > 0.0 ? (p - a).dot(ab) / len2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    if (t_out != nullptr) *t_out = t;
    return (a + t * ab - p).norm();
}

Polyline simplify(const Polyline& path, double tolerance) {
    if (path.size() <= 2) return path;
    std::vector<char> keep(path.size(), 0);
    keep.front() = keep.back() = 1;
    // Douglas-Peucker: keep the point farthest from the chord if it is farther
    // than the tolerance, then repeat on both halves. An explicit stack
    // instead of recursion, so a very long stroke can't overflow the call stack.
    std::vector<std::pair<size_t, size_t>> stack{{0, path.size() - 1}};
    while (!stack.empty()) {
        const auto [i, j] = stack.back();
        stack.pop_back();
        double worst = -1.0;
        size_t worst_k = i;
        for (size_t k = i + 1; k < j; ++k) {
            const double d = point_segment_distance(path[k], path[i], path[j]);
            if (d > worst) worst = d, worst_k = k;
        }
        if (worst > tolerance) {
            keep[worst_k] = 1;
            stack.emplace_back(i, worst_k);
            stack.emplace_back(worst_k, j);
        }
    }
    Polyline out;
    for (size_t k = 0; k < path.size(); ++k)
        if (keep[k]) out.push_back(path[k]);
    return out;
}

int add_cut(CutGraph& graph, const Polyline& path, const CutOptions& options) {
    CutOptions o = options;
    if (!(o.spacing > 0.0) || !std::isfinite(o.spacing)) return 0;
    o.snap = std::clamp(o.snap, 0.05 * o.spacing, 0.9 * o.spacing);
    o.endpoint_snap = std::max(o.endpoint_snap, o.snap);

    // Non-finite points (NaN from a bad touch conversion) split the stroke.
    std::vector<Polyline> raw(1);
    for (const Eigen::Vector2d& p : path) {
        if (p.allFinite()) {
            if (raw.back().empty() || (p - raw.back().back()).norm() > 1e-9) raw.back().push_back(p);
        } else if (!raw.back().empty()) {
            raw.emplace_back();
        }
    }

    // Work on a copy: the original graph changes only if the result is valid.
    CutGraph g = graph;
    int added = 0;
    for (const Polyline& r : raw) {
        for (const Polyline& piece : clip_to_box(r, o.bounds)) {
            if (length(piece) < o.spacing) continue;  // a tap, not a cut
            StrokeInserter inserter(g, o);
            inserter.insert(resample(piece, o.spacing));
            added += inserter.added();
        }
    }
    if (added == 0) return 0;
    // The bounds check allows for rounding at the clip boundary.
    Eigen::AlignedBox2d bounds = o.bounds;
    const Eigen::Vector2d slack = Eigen::Vector2d::Constant(1e-6 * o.spacing);
    bounds.extend(o.bounds.min() - slack);
    bounds.extend(o.bounds.max() + slack);
    if (!is_valid(g, bounds, 0.02 * o.spacing)) return 0;  // pathological stroke: reject it whole
    graph = std::move(g);
    return added;
}

bool is_valid(const CutGraph& graph, const Eigen::AlignedBox2d& bounds, double min_distance) {
    const int n = static_cast<int>(graph.points.size());
    for (const Eigen::Vector2d& p : graph.points)
        if (!p.allFinite() || !bounds.contains(p)) return false;
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            if ((graph.points[i] - graph.points[j]).norm() < min_distance) return false;

    std::set<Edge> seen;
    for (const Edge& e : graph.segments) {
        if (e[0] < 0 || e[1] >= n || e[0] >= e[1]) return false;
        if (!seen.insert(e).second) return false;
    }
    for (size_t s = 0; s < graph.segments.size(); ++s) {
        const Edge& e = graph.segments[s];
        const Eigen::Vector2d a = graph.points[e[0]], b = graph.points[e[1]];
        // No foreign point on (or extremely close to) the segment.
        for (int k = 0; k < n; ++k) {
            if (k == e[0] || k == e[1]) continue;
            if (point_segment_distance(graph.points[k], a, b) < 0.5 * min_distance) return false;
        }
        // No proper crossing with any other segment.
        for (size_t s2 = s + 1; s2 < graph.segments.size(); ++s2) {
            const Edge& f = graph.segments[s2];
            if (f[0] == e[0] || f[0] == e[1] || f[1] == e[0] || f[1] == e[1]) continue;
            const Eigen::Vector2d c = graph.points[f[0]], d = graph.points[f[1]];
            const double d1 = cross(b - a, c - a), d2 = cross(b - a, d - a);
            const double d3 = cross(d - c, a - c), d4 = cross(d - c, b - c);
            if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0)))
                return false;
        }
    }
    return true;
}

}  // namespace skinsim
