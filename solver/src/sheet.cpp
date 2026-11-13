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
    excised_.clear();  // without incisions nothing can be cut out
    flaps_.clear();
    if (!cuts_.empty()) remesh(CutGraph{});
}

void Sheet::remesh(CutGraph cuts) {
    auto sim = std::make_unique<Simulation>(mesh_sheet(geometry_, cuts, excised_));
    sim->set_params(sim_->params());
    sim->set_grab_params(sim_->grab_params());
    sim->set_suture_params(sim_->suture_params());
    sim->set_contact_params(sim_->contact_params());

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
// Excision
// ---------------------------------------------------------------------------

int Sheet::excise(const Eigen::Vector2d& point) {
    MaterialPoint m;
    if (!sim_->material_point_at(point, m)) return 0;
    // Nudge the rest point into its triangle: on an incision both lips share
    // rest positions, and the point must name the piece that was tapped.
    const Triangle& t = mesh().triangles[m.triangle];
    const Eigen::Vector2d centroid =
        (mesh().rest_positions.col(t[0]) + mesh().rest_positions.col(t[1]) + mesh().rest_positions.col(t[2])) / 3.0;
    const Eigen::Vector2d X = sim_->rest_position_of(m);
    return excise_rest(X + 1e-3 * (centroid - X));
}

int Sheet::excise_rest(const Eigen::Vector2d& rest_point) {
    const int where = island_at(mesh(), rest_point);  // -1 off skin, 0 attached, 1 island
    if (where < 0) return 0;
    if (where == 0) return -1;
    excised_.push_back(rest_point);
    remesh(cuts_);
    return 1;
}

int Sheet::excise_outline(const Polyline& outline, const Eigen::Vector2d& inside) {
    if (cut(outline) == 0) return -1;  // nothing cut (e.g. drawn off the skin)
    const int result = excise(inside);
    if (result != 1) undo_cut();  // the outline did not enclose anything: take it back
    return result;
}

bool Sheet::undo_excise() {
    if (excised_.empty()) return false;
    excised_.pop_back();
    remesh(cuts_);
    return true;
}

// ---------------------------------------------------------------------------
// Local flaps
// ---------------------------------------------------------------------------

int Sheet::make_flap(const FlapDesign& design, double undermine_margin) {
    if (!design.valid) return 0;
    // All the cuts as one stroke: one re-mesh, one undo step.
    CutGraph g = cuts_;
    const CutOptions options = geometry_.cut_options();
    int added = add_cut(g, design.defect, options);
    for (const Polyline& p : design.incisions) added += add_cut(g, p, options);
    if (added == 0) return 0;
    history_.push_back(cuts_);
    remesh(std::move(g));

    // Remove the defect's skin: the island around the design's centre.
    if (island_at(mesh(), design.centre) != 1) {
        CutGraph previous = std::move(history_.back());
        history_.pop_back();
        remesh(std::move(previous));
        return -1;
    }
    excised_.push_back(design.centre);
    remesh(cuts_);

    // Undermine the convex region around the defect and the flap, widened by
    // the margin: soft stamps on a grid, as if painted with the brush.
    Flap f;
    f.design = design;
    Polyline region = design.defect;
    region.insert(region.end(), design.flap.begin(), design.flap.end());
    Eigen::AlignedBox2d box;
    for (const Eigen::Vector2d& p : region) box.extend(p);
    const double margin = std::isfinite(undermine_margin) ? std::max(0.0, undermine_margin) : 0.0;
    const double radius = std::clamp(0.5 * margin, 2.5, 6.0);
    const double spacing = 0.6 * radius;  // inner 60% of each stamp is fully undermined
    auto distance_to = [](const Polyline& poly, const Eigen::Vector2d& p) {
        if (inside_polygon(poly, p)) return 0.0;
        double d = 1e300;
        for (size_t i = 0; i + 1 < poly.size(); ++i) d = std::min(d, point_segment_distance(p, poly[i], poly[i + 1]));
        return d;
    };
    std::vector<Stamp> stroke;
    const double reach = margin + 0.4 * radius;  // stamps fade out over their outer 40%
    for (double y = box.min().y() - reach; y <= box.max().y() + reach; y += spacing)
        for (double x = box.min().x() - reach; x <= box.max().x() + reach; x += spacing) {
            const Eigen::Vector2d p(x, y);
            if (std::min(distance_to(design.defect, p), distance_to(design.flap, p)) + 0.4 * radius <= reach + 1e-9)
                stroke.push_back({p, radius});
        }
    if (!stroke.empty()) {
        undermine_strokes_.push_back(std::move(stroke));
        f.undermine_stroke = static_cast<int>(undermine_strokes_.size()) - 1;
        apply_undermining();
    }
    flaps_.push_back(std::move(f));
    return 1;
}

bool Sheet::undo_flap() {
    if (flaps_.empty() || excised_.empty() || history_.empty()) return false;
    const Flap& f = flaps_.back();
    // Its undermining, unless that was cleared since.
    if (f.undermine_stroke >= 0 && static_cast<int>(undermine_strokes_.size()) == f.undermine_stroke + 1)
        undermine_strokes_.pop_back();
    flaps_.pop_back();
    excised_.pop_back();
    CutGraph previous = std::move(history_.back());
    history_.pop_back();
    remesh(std::move(previous));  // also re-applies the undermining
    return true;
}

std::vector<int> Sheet::tie_key_sutures(int i, double tightness, int thread) {
    std::vector<int> ids;
    if (i < 0 || i >= flap_count()) return ids;
    for (const auto& [entry, exit] : flaps_[i].design.key_sutures) {
        // The needle holes are a bite away from any cut, so the rest point
        // alone says where they are (no lip to choose).
        SutureRecord r;
        r.rest_a = r.inside_a = entry;
        r.rest_b = r.inside_b = exit;
        r.tightness = tightness;
        r.thread = thread;
        const int id = sim_->restore_suture(r);
        if (id >= 0) ids.push_back(id);
    }
    return ids;
}

double Sheet::flap_base_stress(int i) const {
    if (i < 0 || i >= flap_count()) return 0.0;
    const Polyline& base = flaps_[i].design.base;
    if (base.size() < 2) return 0.0;
    const TriMesh& m = mesh();
    const TriangleLocator locator(m.rest_positions, m.triangles);
    const Eigen::VectorXd stress = sim_->triangle_field(Field::MaxPrincipalStress);
    double sum = 0.0;
    int n = 0;
    // Sample the inner part of the base line (its ends touch the incisions).
    for (int k = 1; k < 10; ++k) {
        const Eigen::Vector2d p = base.front() + (base.back() - base.front()) * (k / 10.0);
        const int t = locator.locate(p, nullptr, 1e-6);
        if (t < 0) continue;
        sum += stress[t];
        ++n;
    }
    return n > 0 ? sum / n : 0.0;
}

// ---------------------------------------------------------------------------
// Lesions
// ---------------------------------------------------------------------------

bool Sheet::add_lesion(const Eigen::Vector2d& point, double diameter) {
    MaterialPoint m;
    if (!(diameter > 0.0) || !std::isfinite(diameter) || !sim_->material_point_at(point, m)) return false;
    add_lesion_rest(sim_->rest_position_of(m), diameter);
    return true;
}

void Sheet::add_lesion_rest(const Eigen::Vector2d& rest_centre, double diameter) {
    lesions_.push_back({rest_centre, 0.5 * diameter});
}

bool Sheet::undo_lesion() {
    if (lesions_.empty()) return false;
    lesions_.pop_back();
    return true;
}

Sheet::LesionStatus Sheet::lesion_status(int i) const {
    LesionStatus st;
    if (i < 0 || i >= static_cast<int>(lesions_.size())) return st;
    const Lesion& l = lesions_[i];
    const TriMesh& m = mesh();
    const TriangleLocator locator(m.rest_positions, m.triangles);
    // Sample the lesion (centre and three rings): which parts are still on skin?
    int on = 0, total = 0;
    auto sample = [&](const Eigen::Vector2d& p) {
        ++total;
        if (locator.locate(p, nullptr, 0.0) >= 0) ++on;
    };
    sample(l.centre);
    for (double f : {1.0 / 3.0, 2.0 / 3.0, 1.0})
        for (int k = 0; k < 48; ++k) {
            const double a = 2.0 * 3.14159265358979323846 * k / 48;
            sample(l.centre + f * l.radius * Eigen::Vector2d(std::cos(a), std::sin(a)));
        }
    st.excised = on < total;
    if (!st.excised) return st;
    // Margin: from the lesion's edge to the nearest skin left, i.e. to the
    // nearest lip (the defect is bordered by lips).
    double nearest = 1e300;
    for (const Edge& e : m.cut_edges)
        nearest = std::min(nearest, point_segment_distance(l.centre, m.rest_positions.col(e[0]), m.rest_positions.col(e[1])));
    st.margin = nearest - l.radius;
    st.complete = on == 0 && st.margin >= 0.0;
    return st;
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
