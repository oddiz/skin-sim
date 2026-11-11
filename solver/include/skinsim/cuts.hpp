// Incisions, stored as a planar straight-line graph (PSLG).
//
// A PSLG is a set of points joined by straight segments, where segments may
// only meet at shared points: they never cross or overlap. That is exactly the
// input a constrained Delaunay triangulation (CDT) needs: every segment becomes
// a mesh edge that the triangulation is not allowed to flip away.
//
// Everything here is in REST coordinates (mm): where the skin would be if
// nothing pulled on it. An incision is a property of the tissue, so it is
// stored in the tissue's own frame and moves with the skin when it deforms.
//
// Why so much care: a finger stroke is noisy, may cross itself or an earlier
// incision, may start "almost" on another cut, or run off the sheet. CDT
// reports bad input by throwing an exception, which in this build (exceptions
// off, as in Godot) terminates the program. So add_cut() cleans every stroke
// into a valid PSLG first:
//   1. clip to the allowed area (one element inside the sheet's border),
//   2. simplify (remove hand jitter, keep corners) and resample evenly,
//   3. snap points onto nearby existing cuts (so a Z-plasty limb can start
//      exactly on the central limb),
//   4. split segments where they cross, so crossings become shared points.
#pragma once

#include <vector>

#include <Eigen/Core>
#include <Eigen/Geometry>  // AlignedBox

#include "skinsim/mesh.hpp"

namespace skinsim {

using Polyline = std::vector<Eigen::Vector2d>;

struct CutGraph {
    std::vector<Eigen::Vector2d> points;  // rest coordinates, mm
    std::vector<Edge> segments;           // index pairs into points, [0] < [1]

    bool empty() const { return segments.empty(); }
};

struct CutOptions {
    // Target distance between consecutive points along a cut, mm. The mesher
    // places mesh vertices exactly at these points.
    double spacing = 1.25;
    // A new point closer than this to an existing cut point/segment is merged
    // into it. Interior points use `snap`, the two ends of a stroke use the
    // larger `endpoint_snap`: starting a cut on another one should "click" on.
    double snap = 0.6;
    double endpoint_snap = 2.0;
    // Cuts are clipped to this box (the sheet minus a margin), mm.
    Eigen::AlignedBox2d bounds{Eigen::Vector2d(-1e300, -1e300), Eigen::Vector2d(1e300, 1e300)};
};

// Add one stroke (rest coordinates) to the graph. Returns how many segments
// were added; 0 means nothing changed (too short, outside the bounds, or it
// only retraced existing cuts).
int add_cut(CutGraph& graph, const Polyline& path, const CutOptions& options);

// Douglas-Peucker simplification: keeps the points needed so that the
// simplified line stays within `tolerance` of the original. Removes jitter
// from hand-drawn strokes while keeping deliberate corners (a Z stays a Z).
Polyline simplify(const Polyline& path, double tolerance);

// Distance from p to the segment a-b, and the parameter t in [0, 1] of the
// closest point a + t (b - a).
double point_segment_distance(const Eigen::Vector2d& p, const Eigen::Vector2d& a, const Eigen::Vector2d& b,
                              double* t = nullptr);

// Check the PSLG rules (for tests and as a safety net before meshing): finite
// points inside `bounds`, valid distinct indices, no duplicate segments, no two
// segments crossing, no point lying on a segment it doesn't belong to, and no
// two points closer than min_distance.
bool is_valid(const CutGraph& graph, const Eigen::AlignedBox2d& bounds, double min_distance);

}  // namespace skinsim
