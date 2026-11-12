// Unstructured meshing of the skin sheet with incisions, using CDT
// (constrained Delaunay triangulation, by artem-ogre).
//
// A Delaunay triangulation connects a set of points so that no point lies
// inside any triangle's circumscribed circle; among all triangulations of the
// same points it maximises the smallest angle, i.e. avoids thin triangles.
// "Constrained" means certain edges (the outline and the incisions) must
// appear in the mesh even where Delaunay would have chosen differently.
//
// CDT only connects the points it is given; it does no quality refinement.
// The quality comes from where we put the points:
//   - the outline, evenly spaced at the element size,
//   - a hexagonal lattice of interior "seed" points (Delaunay turns it into
//     equilateral triangles, the best shape for linear elements),
//   - points along each incision at twice that density (the lips need
//     resolution: they will gape, be sutured, and carry the highest stress),
//   - minus seeds too close to an incision, then a few passes of smoothing
//     to even out the transition.
// Finally the mesh is split along the incisions so each lip has its own nodes.
#pragma once

#include <Eigen/Core>
#include <Eigen/Geometry>

#include "skinsim/cuts.hpp"
#include "skinsim/mesh.hpp"

namespace skinsim {

struct SheetGeometry {
    double width = 100.0;        // mm
    double height = 60.0;        // mm
    double element_size = 2.5;   // mm, target triangle edge length

    // Points along an incision are this far apart (mm): twice the density.
    double cut_spacing() const { return 0.5 * element_size; }
    // Incisions stay at least one element away from the outer border. The
    // border is pinned ("the skin continues beyond the window"), so a cut
    // reaching it would have nowhere to open into anyway, and keeping clear of
    // it means cut points never collide with border points.
    double cut_margin() const { return element_size; }
    Eigen::AlignedBox2d cut_bounds() const;
    // Cut options consistent with this geometry (snap distances, bounds).
    CutOptions cut_options() const;
};

// Clamp invalid values (zero, negative, NaN, absurdly many elements) to sane ones.
SheetGeometry sanitized(SheetGeometry g);

// Number of smoothing passes the mesher uses (each followed by re-triangulation).
inline constexpr int kSmoothingPasses = 3;

// Mesh the rectangle (centred on the origin) with the given incisions.
// The result has CCW triangles, and TriMesh::cut_edges lists the incision lips.
// `cuts` must be a valid PSLG inside g.cut_bounds() (add_cut guarantees that);
// if it is not, the cuts are ignored and the plain sheet is returned.
// `excised`: rest points inside pieces of skin that have been cut out (see
// remove_islands); those pieces are left out of the mesh.
TriMesh mesh_sheet(const SheetGeometry& g, const CutGraph& cuts, const std::vector<Eigen::Vector2d>& excised = {});

// --- Excision ---
// An "island" is a piece of skin that incisions have cut free all round: a
// connected set of triangles with no edge on the sheet's outer border.
// (Connected = sharing an edge; after the mesh is split along the incisions,
// the two lips of a cut share no edge, so a closed outline really separates.)
//
// Which piece of skin is rest point p on? 1 = an island (it can be excised),
// 0 = skin still attached to the border, -1 = not on skin.
int island_at(const TriMesh& mesh, const Eigen::Vector2d& p);
// Remove the islands containing the given rest points (others are ignored).
// Returns how many were removed. The remaining lips of their outlines stay
// in cut_edges: they are the edge of the defect.
int remove_islands(TriMesh& mesh, const std::vector<Eigen::Vector2d>& points);

// Triangle shape quality: 1 for equilateral, -> 0 for slivers, < 0 if inverted.
// q = 4 sqrt(3) area / (sum of squared edge lengths).
double triangle_quality(const Eigen::Vector2d& a, const Eigen::Vector2d& b, const Eigen::Vector2d& c);

// Smallest interior angle of a triangle, degrees. Meshing tests use it.
double min_angle_deg(const Eigen::Vector2d& a, const Eigen::Vector2d& b, const Eigen::Vector2d& c);

}  // namespace skinsim
