#include "skinsim/sheet.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace skinsim {

Sheet::Sheet(const SheetGeometry& geometry, CutGraph cuts) : geometry_(sanitized(geometry)), cuts_(std::move(cuts)) {
    // Cuts from outside (a saved scenario, a test) get the same scrutiny as
    // strokes: invalid ones are dropped rather than handed to the mesher.
    Eigen::AlignedBox2d bounds = geometry_.cut_bounds();
    const Eigen::Vector2d slack = Eigen::Vector2d::Constant(1e-6 * geometry_.element_size);
    bounds.extend(bounds.min() - slack);
    bounds.extend(bounds.max() + slack);
    if (!is_valid(cuts_, bounds, 0.01 * geometry_.cut_spacing())) cuts_ = CutGraph{};
    sim_ = std::make_unique<Simulation>(mesh_sheet(geometry_, cuts_));
}

std::vector<Polyline> Sheet::current_to_rest(const Polyline& path) const {
    // The skin under a point of the stroke is found in the *current* shape;
    // its barycentric coordinates in that triangle then give the same
    // material point in the rest shape. Within one triangle the deformation is
    // affine (linear elements), so this inverse map is exact.
    //
    // A straight stroke on deformed skin is generally curved at rest, so the
    // stroke is first sampled densely (half the cut spacing) and each sample
    // mapped separately.
    const TriMesh& m = sim_->mesh();
    const Eigen::Matrix2Xd current = sim_->positions();
    const TriangleLocator locator(current, m.triangles);
    const double step = 0.5 * geometry_.cut_spacing();

    std::vector<Polyline> pieces(1);
    auto add_sample = [&](const Eigen::Vector2d& p) {
        Eigen::Vector3d w;
        const int t = locator.locate(p, &w);
        if (t < 0) {  // not over skin: end the current piece
            if (!pieces.back().empty()) pieces.emplace_back();
            return;
        }
        const Triangle& tri = m.triangles[t];
        pieces.back().push_back(w[0] * m.rest_positions.col(tri[0]) + w[1] * m.rest_positions.col(tri[1]) +
                                w[2] * m.rest_positions.col(tri[2]));
    };
    for (size_t i = 0; i < path.size(); ++i) {
        if (!path[i].allFinite()) {
            if (!pieces.back().empty()) pieces.emplace_back();
            continue;
        }
        if (i == 0 || !path[i - 1].allFinite()) {
            add_sample(path[i]);
            continue;
        }
        const Eigen::Vector2d a = path[i - 1], b = path[i];
        const double len = (b - a).norm();
        const int n = std::max(1, static_cast<int>(std::ceil(len / step)));
        for (int k = 1; k <= n; ++k) add_sample(a + (b - a) * (static_cast<double>(k) / n));
    }
    return pieces;
}

int Sheet::cut(const Polyline& path) {
    CutGraph g = cuts_;
    const CutOptions options = geometry_.cut_options();
    int added = 0;
    for (const Polyline& piece : current_to_rest(path))
        if (piece.size() >= 2) added += add_cut(g, piece, options);
    if (added == 0) return 0;
    history_.push_back(cuts_);
    remesh(std::move(g));
    return added;
}

int Sheet::cut_rest(const Polyline& path) {
    CutGraph g = cuts_;
    const int added = add_cut(g, path, geometry_.cut_options());
    if (added == 0) return 0;
    history_.push_back(cuts_);
    remesh(std::move(g));
    return added;
}

bool Sheet::undo_cut() {
    if (history_.empty()) return false;
    CutGraph previous = std::move(history_.back());
    history_.pop_back();
    remesh(std::move(previous));
    return true;
}

void Sheet::clear_cuts() {
    history_.clear();
    if (!cuts_.empty()) remesh(CutGraph{});
}

void Sheet::remesh(CutGraph cuts) {
    auto sim = std::make_unique<Simulation>(mesh_sheet(geometry_, cuts));
    sim->set_params(sim_->params());
    sim->set_grab_params(sim_->grab_params());
    sim->set_suture_params(sim_->suture_params());

    // Carry the deformation over. In extreme states the interpolated
    // positions could turn a new triangle inside out (infinite energy, which
    // Newton cannot start from). Then blend towards rest, halving the
    // deformation until the state is valid; rest itself always is.
    const Eigen::VectorXd rest = sim->dofs();  // a new simulation starts at rest
    const Eigen::VectorXd moved = transfer(sim_->mesh(), sim_->dofs(), sim->mesh());
    bool valid = false;
    for (double a = 1.0; a > 1e-3 && !valid; a *= 0.5) {
        sim->set_dofs(rest + a * (moved - rest));
        valid = std::isfinite(sim->energy(sim->dofs()));
    }
    if (!valid) sim->set_dofs(rest);

    sim->adopt_grabs(*sim_);
    sim->adopt_sutures(*sim_);  // sutures stay at the same points of the tissue
    sim_ = std::move(sim);  // the old simulation is deleted here
    cuts_ = std::move(cuts);
    apply_undermining();    // new nodes: evaluate the painted areas on them
}

// ---------------------------------------------------------------------------
// Undermining
// ---------------------------------------------------------------------------

double Sheet::stamp_weight(const Stamp& s, const Eigen::Vector2d& rest_point) {
    // Full undermining in the inner 60% of the radius, then a smooth
    // (smoothstep) fade to 0 at the edge: a soft brush. A hard edge would put
    // an abrupt jump in tether stiffness between neighbouring nodes.
    const double r = (rest_point - s.centre).norm() / s.radius;
    if (r <= 0.6) return 1.0;
    if (r >= 1.0) return 0.0;
    const double t = (1.0 - r) / 0.4;
    return t * t * (3.0 - 2.0 * t);
}

double Sheet::undermining_at(const Eigen::Vector2d& rest_point) const {
    double u = 0.0;
    for (const auto& stroke : undermine_strokes_)
        for (const Stamp& s : stroke) u = std::max(u, stamp_weight(s, rest_point));
    return u;
}

void Sheet::apply_undermining() {
    const TriMesh& m = sim_->mesh();
    Eigen::VectorXd u(m.vertex_count());
    for (int v = 0; v < m.vertex_count(); ++v) u[v] = undermining_at(m.rest_positions.col(v));
    sim_->set_undermining(u);
}

bool Sheet::undermine_at(const Eigen::Vector2d& point, double radius, bool new_stroke) {
    MaterialPoint m;
    if (!(radius > 0.0) || !std::isfinite(radius) || !sim_->material_point_at(point, m)) return false;
    const Stamp stamp{sim_->rest_position_of(m), radius};
    if (new_stroke || undermine_strokes_.empty()) undermine_strokes_.emplace_back();
    std::vector<Stamp>& stroke = undermine_strokes_.back();
    // Skip stamps that would add (almost) nothing: a slow drag sends many.
    if (!stroke.empty() && (stroke.back().centre - stamp.centre).norm() < 0.2 * radius &&
        std::abs(stroke.back().radius - radius) < 1e-9)
        return true;
    stroke.push_back(stamp);
    // Update only what this stamp changes (max with the existing values),
    // instead of re-evaluating every stamp: painting stays fast.
    const TriMesh& mesh = sim_->mesh();
    Eigen::VectorXd u = sim_->undermining();
    for (int v = 0; v < mesh.vertex_count(); ++v)
        u[v] = std::max(u[v], stamp_weight(stamp, mesh.rest_positions.col(v)));
    sim_->set_undermining(u);
    return true;
}

bool Sheet::undo_undermine() {
    if (undermine_strokes_.empty()) return false;
    undermine_strokes_.pop_back();
    apply_undermining();
    return true;
}

void Sheet::clear_undermining() {
    undermine_strokes_.clear();
    apply_undermining();
}

Eigen::VectorXd Sheet::transfer(const TriMesh& from, const Eigen::VectorXd& x_from, const TriMesh& to) {
    const TriangleLocator locator(from.rest_positions, from.triangles);
    const int n = to.vertex_count();

    // One triangle of the new mesh per vertex, used to decide which side of
    // an incision the vertex belongs to (see below).
    std::vector<int> some_triangle(n, -1);
    for (int t = 0; t < to.triangle_count(); ++t)
        for (int v : to.triangles[t])
            if (some_triangle[v] < 0) some_triangle[v] = t;

    Eigen::VectorXd x(2 * n);
    for (int v = 0; v < n; ++v) {
        const Eigen::Vector2d X = to.rest_positions.col(v);
        // On an existing incision both lips sit at the same rest point, so
        // "the old triangle containing X" is ambiguous. Look up a point nudged
        // slightly into one of this vertex's own triangles instead: that
        // triangle lies on this vertex's side of the incision.
        Eigen::Vector2d probe = X;
        if (some_triangle[v] >= 0) {
            const Triangle& t = to.triangles[some_triangle[v]];
            const Eigen::Vector2d centroid =
                (to.rest_positions.col(t[0]) + to.rest_positions.col(t[1]) + to.rest_positions.col(t[2])) / 3.0;
            probe = X + 1e-3 * (centroid - X);
        }
        int t = locator.locate(probe, nullptr, 1e-6);
        if (t < 0) t = locator.locate(X, nullptr, 1e-3);
        if (t < 0) {  // not on the old sheet at all (should not happen): leave at rest
            x.segment<2>(2 * v) = X;
            continue;
        }
        // Evaluate that triangle's (affine) deformation at X itself, so the
        // transfer is exact for any deformation that is affine near X.
        const Triangle& tri = from.triangles[t];
        const Eigen::Vector3d w = barycentric(from.rest_positions.col(tri[0]), from.rest_positions.col(tri[1]),
                                              from.rest_positions.col(tri[2]), X);
        x.segment<2>(2 * v) = w[0] * x_from.segment<2>(2 * tri[0]) + w[1] * x_from.segment<2>(2 * tri[1]) +
                              w[2] * x_from.segment<2>(2 * tri[2]);
    }
    return x;
}

}  // namespace skinsim
