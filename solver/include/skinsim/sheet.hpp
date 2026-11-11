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
//      not jump when cut, and re-attach any grabs that are being held,
//   5. let the solver relax: the lips are now free and separate.
#pragma once

#include <memory>
#include <vector>

#include "skinsim/cuts.hpp"
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

    // Positions on `to` (rest mesh) that reproduce the deformation `x_from`
    // of mesh `from`: each new vertex takes the position the old (piecewise
    // linear) deformation gives at its rest point. Public for testing.
    static Eigen::VectorXd transfer(const TriMesh& from, const Eigen::VectorXd& x_from, const TriMesh& to);

private:
    void remesh(CutGraph cuts);
    std::vector<Polyline> current_to_rest(const Polyline& path) const;

    SheetGeometry geometry_;
    CutGraph cuts_;
    std::vector<CutGraph> history_;  // the cut graph before each stroke, for undo
    std::unique_ptr<Simulation> sim_;
};

}  // namespace skinsim
