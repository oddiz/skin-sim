// Markings: lines drawn on the skin (pen strokes, rulers, lesion outlines),
// kept in rest coordinates so they belong to the tissue, and drawn where that
// tissue is now.
//
// Mapping only the drawn points and joining them with straight lines (what
// the app did before) goes wrong in two ways once the skin moves:
//   - Two neighbouring points on opposite sides of an incision both land on
//     skin, one on each lip, and the straight line between them jumps across
//     the open wound.
//   - Between sparse points the line is a straight chord, while the skin
//     under it stretches unevenly: curves flatten, lines "sag".
// So each segment is cut where it crosses an incision lip (the pieces end on
// their own lips), and sampled densely in between: a linear triangle maps a
// straight line to a straight line, so samples every 0.5 mm (about the
// element size) follow the deformed skin closely.
//
// Which triangle each sample lies in, and where the crossings are, depend
// only on the rest mesh, so they are worked out once per polyline and cached
// as material points (triangle + barycentric weights). Each frame then only
// evaluates them on the current positions: cheap enough for many markings.

#include <algorithm>
#include <cmath>
#include <cstring>

#include "skinsim/simulation.hpp"

namespace skinsim {

namespace {

double cross(const Eigen::Vector2d& a, const Eigen::Vector2d& b) { return a.x() * b.y() - a.y() * b.x(); }

// FNV-1a over the polyline's bytes: a cheap key for the cache.
std::uint64_t polyline_hash(const std::vector<Eigen::Vector2d>& pts, double max_step) {
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&](double v) {
        unsigned char bytes[sizeof(double)];
        std::memcpy(bytes, &v, sizeof(double));
        for (unsigned char b : bytes) {
            h ^= b;
            h *= 1099511628211ull;
        }
    };
    mix(max_step);
    for (const Eigen::Vector2d& p : pts) {
        mix(p.x());
        mix(p.y());
    }
    return h;
}

}  // namespace

const std::vector<std::vector<MaterialPoint>>& Simulation::rest_polyline_points(
    const std::vector<Eigen::Vector2d>& rest, double max_step) const {
    if (!(max_step > 0.0) || !std::isfinite(max_step)) max_step = 0.5;
    const std::uint64_t key = polyline_hash(rest, max_step);
    auto it = polyline_cache_.find(key);
    if (it != polyline_cache_.end() && it->second.max_step == max_step && it->second.rest == rest)
        return it->second.pieces;
    // Markings change rarely; a cap keeps a long session from growing it.
    if (polyline_cache_.size() > 512) polyline_cache_.clear();

    std::vector<std::vector<MaterialPoint>> pieces;
    std::vector<MaterialPoint> cur;
    auto flush = [&]() {
        if (cur.size() >= 2) pieces.push_back(cur);
        cur.clear();
    };
    // Add the tissue at rest point X, found from `inside` (a point a hair
    // away from X on the side we want: at a crossing, X itself lies on both
    // lips). Off the skin, the piece ends.
    auto add = [&](const Eigen::Vector2d& X, const Eigen::Vector2d& inside) {
        const int tri = rest_locator_.locate(inside, nullptr, 1e-9);
        if (tri < 0) {
            flush();
            return;
        }
        const Triangle& t = mesh_.triangles[tri];
        MaterialPoint m;
        m.triangle = tri;
        m.weights = barycentric(mesh_.rest_positions.col(t[0]), mesh_.rest_positions.col(t[1]),
                                mesh_.rest_positions.col(t[2]), X);
        cur.push_back(m);
    };

    // Distances below this (mm) count as "on a lip".
    constexpr double kOn = 1e-7;
    constexpr double kNudge = 1e-5;  // how far `inside` steps off a lip, mm
    auto near_edges = [&](const Eigen::Vector2d& lo, const Eigen::Vector2d& hi) {
        std::vector<int> out;
        for (int k = 0; k < static_cast<int>(mesh_.cut_edges.size()); ++k) {
            const Edge& e = mesh_.cut_edges[k];
            const Eigen::Vector2d P = mesh_.rest_positions.col(e[0]), Q = mesh_.rest_positions.col(e[1]);
            if (std::max(P.x(), Q.x()) < lo.x() - kOn || std::min(P.x(), Q.x()) > hi.x() + kOn ||
                std::max(P.y(), Q.y()) < lo.y() - kOn || std::min(P.y(), Q.y()) > hi.y() + kOn)
                continue;  // bounding boxes apart
            out.push_back(k);
        }
        return out;
    };
    auto on_lip = [&](const Eigen::Vector2d& X, const std::vector<int>& edges) {
        for (int k : edges) {
            const Edge& e = mesh_.cut_edges[k];
            const Eigen::Vector2d P = mesh_.rest_positions.col(e[0]), f = mesh_.rest_positions.col(e[1]) - P;
            const double len2 = f.squaredNorm();
            const double t = len2 > 0.0 ? std::clamp((X - P).dot(f) / len2, 0.0, 1.0) : 0.0;
            if ((P + t * f - X).squaredNorm() < kOn * kOn) return true;
        }
        return false;
    };
    // Is each drawn point exactly on a lip? (A stroke point can land right on
    // an incision; then the segments either side of it are on opposite sides.)
    std::vector<char> vertex_on_lip(rest.size(), 0);
    for (size_t i = 0; i < rest.size(); ++i)
        vertex_on_lip[i] = on_lip(rest[i], near_edges(rest[i], rest[i]));

    for (size_t i = 0; i + 1 < rest.size(); ++i) {
        const Eigen::Vector2d A = rest[i], d = rest[i + 1] - rest[i];
        const double len = d.norm();
        if (!(len > 1e-12)) continue;
        const Eigen::Vector2d dir = d / len;
        const Eigen::Vector2d left(-dir.y(), dir.x());
        const std::vector<int> edges = near_edges(A.cwiseMin(rest[i + 1]), A.cwiseMax(rest[i + 1]));

        // Where this segment crosses an incision lip (or a defect's edge:
        // those are cut edges too), strictly between its ends. Both lips of
        // an incision share their rest positions, so each crossing shows up
        // twice; merged below.
        std::vector<double> ts;
        for (int k : edges) {
            const Edge& e = mesh_.cut_edges[k];
            const Eigen::Vector2d P = mesh_.rest_positions.col(e[0]), f = mesh_.rest_positions.col(e[1]) - P;
            const double den = cross(d, f);
            if (std::abs(den) <= 1e-12 * len * f.norm()) continue;  // parallel (running along a lip)
            const double t = cross(P - A, f) / den;  // along this segment
            const double u = cross(P - A, d) / den;  // along the lip edge
            if (t * len > kOn && (1.0 - t) * len > kOn && u >= -1e-9 && u <= 1.0 + 1e-9) ts.push_back(t);
        }
        std::sort(ts.begin(), ts.end());
        ts.erase(std::unique(ts.begin(), ts.end(), [&](double a, double b) { return (b - a) * len < kOn; }),
                 ts.end());

        // A drawn point on a lip ends the piece there too.
        if (i > 0 && vertex_on_lip[i]) flush();

        std::vector<double> bounds{0.0};
        bounds.insert(bounds.end(), ts.begin(), ts.end());
        bounds.push_back(1.0);
        for (size_t k = 0; k + 1 < bounds.size(); ++k) {
            const double a = bounds[k], b = bounds[k + 1];
            if (k > 0) flush();  // crossed a lip: the line goes on from the other side
            const double span = (b - a) * len;
            const int n = std::max(1, static_cast<int>(std::ceil(span / max_step)));
            const double nudge = std::min(kNudge, 0.25 * span);
            const bool starts_on_lip = k > 0 || vertex_on_lip[i];
            const bool ends_on_lip = k + 2 < bounds.size() || vertex_on_lip[i + 1];
            for (int j = 0; j <= n; ++j) {
                // The first point of a segment is the last of the previous
                // one (unless the piece was broken there).
                if (j == 0 && k == 0 && i > 0 && !vertex_on_lip[i]) continue;
                const Eigen::Vector2d X = A + (a + (b - a) * j / n) * d;
                Eigen::Vector2d inside = X;
                if (j == 0 && starts_on_lip)
                    inside = X + nudge * dir;  // just past the lip
                else if (j == n && ends_on_lip)
                    inside = X - nudge * dir;  // just before the next lip
                else if (on_lip(X, edges))
                    inside = X + kNudge * left;  // running along a lip: keep to its left side
                add(X, inside);
            }
        }
    }
    flush();

    PolylineCacheEntry& entry = polyline_cache_[key];
    entry.rest = rest;
    entry.max_step = max_step;
    entry.pieces = std::move(pieces);
    return entry.pieces;
}

std::vector<std::vector<Eigen::Vector2d>> Simulation::map_rest_polyline(const std::vector<Eigen::Vector2d>& rest,
                                                                        double max_step) const {
    std::vector<std::vector<Eigen::Vector2d>> out;
    for (const std::vector<MaterialPoint>& piece : rest_polyline_points(rest, max_step)) {
        std::vector<Eigen::Vector2d> pts;
        pts.reserve(piece.size());
        for (const MaterialPoint& m : piece) pts.push_back(point_at(m, x_));
        out.push_back(std::move(pts));
    }
    return out;
}

}  // namespace skinsim
