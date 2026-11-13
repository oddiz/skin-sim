// Sheet: the patch of skin as a whole: its geometry, its incisions, and the
// simulation running on the current mesh.
//
// Simulation works on one fixed mesh. Cutting changes the mesh, so Sheet
// handles the sequence around it:
//   1. map the stroke from where the user drew it (on the deformed skin) back
//      to rest coordinates, where incisions are stored,
//   2. add it to the cut graph (cleaned up, see cuts.hpp),
//   3. re-mesh the whole sheet with all incisions (see mesher.hpp),
//   4. carry the current deformation over to the new mesh, so the skin does
//      not jump when cut, and re-attach held grabs and tied sutures,
//   5. let the solver relax: the lips are now free and separate.
//
// Sheet also keeps the undermined areas, painted with a brush. Like the
// incisions they are stored in rest coordinates (a list of brush stamps), so
// they belong to the tissue and survive re-meshing exactly.
//
// Excisions are kept the same way: as a rest point inside each piece of skin
// that was cut out. Every re-mesh leaves those pieces out (see remove_islands).
// Lesions are markings on the skin (rest centre and radius): they have no
// mechanical effect, but Sheet can tell whether, and with what margin, they
// have been excised.
#pragma once

#include <memory>
#include <vector>

#include "skinsim/cuts.hpp"
#include "skinsim/flaps.hpp"
#include "skinsim/mesher.hpp"
#include "skinsim/simulation.hpp"

namespace skinsim {

class Sheet {
public:
    explicit Sheet(const SheetGeometry& geometry, CutGraph cuts = CutGraph{});

    const SheetGeometry& geometry() const { return geometry_; }
    Simulation& simulation() { return *sim_; }
    const Simulation& simulation() const { return *sim_; }
    const TriMesh& mesh() const { return sim_->mesh(); }
    const CutGraph& cuts() const { return cuts_; }

    // Number of strokes that can be undone.
    int cut_count() const { return static_cast<int>(history_.size()); }

    // Cut along a path drawn on the skin as it looks now (current, deformed
    // positions, mm). Parts of the path not over skin (e.g. across an open
    // wound) are skipped. Returns the number of cut segments added; 0 means
    // nothing was cut and nothing changed.
    int cut(const Polyline& path);
    // Same, with the path given in rest coordinates (tests, saved scenarios).
    int cut_rest(const Polyline& path);

    bool undo_cut();   // remove the most recent stroke; false if none
    void clear_cuts(); // remove all incisions ("heal" the skin)

    // --- excision ---
    // Remove the piece of skin under `point` (current, deformed positions),
    // which incisions must have cut free all round.
    // Returns 1 if removed, 0 if the point is not on skin, -1 if that skin is
    // still attached (the outline is not closed, or it is the main sheet).
    int excise(const Eigen::Vector2d& point);
    int excise_rest(const Eigen::Vector2d& rest_point);
    // Cut along a closed outline drawn on the skin (current positions) and
    // remove what it encloses, as one operation: the island containing
    // `inside` (current positions). Return codes as excise(); if nothing is
    // removed, the outline is not cut either.
    int excise_outline(const Polyline& outline, const Eigen::Vector2d& inside);
    bool undo_excise();  // put the most recently removed piece back
    int excision_count() const { return static_cast<int>(excised_.size()); }

    // --- local flaps ---
    // Carry out a flap design (rest coordinates, see flaps.hpp) in one step:
    // cut the defect outline and the flap's incisions, remove the skin in
    // the defect, and undermine the flap, the defect and `undermine_margin`
    // mm of skin around them. One undo step (undo_flap).
    // Returns 1 if done, 0 if nothing could be cut (e.g. off the sheet), -1
    // if the defect's skin did not come free (nothing is changed then).
    int make_flap(const FlapDesign& design, double undermine_margin);
    bool undo_flap();
    int flap_count() const { return static_cast<int>(flaps_.size()); }
    const FlapDesign& flap(int i) const { return flaps_.at(i).design; }
    // Tie the design's key sutures (thread: USP size, see thread_strength).
    // Returns the ids of those tied (an end no longer on skin is skipped).
    std::vector<int> tie_key_sutures(int i, double tightness, int thread);
    // Mean largest principal stress along the flap's base (pedicle), MPa:
    // how hard the closure pulls on the skin the flap hangs from.
    double flap_base_stress(int i) const;

    // --- lesions (markings) ---
    struct Lesion {
        Eigen::Vector2d centre;  // rest coordinates, mm
        double radius;           // mm
    };
    struct LesionStatus {
        bool excised = false;   // some or all of it has been cut out
        bool complete = false;  // all of it, with skin margin >= 0
        // Distance from the lesion's edge to the edge of the defect, mm (the
        // surgical margin). Negative: the defect edge runs through the
        // lesion (incomplete excision). Only meaningful if excised.
        double margin = 0.0;
    };
    // Mark a lesion of the given diameter at a point on the skin (current
    // positions). False if not on skin.
    bool add_lesion(const Eigen::Vector2d& point, double diameter);
    void add_lesion_rest(const Eigen::Vector2d& rest_centre, double diameter);
    bool undo_lesion();
    void clear_lesions() { lesions_.clear(); }
    const std::vector<Lesion>& lesions() const { return lesions_; }
    LesionStatus lesion_status(int i) const;

    // --- undermining ---
    // Undermine the skin within `radius` (mm) of a point on the skin as it
    // looks now. new_stroke = true starts a new brush stroke (one undo step),
    // false continues the last one. Returns false if the point is not on skin.
    bool undermine_at(const Eigen::Vector2d& point, double radius, bool new_stroke);
    bool undo_undermine();
    void clear_undermining();
    int undermine_count() const { return static_cast<int>(undermine_strokes_.size()); }
    // How undermined a rest point is: 1 well inside a brush stamp, fading
    // smoothly to 0 at its edge. Public for testing.
    double undermining_at(const Eigen::Vector2d& rest_point) const;

    // Positions on `to` (rest mesh) that reproduce the deformation `x_from`
    // of mesh `from`: each new vertex takes the position the old (piecewise
    // linear) deformation gives at its rest point. Public for testing.
    static Eigen::VectorXd transfer(const TriMesh& from, const Eigen::VectorXd& x_from, const TriMesh& to);

private:
    struct Stamp {
        Eigen::Vector2d centre;  // rest coordinates, mm
        double radius;           // mm
    };
    static double stamp_weight(const Stamp& s, const Eigen::Vector2d& rest_point);
    void apply_undermining();  // recompute the per-node values for the current mesh
    void remesh(CutGraph cuts);  // with the current excisions
    std::vector<Polyline> current_to_rest(const Polyline& path) const;

    SheetGeometry geometry_;
    CutGraph cuts_;
    std::vector<CutGraph> history_;  // the cut graph before each stroke, for undo
    std::unique_ptr<Simulation> sim_;
    std::vector<std::vector<Stamp>> undermine_strokes_;
    std::vector<Eigen::Vector2d> excised_;  // a rest point inside each removed piece
    std::vector<Lesion> lesions_;
    struct Flap {
        FlapDesign design;
        int undermine_stroke = -1;  // index in undermine_strokes_, -1 = none
    };
    std::vector<Flap> flaps_;
};

}  // namespace skinsim
