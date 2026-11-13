#include "skinsim/simulation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

#include <Eigen/LU>  // provides Matrix::inverse(); Eigen/Core only declares it
#include <Eigen/SparseCholesky>

#include "skinsim/contact.hpp"
#include "skinsim/cuts.hpp"  // point_segment_distance
#include "skinsim/material.hpp"
#include "skinsim/mesh.hpp"

namespace skinsim {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

// Newton safeguards.
constexpr double kMaxStepFraction = 0.5;  // max node move per iteration, in mean edge lengths
constexpr double kArmijo = 1e-4;          // required fraction of predicted energy decrease
constexpr int kMaxLineSearchCuts = 30;    // 2^-30: the step is effectively zero by then
constexpr double kRelativeTolerance = 1e-8;
// Below this relative size, energy differences are dominated by rounding (see step()).
constexpr double kEnergyRoundoff = 1e-12;

// Replace NaN or values below lo by lo. See mesh.cpp for why it's written this way.
double at_least(double x, double lo) { return (x >= lo) ? x : lo; }

TissueParams sanitized(TissueParams p) {
    // Tiny positive floors instead of zero keep the stiffness matrix
    // invertible; at these values the material is effectively absent.
    p.shear_modulus = at_least(p.shear_modulus, 1e-9);
    p.thickness = at_least(p.thickness, 1e-3);
    p.tether_stiffness = at_least(p.tether_stiffness, 0.0);
    // Stiffening is capped: beyond ~100 the exponential overflows at strains
    // of a few percent, and no tissue is that extreme.
    p.stiffening = std::min(at_least(p.stiffening, 0.0), 100.0);
    if (!std::isfinite(p.fibre_angle)) p.fibre_angle = 0.0;
    p.fibre_stiffness = std::min(at_least(p.fibre_stiffness, 0.0), 100.0);
    p.fibre_nonlinearity = std::min(at_least(p.fibre_nonlinearity, 0.0), 100.0);
    p.fibre_dispersion = std::min(at_least(p.fibre_dispersion, 0.0), 1.0 / 3.0);
    // Prestretch between 0.5 and 2: beyond that it is not skin any more.
    p.prestretch_along = std::min(at_least(p.prestretch_along, 0.5), 2.0);
    p.prestretch_across = std::min(at_least(p.prestretch_across, 0.5), 2.0);
    p.undermined_tether = std::min(at_least(p.undermined_tether, 0.0), 1.0);
    p.wrinkling = std::min(at_least(p.wrinkling, 0.0), 0.99);
    return p;
}

SutureParams sanitized(SutureParams p) {
    // Capped so a typo can't wreck the conditioning of the stiffness matrix.
    p.stiffness = std::min(at_least(p.stiffness, 1e-6), 1e4);
    p.strip_width = std::min(at_least(p.strip_width, 0.05), 20.0);
    return p;
}

ContactParams sanitized(ContactParams p) {
    // Too soft and the lips sink into each other; too stiff and the stiffness
    // matrix becomes badly conditioned (contact springs dwarf the skin).
    p.stiffness = std::min(at_least(p.stiffness, 1e-3), 1e3);
    return p;
}

GrabParams sanitized(GrabParams p) {
    p.radius = at_least(p.radius, 0.0);
    p.stiffness = at_least(p.stiffness, 1e-9);
    p.max_force = at_least(p.max_force, 1e-9);  // +infinity passes through: "unlimited"
    return p;
}

// Smooth bell-shaped weight for nodes in a grab patch: 1 at the centre,
// falling to 0 (with zero slope) at distance r. A hard cut-off would make the
// grab jumpy as nodes enter/leave the patch on slightly different clicks.
double patch_kernel(double distance, double radius) {
    const double s = distance / radius;
    if (s >= 1.0) return 0.0;
    const double t = 1.0 - s * s;
    return t * t;
}

}  // namespace

Simulation::Simulation(TriMesh mesh) : mesh_(std::move(mesh)) {
    // std::move hands over the mesh's memory instead of copying it.
    const int n = mesh_.vertex_count();
    rest_ = Eigen::Map<const Eigen::VectorXd>(mesh_.rest_positions.data(), 2 * n);
    x_ = rest_;
    node_area_ = Eigen::VectorXd::Zero(n);

    // Precompute everything about each triangle that depends only on its rest
    // shape, so the per-iteration work is as small as possible.
    elements_.reserve(mesh_.triangles.size());
    for (int ti = 0; ti < mesh_.triangle_count(); ++ti) {
        const Triangle& t = mesh_.triangles[ti];
        const Eigen::Vector2d X0 = mesh_.rest_positions.col(t[0]);
        const Eigen::Vector2d X1 = mesh_.rest_positions.col(t[1]);
        const Eigen::Vector2d X2 = mesh_.rest_positions.col(t[2]);

        // Dm: the triangle's two edge vectors at rest, as columns.
        Eigen::Matrix2d Dm;
        Dm.col(0) = X1 - X0;
        Dm.col(1) = X2 - X0;
        const double area = 0.5 * Dm.determinant();
        if (!(area > 1e-12)) continue;  // skip degenerate triangles (none on a grid)

        // Linear triangle: inside the element, position is interpolated by the
        // shape functions N0, N1, N2 (the barycentric coordinates). Their
        // gradients are constant over the triangle, so F is constant per
        // element too: F = sum_a x_a (grad N_a)^T. The gradients come from Dm^-1.
        const Eigen::Matrix2d Dinv = Dm.inverse();
        Element e;
        e.triangle = ti;
        e.v = t;
        e.area = area;
        e.grad_n.row(1) = Dinv.row(0);
        e.grad_n.row(2) = Dinv.row(1);
        e.grad_n.row(0) = -(Dinv.row(0) + Dinv.row(1));  // shape functions sum to 1
        elements_.push_back(e);

        for (int a = 0; a < 3; ++a) node_area_[t[a]] += area / 3.0;
    }

    rest_locator_ = TriangleLocator(mesh_.rest_positions, mesh_.triangles);
    vertex_elements_.assign(n, {});
    for (int ei = 0; ei < static_cast<int>(elements_.size()); ++ei)
        for (int v : elements_[ei].v) vertex_elements_[v].push_back(ei);

    const std::vector<Edge> edges = unique_edges(mesh_);
    neighbours_.assign(n, {});
    double total = 0.0;
    for (const Edge& e : edges) {
        total += (mesh_.rest_positions.col(e[1]) - mesh_.rest_positions.col(e[0])).norm();
        neighbours_[e[0]].push_back(e[1]);
        neighbours_[e[1]].push_back(e[0]);
    }
    if (!edges.empty()) mean_edge_ = total / static_cast<double>(edges.size());

    // Boundary edges are the outer border plus the incision lips. Only the
    // outer border is pinned; the lips must be free to gape.
    const std::set<Edge> lips(mesh_.cut_edges.begin(), mesh_.cut_edges.end());
    for (const Edge& e : boundary_edges(mesh_)) {
        if (lips.count(e)) continue;
        boundary_nodes_.push_back(e[0]);
        boundary_nodes_.push_back(e[1]);
    }
    std::sort(boundary_nodes_.begin(), boundary_nodes_.end());
    boundary_nodes_.erase(std::unique(boundary_nodes_.begin(), boundary_nodes_.end()),
                          boundary_nodes_.end());

    // std::vector<bool> is a special space-saving type with surprising
    // behaviour in C++, so a vector of char is the usual workaround.
    fixed_.assign(2 * n, 0);
    undermining_ = Eigen::VectorXd::Zero(n);
    params_ = sanitized(params_);
    update_material();
    apply_pins();

    // Which triangle each lip edge belongs to (exactly one: it's a boundary
    // edge). Sutures attach to the lips through these triangles. The same
    // triangle tells which side of the edge is tissue: triangles are
    // counter-clockwise, so walking a -> b in the triangle's own order, its
    // inside is on the left. Contact needs exactly that orientation.
    lip_length_ = Eigen::VectorXd::Zero(n);
    for (int ti = 0; ti < mesh_.triangle_count(); ++ti) {
        const Triangle& t = mesh_.triangles[ti];
        for (int k = 0; k < 3; ++k) {
            const int a = t[k], b = t[(k + 1) % 3];
            const Edge e = a < b ? Edge{a, b} : Edge{b, a};
            if (!lips.count(e)) continue;
            lip_triangle_[e] = ti;
            lip_segments_.push_back({a, b});
            // Each end of a lip edge carries half of its length.
            const double half = 0.5 * (rest_.segment<2>(2 * b) - rest_.segment<2>(2 * a)).norm();
            lip_length_[a] += half;
            lip_length_[b] += half;
        }
    }
    for (int v = 0; v < n; ++v)
        if (lip_length_[v] > 0.0) lip_nodes_.push_back(v);
}

void Simulation::set_params(const TissueParams& params) {
    const TissueParams p = sanitized(params);
    const bool pins_changed = p.pin_boundary != params_.pin_boundary;
    params_ = p;
    update_material();
    if (pins_changed) apply_pins();
    converged_ = false;  // new parameters -> new equilibrium to find
}

// ---------------------------------------------------------------------------
// Material law with prestretch and fibres
// ---------------------------------------------------------------------------
//
// Prestretch: the rest mesh is skin as it is in the body (in vivo). Its
// natural, unloaded shape is smaller: X_rest = G X_natural, with
//   G = R diag(lambda_along, lambda_across) R^T,  R = rotation to the RSTL.
// The skin's elastic stretch is measured from the natural shape, so it is
//   F_e = F G      (F: rest -> current, G: natural -> rest).
// At rest F = I and F_e = G: the skin is under tension even though nothing
// moved. With the border pinned and no cuts that tension is in equilibrium;
// a cut releases it and the lips gape.
//
// The energy per rest volume is W(F_e) (incompressible: the natural and rest
// volumes are equal). By the chain rule
//   P = dW/dF = P_e(F_e) G^T,      K = M^T K_e(F_e) M,  M = G^T (x) I,
// since vec(F G) = (G^T (x) I) vec(F) ("(x)" = Kronecker product).
// With G = I and no fibres this is exactly the plain M2/M3 material.

void Simulation::update_material() {
    const double a = params_.fibre_angle * 3.14159265358979323846 / 180.0;
    Eigen::Matrix2d R;
    R << std::cos(a), -std::sin(a), std::sin(a), std::cos(a);
    prestretch_ = R * Eigen::Vector2d(params_.prestretch_along, params_.prestretch_across).asDiagonal() * R.transpose();
    // Kronecker product G^T (x) I2, written out: block (j, l) = G(l, j) * I2.
    prestretch_vec_.setZero();
    for (int j = 0; j < 2; ++j)
        for (int l = 0; l < 2; ++l)
            for (int i = 0; i < 2; ++i) prestretch_vec_(i + 2 * j, i + 2 * l) = prestretch_(l, j);

    fibres_.k1 = params_.fibre_stiffness;
    fibres_.k2 = params_.fibre_nonlinearity;
    fibres_.kappa = params_.fibre_dispersion;
    // G maps the RSTL direction onto itself (it is a principal direction of
    // G), so the fibre direction is the same in the natural and rest shapes.
    fibres_.direction = Eigen::Vector2d(std::cos(a), std::sin(a));
    material_.mu = params_.shear_modulus;
    material_.b = params_.stiffening;
    material_.fibres = fibres_;
}

// Wrinkling blends the plain energy W with the relaxed (tension-field) energy
// W_rel of wrinkling.hpp:  W_mix = (1 - r) W + r W_rel,  r = params_.wrinkling.
// Where the skin is taut W_rel = W, so nothing changes; under compression only
// the fraction 1 - r of the flat membrane's resistance is left. That remainder
// keeps W's barrier against squashing an element flat (1/J^2 -> infinity),
// which W_rel alone lacks (crumpled skin can fold over itself; a 2D sheet must not).
// Stress and stiffness blend the same way (derivatives are linear).

double Simulation::element_energy_density(const Eigen::Matrix2d& F) const {
    const Eigen::Matrix2d Fe = F * prestretch_;
    double W = membrane::energy_density(Fe, params_.shear_modulus, params_.stiffening);
    if (W == kInf) return kInf;
    W += membrane::fibre_energy_density(Fe, fibres_);
    const double r = params_.wrinkling;
    if (r > 0.0) W += r * (membrane::relaxed_energy_density(Fe, material_) - W);
    return W;
}

Eigen::Matrix2d Simulation::element_piola(const Eigen::Matrix2d& F) const {
    const Eigen::Matrix2d Fe = F * prestretch_;
    Eigen::Matrix2d Pe =
        membrane::first_piola(Fe, params_.shear_modulus, params_.stiffening) + membrane::fibre_piola(Fe, fibres_);
    const double r = params_.wrinkling;
    if (r > 0.0) Pe += r * (membrane::relaxed_piola(Fe, material_) - Pe);
    return Pe * prestretch_.transpose();
}

membrane::Mat4 Simulation::element_stiffness(const Eigen::Matrix2d& F) const {
    const Eigen::Matrix2d Fe = F * prestretch_;
    membrane::Mat4 Ke =
        membrane::stiffness(Fe, params_.shear_modulus, params_.stiffening) + membrane::fibre_stiffness(Fe, fibres_);
    const double r = params_.wrinkling;
    if (r > 0.0) Ke += r * (membrane::relaxed_stiffness(Fe, material_) - Ke);
    return prestretch_vec_.transpose() * Ke * prestretch_vec_;
}

double Simulation::tether_weight(int node) const {
    return node_area_[node] * (1.0 - undermining_[node] * (1.0 - params_.undermined_tether));
}

void Simulation::set_undermining(const Eigen::VectorXd& u) {
    if (u.size() != undermining_.size()) return;
    undermining_ = u.cwiseMax(0.0).cwiseMin(1.0);
    converged_ = false;
}

Eigen::Vector2d Simulation::rest_position_of(const MaterialPoint& p) const {
    return valid(p) ? point_at(p, rest_) : Eigen::Vector2d::Zero();
}

bool Simulation::current_position_at_rest(const Eigen::Vector2d& X, Eigen::Vector2d& out) const {
    MaterialPoint p;
    p.triangle = rest_locator_.locate(X, &p.weights, 1e-9);
    if (p.triangle < 0) return false;
    out = point_at(p, x_);
    return true;
}

void Simulation::set_grab_params(const GrabParams& params) {
    grab_params_ = sanitized(params);
    converged_ = false;
}

void Simulation::set_suture_params(const SutureParams& params) {
    const SutureParams p = sanitized(params);
    const bool strips_changed = p.strip_width != suture_params_.strip_width;
    // Switching snapping on must re-check sutures that are already overloaded.
    if (p.can_break != suture_params_.can_break) converged_ = false;
    suture_params_ = p;
    if (strips_changed)
        for (auto& [id, su] : sutures_) rebuild_suture(su);
    converged_ = false;
}

void Simulation::set_contact_params(const ContactParams& params) {
    contact_params_ = sanitized(params);
    pairs_valid_ = false;
    converged_ = false;
}

void Simulation::apply_pins() {
    std::fill(fixed_.begin(), fixed_.end(), 0);
    if (!params_.pin_boundary) return;
    for (int v : boundary_nodes_) {
        fixed_[2 * v] = fixed_[2 * v + 1] = 1;
        x_.segment<2>(2 * v) = rest_.segment<2>(2 * v);  // pinned nodes sit at rest
    }
}

void Simulation::set_dofs(const Eigen::VectorXd& x) {
    if (x.size() == x_.size()) x_ = x;
    apply_pins();
    converged_ = false;
}

void Simulation::reset() {
    x_ = rest_;
    grabs_.clear();
    converged_ = false;
}

// ---------------------------------------------------------------------------
// Grabs
// ---------------------------------------------------------------------------

bool Simulation::build_patch(const Eigen::Vector2d& point, double r, Grab& g) const {
    // 1. Is the point on the sheet? Find the triangle containing it in the
    //    *current* shape (what the user sees). A linear scan is fine for a few
    //    thousand triangles.
    int hit = -1;
    Eigen::Vector3d bary;
    for (int ei = 0; ei < static_cast<int>(elements_.size()) && hit < 0; ++ei) {
        const Element& e = elements_[ei];
        const Eigen::Vector2d a = x_.segment<2>(2 * e.v[0]);
        const Eigen::Vector2d b = x_.segment<2>(2 * e.v[1]);
        const Eigen::Vector2d c = x_.segment<2>(2 * e.v[2]);
        if (!(signed_area(a, b, c) > 0.0)) continue;
        bary = barycentric(a, b, c, point);
        if (bary.minCoeff() >= -1e-9) hit = ei;
    }
    if (hit < 0) return false;

    // 2. Collect the patch: nodes within the radius, weighted by
    //    (node area) x (bell-shaped kernel). Weighting by area is what makes the
    //    pull a *distributed* force that doesn't depend on how finely the mesh
    //    is divided: the same physical pull at any resolution.
    //    The patch grows outwards from the hit triangle along mesh edges (a
    //    "flood fill", or breadth-first search), so it only contains skin
    //    connected to the grabbed spot: there are no edges across an incision.
    g = Grab{};
    if (r > 0.0) {
        std::vector<char> seen(mesh_.vertex_count(), 0);
        std::vector<int> queue(elements_[hit].v.begin(), elements_[hit].v.end());
        for (int v : queue) seen[v] = 1;
        for (size_t qi = 0; qi < queue.size(); ++qi) {
            const int i = queue[qi];
            const double w = node_area_[i] * patch_kernel((x_.segment<2>(2 * i) - point).norm(), r);
            if (w <= 0.0) continue;  // outside the radius: don't spread further from here
            g.nodes.push_back(i);
            g.weights.push_back(w);
            for (int j : neighbours_[i])
                if (!seen[j]) {
                    seen[j] = 1;
                    queue.push_back(j);
                }
        }
    }
    if (g.nodes.size() >= 3) {
        g.radius = r;
    } else {
        // Radius smaller than the elements: fall back to the containing
        // triangle, weighted barycentrically (an exact point grab).
        g.nodes.assign(elements_[hit].v.begin(), elements_[hit].v.end());
        g.weights = {bary[0], bary[1], bary[2]};
        g.radius = 0.0;
    }
    double total = 0.0;
    for (double w : g.weights) total += w;
    for (double& w : g.weights) w /= total;  // weights sum to 1
    return true;
}

bool Simulation::grab(int id, const Eigen::Vector2d& point) {
    Grab g;
    if (!build_patch(point, grab_params_.radius, g)) return false;
    // The patch centre may differ slightly from the clicked point; keep that
    // offset so the skin doesn't jump when grabbed.
    const Eigen::Vector2d centre = grab_point(g, x_);
    g.offset = centre - point;
    g.target = centre;
    grabs_[id] = std::move(g);
    converged_ = false;
    return true;
}

void Simulation::adopt_grabs(const Simulation& other) {
    // `other.grabs_` is private, but in C++ "private" means private to the
    // class, not to the object: one Simulation may read another one's members.
    for (const auto& [id, old] : other.grabs_) {
        const Eigen::Vector2d centre = other.grab_point(old, other.x_);
        Grab g;
        if (!build_patch(centre, old.radius, g)) continue;  // skin no longer there
        const Eigen::Vector2d new_centre = grab_point(g, x_);
        const Eigen::Vector2d finger = old.target - old.offset;
        // Keep the spring's stretch (patch centre minus target), hence the force.
        g.target = new_centre - (centre - old.target);
        g.offset = g.target - finger;
        grabs_[id] = std::move(g);
    }
    converged_ = false;
}

void Simulation::move_grab(int id, const Eigen::Vector2d& finger) {
    auto it = grabs_.find(id);
    if (it == grabs_.end()) return;
    const Eigen::Vector2d target = finger + it->second.offset;
    if (it->second.target == target) return;
    it->second.target = target;
    converged_ = false;
}

std::vector<GrabInfo> Simulation::grab_info() const {
    std::vector<GrabInfo> out;
    for (const auto& [id, g] : grabs_) {
        GrabInfo info;
        info.id = id;
        info.point = grab_point(g, x_);
        info.target = g.target;
        info.force = grab_force(info.point - g.target).norm();
        info.force_limited = info.force >= grab_params_.max_force * (1.0 - 1e-9);
        info.radius = g.radius;
        out.push_back(info);
    }
    return out;
}

// The finger: a spring of stiffness k that saturates at max_force F.
// With gap s = |r| and s0 = F/k (the stretch at which the spring hits F):
//
//   s <= s0:  psi = k s^2 / 2          force = k s      (ordinary spring)
//   s >  s0:  psi = F (s - s0/2)       force = F        (constant pull, slipping)
//
// Both pieces meet with equal value and slope at s0, and psi stays convex.
// This "Huber" shape is common in robust statistics for the same reason: a
// quadratic core with linear tails, smooth enough for Newton.
double Simulation::grab_energy(const Eigen::Vector2d& r) const {
    const double k = grab_params_.stiffness, F = grab_params_.max_force;
    const double s = r.norm(), s0 = F / k;
    return (s <= s0) ? 0.5 * k * s * s : F * (s - 0.5 * s0);
}

Eigen::Vector2d Simulation::grab_force(const Eigen::Vector2d& r) const {
    const double k = grab_params_.stiffness, F = grab_params_.max_force;
    const double s = r.norm();
    return (k * s <= F) ? Eigen::Vector2d(k * r) : Eigen::Vector2d(F * r / s);
}

Eigen::Matrix2d Simulation::grab_stiffness(const Eigen::Vector2d& r) const {
    const double k = grab_params_.stiffness, F = grab_params_.max_force;
    const double s = r.norm();
    if (k * s <= F) return k * Eigen::Matrix2d::Identity();
    // Constant-magnitude pull: no stiffness along the pull direction n, and
    // F/s sideways (the direction of a constant force still turns with r).
    const Eigen::Vector2d n = r / s;
    return (F / s) * (Eigen::Matrix2d::Identity() - n * n.transpose());
}

void Simulation::release_grab(int id) {
    if (grabs_.erase(id) > 0) converged_ = false;
}

void Simulation::release_all_grabs() {
    if (!grabs_.empty()) converged_ = false;
    grabs_.clear();
}

Eigen::Vector2d Simulation::grab_point(const Grab& g, const Eigen::VectorXd& x) const {
    Eigen::Vector2d p = Eigen::Vector2d::Zero();
    for (size_t a = 0; a < g.nodes.size(); ++a) p += g.weights[a] * x.segment<2>(2 * g.nodes[a]);
    return p;
}

// ---------------------------------------------------------------------------
// Energy, gradient, Hessian
// ---------------------------------------------------------------------------

Eigen::Matrix2d Simulation::deformation_gradient(const Element& e, const Eigen::VectorXd& x) const {
    Eigen::Matrix2d F = Eigen::Matrix2d::Zero();
    for (int a = 0; a < 3; ++a) {
        // (2x1 column) * (1x2 row) = 2x2 "outer product"
        F += x.segment<2>(2 * e.v[a]) * e.grad_n.row(a);
    }
    return F;
}

double Simulation::energy(const Eigen::VectorXd& x) const {
    const double h = params_.thickness;
    double E = 0.0;

    for (const Element& e : elements_) {
        const double W = element_energy_density(deformation_gradient(e, x));
        if (W == kInf) return kInf;  // inverted element: this state is not allowed
        E += h * e.area * W;
    }

    const double k = params_.tether_stiffness;
    if (k > 0.0) {
        for (int i = 0; i < mesh_.vertex_count(); ++i) {
            E += 0.5 * k * tether_weight(i) * (x.segment<2>(2 * i) - rest_.segment<2>(2 * i)).squaredNorm();
        }
    }

    for (const auto& [id, g] : grabs_) {  // C++17 "structured binding" over map entries
        E += grab_energy(grab_point(g, x) - g.target);
    }

    for (const auto& [id, su] : sutures_)
        E += suture_energy(strip_point(su.bite_a, x) - strip_point(su.bite_b, x), su.rest_length);

    for (const ContactPair& c : contact_pairs(x)) E += 0.5 * contact_weight(c.node) * c.gap * c.gap;
    return E;
}

Eigen::VectorXd Simulation::gradient(const Eigen::VectorXd& x) const {
    const double h = params_.thickness;
    Eigen::VectorXd g = Eigen::VectorXd::Zero(x.size());

    for (const Element& e : elements_) {
        const Eigen::Matrix2d P = element_piola(deformation_gradient(e, x));
        // Chain rule: dE/dx_a = h * area * P * grad N_a. This is the internal
        // force the element exerts on node a (with the opposite sign).
        for (int a = 0; a < 3; ++a) {
            g.segment<2>(2 * e.v[a]) += h * e.area * P * e.grad_n.row(a).transpose();
        }
    }

    const double k = params_.tether_stiffness;
    if (k > 0.0) {
        for (int i = 0; i < mesh_.vertex_count(); ++i) {
            g.segment<2>(2 * i) += k * tether_weight(i) * (x.segment<2>(2 * i) - rest_.segment<2>(2 * i));
        }
    }

    for (const auto& [id, gr] : grabs_) {
        // Each patch node carries its weighted share of the finger's force.
        const Eigen::Vector2d f = grab_force(grab_point(gr, x) - gr.target);
        for (size_t a = 0; a < gr.nodes.size(); ++a) g.segment<2>(2 * gr.nodes[a]) += gr.weights[a] * f;
    }

    for (const auto& [id, su] : sutures_) {
        // The thread pulls each bite strip towards the other, the force
        // shared along the strip by its weights (a distributed load).
        std::vector<int> nodes;
        std::vector<double> c;
        suture_terms(su, nodes, c);
        const Eigen::Vector2d f = suture_force(strip_point(su.bite_a, x) - strip_point(su.bite_b, x), su.rest_length);
        for (size_t i = 0; i < nodes.size(); ++i) g.segment<2>(2 * nodes[i]) += c[i] * f;
    }

    for (const ContactPair& c : contact_pairs(x)) {
        // E = 1/2 w g^2  ->  dE/dq = w g dg/dq. With g < 0 this pushes the
        // node back out of the skin it entered and the lip the other way:
        // equal and opposite (Newton's third law), as the gap only depends on
        // relative positions. (For a vertex pair nodes[2] = nodes[1] and the
        // last entries of dg are 0.)
        int nodes[3];
        contact::Vec6 dg;
        pair_gap(c, x, nodes, &dg, nullptr);
        const contact::Vec6 f = contact_weight(c.node) * c.gap * dg;
        for (int k = 0; k < 3; ++k) g.segment<2>(2 * nodes[k]) += f.segment<2>(2 * k);
    }
    return g;
}

SparseMatrix Simulation::hessian(const Eigen::VectorXd& x, bool project_psd, bool include_grabs) const {
    const double h = params_.thickness;

    // Sparse matrices are built from a list of (row, col, value) "triplets";
    // duplicates are summed, which is exactly what assembly needs (several
    // elements contribute to the same node pair).
    std::vector<Eigen::Triplet<double>> trip;
    trip.reserve(elements_.size() * 36 + x.size() + grabs_.size() * 18);

    for (const Element& e : elements_) {
        membrane::Mat4 K = element_stiffness(deformation_gradient(e, x));
        if (project_psd) K = membrane::project_psd(K);

        // B maps the element's 6 nodal coordinates to the 4 entries of vec(F):
        // vecF[i + 2j] = sum_a x_a[i] * gradN_a[j]  ->  B(i + 2j, 2a + i) = gradN_a[j].
        Eigen::Matrix<double, 4, 6> B = Eigen::Matrix<double, 4, 6>::Zero();
        for (int a = 0; a < 3; ++a)
            for (int i = 0; i < 2; ++i)
                for (int j = 0; j < 2; ++j) B(i + 2 * j, 2 * a + i) = e.grad_n(a, j);

        const Eigen::Matrix<double, 6, 6> Ke = h * e.area * B.transpose() * K * B;
        for (int r = 0; r < 6; ++r)
            for (int c = 0; c < 6; ++c)
                trip.emplace_back(2 * e.v[r / 2] + r % 2, 2 * e.v[c / 2] + c % 2, Ke(r, c));
    }

    const double k = params_.tether_stiffness;
    if (k > 0.0) {
        for (int i = 0; i < static_cast<int>(x.size()); ++i) trip.emplace_back(i, i, k * tether_weight(i / 2));
    }

    for (const auto& [id, gr] : grabs_) {
        if (!include_grabs) break;
        // p = sum w_a x_a, so d2psi/dx_a dx_b = w_a w_b * (2x2 spring stiffness).
        // Note this couples *every pair* of patch nodes: a dense block. step()
        // therefore never assembles it and uses the Woodbury identity instead.
        const Eigen::Matrix2d Kg = grab_stiffness(grab_point(gr, x) - gr.target);
        for (size_t na = 0; na < gr.nodes.size(); ++na)
            for (size_t nb = 0; nb < gr.nodes.size(); ++nb)
                for (int i = 0; i < 2; ++i)
                    for (int j = 0; j < 2; ++j)
                        trip.emplace_back(2 * gr.nodes[na] + i, 2 * gr.nodes[nb] + j,
                                          gr.weights[na] * gr.weights[nb] * Kg(i, j));
    }

    for (const auto& [id, su] : sutures_) {
        // r = sum c_i x_i, so d2E/dx_i dx_j = c_i c_j K (K = 2x2 thread
        // stiffness). This couples every node of both strips (a dense block),
        // but a strip has only ~10-30 nodes, so it is assembled directly.
        std::vector<int> nodes;
        std::vector<double> c;
        suture_terms(su, nodes, c);
        const Eigen::Matrix2d K = suture_stiffness(strip_point(su.bite_a, x) - strip_point(su.bite_b, x), su.rest_length);
        if (K.isZero()) continue;  // slack thread: no stiffness
        for (size_t i = 0; i < nodes.size(); ++i)
            for (size_t j = 0; j < nodes.size(); ++j)
                for (int d = 0; d < 2; ++d)
                    for (int e = 0; e < 2; ++e)
                        trip.emplace_back(2 * nodes[i] + d, 2 * nodes[j] + e, c[i] * c[j] * K(d, e));
    }

    for (const ContactPair& c : contact_pairs(x)) {
        // d2E/dq2 = w (dg dg^T + g d2g). The first term is a plain spring
        // across the gap (always positive semi-definite). The second comes
        // from the lip rotating under the node, and can be indefinite: the
        // projected (safe) fallback drops it. (Measured: keeping it in the
        // first, exact attempt converges in far fewer iterations than
        // always dropping it, despite the fallback it sometimes triggers.)
        int nodes[3];
        contact::Vec6 dg;
        contact::Mat6 d2g;
        pair_gap(c, x, nodes, &dg, &d2g);
        contact::Mat6 K = dg * dg.transpose();
        // (Pressed into a lip vertex, the two terms together are w I: a plain
        // spring towards the vertex, never indefinite, and needed whole, or
        // the node could slide round the vertex freely. Kept always.)
        // (Pressed into a lip vertex, the two terms together are w I: a plain
        // spring towards the vertex, never indefinite, and needed whole, or
        // the node could slide round the vertex freely. Kept always.)
        if (c.segment < 0 || !project_psd) K += c.gap * d2g;
        K *= contact_weight(c.node);
        for (int r = 0; r < 6; ++r)
            for (int q = 0; q < 6; ++q) trip.emplace_back(2 * nodes[r / 2] + r % 2, 2 * nodes[q / 2] + q % 2, K(r, q));
    }

    SparseMatrix H(x.size(), x.size());
    H.setFromTriplets(trip.begin(), trip.end());
    return H;
}

SparseMatrix Simulation::constrain(const SparseMatrix& H) const {
    std::vector<Eigen::Triplet<double>> trip;
    trip.reserve(H.nonZeros());
    // Iterate over stored non-zeros: outer loop = columns, inner = entries in that column.
    for (int col = 0; col < H.outerSize(); ++col) {
        for (SparseMatrix::InnerIterator it(H, col); it; ++it) {
            const int r = static_cast<int>(it.row()), c = static_cast<int>(it.col());
            if (!fixed_[r] && !fixed_[c]) trip.emplace_back(r, c, it.value());
        }
    }
    for (int i = 0; i < static_cast<int>(H.rows()); ++i)
        if (fixed_[i]) trip.emplace_back(i, i, 1.0);
    SparseMatrix A(H.rows(), H.cols());
    A.setFromTriplets(trip.begin(), trip.end());
    return A;
}

double Simulation::max_node_force(const Eigen::VectorXd& g) const {
    double m = 0.0;
    for (int i = 0; i < mesh_.vertex_count(); ++i) m = std::max(m, g.segment<2>(2 * i).norm());
    return m;
}

double Simulation::residual_tolerance() const {
    // Typical force in the problem: membrane tension (mu * h) times an element
    // length, plus a tether spring over one element. "Converged" means the
    // largest leftover force is a tiny fraction of that.
    const double L = mean_edge_;
    const double scale = (params_.shear_modulus + params_.fibre_stiffness) * params_.thickness * L +
                         params_.tether_stiffness * L * L * L;
    return kRelativeTolerance * scale;
}

double Simulation::min_area_ratio() const {
    double m = kInf;
    for (const Element& e : elements_) m = std::min(m, deformation_gradient(e, x_).determinant());
    return m;
}

double Simulation::max_area_ratio() const {
    double m = -kInf;
    for (const Element& e : elements_) m = std::max(m, deformation_gradient(e, x_).determinant());
    return m;
}

Eigen::VectorXd Simulation::triangle_field(Field field) const {
    Eigen::VectorXd out = Eigen::VectorXd::Zero(mesh_.triangle_count());
    for (const Element& e : elements_) {
        const Eigen::Matrix2d F = deformation_gradient(e, x_);
        if (field == Field::AreaChange) {
            out[e.triangle] = F.determinant();  // relative to the rest (in vivo) area
            continue;
        }
        if (field == Field::ExcessSkin) {
            // Measured from the natural (unstretched) skin: F_e = F G.
            out[e.triangle] = membrane::excess_length(membrane::relax(F * prestretch_, material_));
            continue;
        }
        // Cauchy ("true") stress: force per area of the *deformed* skin.
        // In general sigma = P F^T / det(F_3D); skin is incompressible, so the
        // 3D volume ratio det(F_3D) is 1 and sigma = P F^T. With prestretch,
        // P F^T = P_e G^T F^T = P_e F_e^T. Plane stress means the third
        // (through-thickness) principal stress is 0.
        const Eigen::Matrix2d sigma = element_piola(F) * F.transpose();
        // Principal stresses: eigenvalues of the symmetric 2x2 tensor,
        // mean +/- radius of Mohr's circle.
        const double mean = 0.5 * (sigma(0, 0) + sigma(1, 1));
        const double half_diff = 0.5 * (sigma(0, 0) - sigma(1, 1));
        const double shear = 0.5 * (sigma(0, 1) + sigma(1, 0));
        const double radius = std::sqrt(half_diff * half_diff + shear * shear);
        const double s1 = mean + radius, s2 = mean - radius;
        out[e.triangle] = field == Field::MaxPrincipalStress   ? s1
                          : field == Field::MinPrincipalStress ? s2
                                                               : std::sqrt(s1 * s1 - s1 * s2 + s2 * s2);
    }
    return out;
}

Eigen::VectorXd Simulation::vertex_field(Field field) const {
    const Eigen::VectorXd per_triangle = triangle_field(field);
    Eigen::VectorXd sum = Eigen::VectorXd::Zero(mesh_.vertex_count());
    Eigen::VectorXd weight = Eigen::VectorXd::Zero(mesh_.vertex_count());
    for (const Element& e : elements_)
        for (int v : e.v) {
            sum[v] += e.area * per_triangle[e.triangle];
            weight[v] += e.area;
        }
    for (int v = 0; v < sum.size(); ++v)
        if (weight[v] > 0.0) sum[v] /= weight[v];
    return sum;
}

// ---------------------------------------------------------------------------
// Sutures
// ---------------------------------------------------------------------------

bool Simulation::valid(const MaterialPoint& p) const {
    return p.triangle >= 0 && p.triangle < mesh_.triangle_count() && p.weights.allFinite();
}

Eigen::Vector2d Simulation::point_at(const MaterialPoint& p, const Eigen::VectorXd& x) const {
    const Triangle& t = mesh_.triangles[p.triangle];
    return p.weights[0] * x.segment<2>(2 * t[0]) + p.weights[1] * x.segment<2>(2 * t[1]) +
           p.weights[2] * x.segment<2>(2 * t[2]);
}

Eigen::Vector2d Simulation::position_of(const MaterialPoint& p) const {
    return valid(p) ? point_at(p, x_) : Eigen::Vector2d::Zero();
}

void Simulation::suture_terms(const Suture& su, std::vector<int>& nodes, std::vector<double>& c) const {
    nodes = su.bite_a.nodes;
    c = su.bite_a.weights;
    for (size_t i = 0; i < su.bite_b.nodes.size(); ++i) {
        nodes.push_back(su.bite_b.nodes[i]);
        c.push_back(-su.bite_b.weights[i]);
    }
}

Eigen::Vector2d Simulation::strip_point(const Bite& b, const Eigen::VectorXd& x) const {
    Eigen::Vector2d p = Eigen::Vector2d::Zero();
    for (size_t i = 0; i < b.nodes.size(); ++i) p += b.weights[i] * x.segment<2>(2 * b.nodes[i]);
    return p;
}

double Simulation::nearest_cut_point(const Eigen::Vector2d& X, Eigen::Vector2d& W) const {
    W = X;
    double best = kInf;
    for (const Edge& e : mesh_.cut_edges) {
        const Eigen::Vector2d A = rest_.segment<2>(2 * e[0]), AB = rest_.segment<2>(2 * e[1]) - A;
        const double len2 = AB.squaredNorm();
        const double t = len2 > 0.0 ? std::clamp((X - A).dot(AB) / len2, 0.0, 1.0) : 0.0;
        const double d = (A + t * AB - X).norm();
        if (d < best) {
            best = d;
            W = A + t * AB;
        }
    }
    return best == kInf ? 0.0 : best;
}

// The bite of one side: the strip of tissue from the needle hole X to the
// nearest wound edge point W (at rest), strip_width wide, with rounded ends.
// The thread's pull is spread over that area as a distributed load q, with
// a bell-shaped profile across the strip (full on the centre line, fading to
// zero at the sides). In finite elements a distributed load reaches the nodes
// as the "consistent load vector"  f_i = integral of N_i q dA,  N_i being the
// node's shape function. It is computed here by sampling the strip densely
// (much finer than the elements): each sample finds its triangle and shares
// its weight among the three corners by its barycentric coordinates, which
// are exactly the N_i. Unlike picking "nodes inside the strip", this works at
// any mesh size and converges as the mesh is refined.
//
// Samples must stay on the needle's side of the incision (near W the strip
// touches the cut, where both lips share rest positions). So only triangles
// reachable from the needle hole along mesh edges, without crossing the cut,
// may receive load.
Simulation::Bite Simulation::build_bite(const MaterialPoint& entry) const {
    Bite bite;
    const Eigen::Vector2d X = point_at(entry, rest_);
    Eigen::Vector2d W;
    bite.depth = nearest_cut_point(X, W);
    const double hw = 0.5 * suture_params_.strip_width;

    // 1. Nodes reachable from the needle hole without crossing an incision,
    //    within reach of the strip.
    const int n = mesh_.vertex_count();
    const double reach = hw + 1.5 * mean_edge_;
    std::vector<char> reachable(n, 0);
    const Triangle& start = mesh_.triangles[entry.triangle];
    std::vector<int> queue(start.begin(), start.end());
    for (int v : queue) reachable[v] = 1;
    for (size_t qi = 0; qi < queue.size(); ++qi) {
        const int i = queue[qi];
        if (point_segment_distance(rest_.segment<2>(2 * i), X, W) > reach) continue;
        for (int j : neighbours_[i])
            if (!reachable[j]) {
                reachable[j] = 1;
                queue.push_back(j);
            }
    }

    // 2. Sample the strip on a fine grid aligned with it.
    const Eigen::Vector2d axis = (bite.depth > 1e-9) ? Eigen::Vector2d((W - X) / bite.depth) : Eigen::Vector2d(1, 0);
    const Eigen::Vector2d side(-axis.y(), axis.x());
    const double ds = hw / 6.0;
    std::map<int, double> load;  // node -> weight
    const int nu = static_cast<int>(std::ceil((bite.depth + 2.0 * hw) / ds));
    const int nv = static_cast<int>(std::ceil(2.0 * hw / ds));
    for (int iu = 0; iu < nu; ++iu) {
        for (int iv = 0; iv < nv; ++iv) {
            // Cell centres, from hw behind the needle hole to hw past W.
            const double u = -hw + (iu + 0.5) * ds, v = -hw + (iv + 0.5) * ds;
            const Eigen::Vector2d p = X + u * axis + v * side;
            const double d = point_segment_distance(p, X, W) / hw;
            if (d >= 1.0) continue;
            const double q = (1.0 - d * d) * (1.0 - d * d);  // load profile
            Eigen::Vector3d w;
            const int t = rest_locator_.locate(p, &w, 1e-9);
            if (t < 0) continue;  // off the sheet, or in the gap of an incision
            const Triangle& tri = mesh_.triangles[t];
            if (!reachable[tri[0]] || !reachable[tri[1]] || !reachable[tri[2]]) continue;  // other lip
            for (int k = 0; k < 3; ++k) load[tri[k]] += q * w[k];
        }
    }

    double total = 0.0;
    for (const auto& [node, w] : load)
        if (w > 0.0) {
            bite.nodes.push_back(node);
            bite.weights.push_back(w);
            total += w;
        }
    if (bite.nodes.empty() || !(total > 0.0)) {
        // Nothing sampled (a strip narrower than rounding, say): fall back to
        // the needle hole itself.
        bite.nodes.assign(start.begin(), start.end());
        bite.weights = {entry.weights[0], entry.weights[1], entry.weights[2]};
        total = 1.0;
    }
    for (double& w : bite.weights) w /= total;
    bite.centre_offset = (strip_point(bite, rest_) - W).norm();
    return bite;
}

void Simulation::rebuild_suture(Suture& s) const {
    s.bite_a = build_bite(s.a);
    s.bite_b = build_bite(s.b);
    // With the wound edges touching, the two strip centres sit their offsets
    // apart (straight across the wound). Tying tighter shortens the loop.
    s.rest_length = std::max(
        0.0, s.bite_a.centre_offset + s.bite_b.centre_offset - s.tightness * (s.bite_a.depth + s.bite_b.depth));
}

// The thread, as a function of the gap r between its two ends, tied to
// length L. With s = |r|:
//   s <= L:  slack, E = 0
//   s >  L:  E = k/2 (s - L)^2, pulling with force k (s - L) along r.
// Its Hessian splits into the direction along the thread (stiffness k) and
// across it (k (1 - L/s)): a taut string resists being stretched more than
// being swung sideways. For L = 0 both are k: the plain spring k I.
// E is continuous with a continuous slope at s = L, and convex, so Newton and
// the line search handle the switch between slack and taut.
double Simulation::suture_energy(const Eigen::Vector2d& r, double L) const {
    const double stretch = r.norm() - L;
    return stretch > 0.0 ? 0.5 * suture_params_.stiffness * stretch * stretch : 0.0;
}

Eigen::Vector2d Simulation::suture_force(const Eigen::Vector2d& r, double L) const {
    const double k = suture_params_.stiffness;
    if (L <= 0.0) return k * r;
    const double s = r.norm();
    return s > L ? Eigen::Vector2d(k * (s - L) * r / s) : Eigen::Vector2d::Zero();
}

Eigen::Matrix2d Simulation::suture_stiffness(const Eigen::Vector2d& r, double L) const {
    const double k = suture_params_.stiffness;
    if (L <= 0.0) return k * Eigen::Matrix2d::Identity();
    const double s = r.norm();
    if (!(s > L)) return Eigen::Matrix2d::Zero();
    const Eigen::Vector2d n = r / s;
    const Eigen::Matrix2d along = n * n.transpose();
    return k * (along + (1.0 - L / s) * (Eigen::Matrix2d::Identity() - along));
}

int Simulation::add_suture(const MaterialPoint& a, const MaterialPoint& b, double tightness, int thread) {
    if (!valid(a) || !valid(b) || !std::isfinite(tightness)) return -1;
    Suture s;
    s.a = a;
    s.b = b;
    s.tightness = tightness;
    s.thread = thread_strength(thread) > 0.0 ? thread : 0;
    rebuild_suture(s);
    const int id = next_suture_id_++;
    sutures_[id] = std::move(s);
    converged_ = false;
    return id;
}

bool Simulation::set_suture_tightness(int id, double tightness) {
    auto it = sutures_.find(id);
    if (it == sutures_.end() || !std::isfinite(tightness)) return false;
    it->second.tightness = tightness;
    rebuild_suture(it->second);
    converged_ = false;
    return true;
}

bool Simulation::set_suture_thread(int id, int thread) {
    auto it = sutures_.find(id);
    if (it == sutures_.end()) return false;
    it->second.thread = thread_strength(thread) > 0.0 ? thread : 0;
    converged_ = false;  // a weaker thread may now be overloaded
    return true;
}

bool Simulation::suture_strip(int id, int side, std::vector<int>& nodes, std::vector<double>& weights) const {
    auto it = sutures_.find(id);
    if (it == sutures_.end()) return false;
    const Bite& b = side == 0 ? it->second.bite_a : it->second.bite_b;
    nodes = b.nodes;
    weights = b.weights;
    return true;
}

bool Simulation::material_point_at(const Eigen::Vector2d& point, MaterialPoint& out) const {
    for (const Element& e : elements_) {
        const Eigen::Vector2d a = x_.segment<2>(2 * e.v[0]);
        const Eigen::Vector2d b = x_.segment<2>(2 * e.v[1]);
        const Eigen::Vector2d c = x_.segment<2>(2 * e.v[2]);
        if (!(signed_area(a, b, c) > 0.0)) continue;
        const Eigen::Vector3d w = barycentric(a, b, c, point);
        if (w.minCoeff() >= -1e-9) {
            out.triangle = e.triangle;
            out.weights = w;
            return true;
        }
    }
    return false;
}

double Simulation::bite(const MaterialPoint& p) const {
    if (!valid(p)) return 0.0;
    Eigen::Vector2d W;
    return nearest_cut_point(point_at(p, rest_), W);
}

bool Simulation::remove_suture(int id) {
    if (sutures_.erase(id) == 0) return false;
    converged_ = false;
    return true;
}

void Simulation::clear_sutures() {
    if (!sutures_.empty()) converged_ = false;
    sutures_.clear();
}

std::vector<SutureInfo> Simulation::suture_info() const {
    std::vector<SutureInfo> out;
    for (const auto& [id, su] : sutures_) {
        SutureInfo info;
        info.id = id;
        info.a = point_at(su.a, x_);
        info.b = point_at(su.b, x_);
        const Eigen::Vector2d ca = strip_point(su.bite_a, x_), cb = strip_point(su.bite_b, x_);
        info.force = suture_force(ca - cb, su.rest_length).norm();
        info.tightness = su.tightness;
        info.rest_length = su.rest_length;
        info.bites = su.bite_a.depth + su.bite_b.depth;
        info.thread = su.thread;
        info.tension = 0.5 * info.force;  // the loop crosses the wound twice
        info.strength = thread_strength(su.thread);
        info.load = info.strength > 0.0 ? info.tension / info.strength : 0.0;

        // Loop stress: the Cauchy stress component along the thread,
        // n^T sigma n, averaged over the elements of both bite strips with the
        // strips' own weights (each node shares its weight among its elements).
        // An average rather than a peak: it does not depend on the mesh.
        Eigen::Vector2d dir = cb - ca;
        if (dir.norm() < 1e-9) dir = info.b - info.a;
        const Eigen::Vector2d nrm = dir.norm() > 1e-12 ? Eigen::Vector2d(dir.normalized()) : Eigen::Vector2d(1, 0);
        std::map<int, double> element_weight;
        for (const Bite* b : {&su.bite_a, &su.bite_b})
            for (size_t i = 0; i < b->nodes.size(); ++i) {
                const auto& els = vertex_elements_[b->nodes[i]];
                for (int ei : els) element_weight[ei] += b->weights[i] / static_cast<double>(els.size());
            }
        double sum = 0.0, wsum = 0.0;
        for (const auto& [ei, w] : element_weight) {
            const Eigen::Matrix2d F = deformation_gradient(elements_[ei], x_);
            const Eigen::Matrix2d sigma = element_piola(F) * F.transpose();
            sum += w * nrm.dot(sigma * nrm);
            wsum += w;
        }
        info.loop_stress = wsum > 0.0 ? sum / wsum : 0.0;
        out.push_back(info);
    }
    return out;
}

bool Simulation::nearest_lip_point(const Eigen::Vector2d& point, double max_distance, MaterialPoint& out) const {
    double best = max_distance;
    bool found = false;
    for (const auto& [edge, tri] : lip_triangle_) {
        const Eigen::Vector2d a = x_.segment<2>(2 * edge[0]), b = x_.segment<2>(2 * edge[1]);
        const Eigen::Vector2d ab = b - a;
        const double len2 = ab.squaredNorm();
        const double t = len2 > 0.0 ? std::clamp((point - a).dot(ab) / len2, 0.0, 1.0) : 0.0;
        const double d = (a + t * ab - point).norm();
        if (d < best) {
            best = d;
            found = true;
            // Weights: (1 - t) on edge[0], t on edge[1], 0 on the third corner.
            out.triangle = tri;
            for (int k = 0; k < 3; ++k) {
                const int v = mesh_.triangles[tri][k];
                out.weights[k] = (v == edge[0]) ? 1.0 - t : (v == edge[1]) ? t : 0.0;
            }
        }
    }
    return found;
}

bool Simulation::opposite_lip_point(const MaterialPoint& p, MaterialPoint& out) const {
    if (!valid(p)) return false;
    const Eigen::Vector2d X = point_at(p, rest_);
    const Eigen::Vector2d here = point_at(p, x_);
    // The nodes p actually depends on (non-zero weight).
    const Triangle& tp = mesh_.triangles[p.triangle];
    auto uses = [&](int v) {
        for (int k = 0; k < 3; ++k)
            if (tp[k] == v && std::abs(p.weights[k]) > 1e-12) return true;
        return false;
    };
    // Candidates: lip edges through the same rest point X, built from
    // *different* nodes. A neighbouring edge of the same lip shares p's node;
    // at a cut's tip both lips share the single tip node: neither qualifies.
    // Where several cuts meet there can be more than one candidate: take the
    // one farthest away right now (the gap a suture would have to close).
    double best = -1.0;
    for (const auto& [edge, tri] : lip_triangle_) {
        const Eigen::Vector2d A = rest_.segment<2>(2 * edge[0]), B = rest_.segment<2>(2 * edge[1]);
        const Eigen::Vector2d AB = B - A;
        const double len2 = AB.squaredNorm();
        if (!(len2 > 0.0)) continue;
        const double t = std::clamp((X - A).dot(AB) / len2, 0.0, 1.0);
        if ((A + t * AB - X).norm() > 1e-7 * mean_edge_) continue;  // not through X
        if ((t < 1.0 - 1e-12 && uses(edge[0])) || (t > 1e-12 && uses(edge[1]))) continue;
        MaterialPoint q;
        q.triangle = tri;
        for (int k = 0; k < 3; ++k) {
            const int v = mesh_.triangles[tri][k];
            q.weights[k] = (v == edge[0]) ? 1.0 - t : (v == edge[1]) ? t : 0.0;
        }
        const double d = (point_at(q, x_) - here).norm();
        if (d > best) {
            best = d;
            out = q;
        }
    }
    return best >= 0.0;
}

// A material point's rest position X, and a point nudged a hair from X into
// its own triangle (towards the centroid). On an incision both lips share X;
// the nudged point lies on the correct lip.
static void rest_and_inside(const TriMesh& mesh, const Eigen::VectorXd& rest, const Eigen::Vector2d& X, int triangle,
                            Eigen::Vector2d& inside) {
    const Triangle& t = mesh.triangles[triangle];
    const Eigen::Vector2d centroid =
        (rest.segment<2>(2 * t[0]) + rest.segment<2>(2 * t[1]) + rest.segment<2>(2 * t[2])) / 3.0;
    inside = X + 1e-3 * (centroid - X);
}

bool Simulation::suture_record(int id, SutureRecord& out) const {
    auto it = sutures_.find(id);
    if (it == sutures_.end()) return false;
    const Suture& su = it->second;
    out.id = id;
    out.tightness = su.tightness;
    out.thread = su.thread;
    out.rest_a = point_at(su.a, rest_);
    out.rest_b = point_at(su.b, rest_);
    rest_and_inside(mesh_, rest_, out.rest_a, su.a.triangle, out.inside_a);
    rest_and_inside(mesh_, rest_, out.rest_b, su.b.triangle, out.inside_b);
    return true;
}

bool Simulation::locate_rest(const Eigen::Vector2d& X, const Eigen::Vector2d& inside, MaterialPoint& out) const {
    int tri = rest_locator_.locate(inside, nullptr, 1e-6);
    if (tri < 0) tri = rest_locator_.locate(X, nullptr, 1e-3);
    if (tri < 0) return false;
    const Triangle& t = mesh_.triangles[tri];
    out.triangle = tri;
    out.weights = barycentric(mesh_.rest_positions.col(t[0]), mesh_.rest_positions.col(t[1]),
                              mesh_.rest_positions.col(t[2]), X);
    return true;
}

int Simulation::restore_suture(const SutureRecord& r) {
    Suture s;
    s.tightness = r.tightness;
    s.thread = thread_strength(r.thread) > 0.0 ? r.thread : 0;
    if (!locate_rest(r.rest_a, r.inside_a, s.a) || !locate_rest(r.rest_b, r.inside_b, s.b)) return -1;
    rebuild_suture(s);  // this mesh: new bite strips, same tissue
    const int id = (r.id > 0 && sutures_.count(r.id) == 0) ? r.id : next_suture_id_;
    sutures_[id] = std::move(s);
    next_suture_id_ = std::max(next_suture_id_, id + 1);
    converged_ = false;
    return id;
}

// Re-create the sutures of a simulation of the same skin on another mesh:
// a snapshot of each in the old mesh, tied again in this one.
void Simulation::adopt_sutures(const Simulation& other) {
    for (const auto& [id, su] : other.sutures_) {
        SutureRecord r;
        if (other.suture_record(id, r)) restore_suture(r);
    }
    next_suture_id_ = std::max(next_suture_id_, other.next_suture_id_);
    converged_ = false;
}

std::vector<SutureBreak> Simulation::take_suture_breaks() {
    std::vector<SutureBreak> out;
    out.swap(suture_breaks_);
    return out;
}

// Called by step() once the sheet is in equilibrium. Only then are the
// suture forces meaningful: while Newton is still on its way (say, just after
// a suture is tied across a wide gap), the thread is momentarily stretched
// far more than it will be once the skin has followed.
//
// Only the single most overloaded suture snaps; the solver then finds the new
// equilibrium, in which its neighbours carry its share of the load, and the
// next check decides whether one of them snaps too. A closure that is too
// tight therefore fails one suture at a time, worst first, as it does in
// real life ("unzipping").
bool Simulation::snap_overloaded_suture() {
    if (!suture_params_.can_break) return false;
    int worst = -1;
    double worst_load = 1.0;
    SutureBreak brk;
    for (const SutureInfo& info : suture_info()) {
        if (info.strength > 0.0 && info.load > worst_load) {
            worst_load = info.load;
            worst = info.id;
            brk.id = info.id;
            brk.a = info.a;
            brk.b = info.b;
            brk.tension = info.tension;
            brk.thread = info.thread;
        }
    }
    if (worst < 0) return false;
    sutures_.erase(worst);
    suture_breaks_.push_back(brk);
    converged_ = false;
    return true;
}

// ---------------------------------------------------------------------------
// Contact between incision lips
// ---------------------------------------------------------------------------
//
// Without contact the two lips of a wound can pass through each other: a knot
// tied tight makes them overlap instead of pressing together. Here, a lip
// node that has entered skin other than its own is pushed back out by a
// penalty spring:
//
//   E = 1/2 w g^2,   w = k_c h l / 2,   g = -(depth of the node inside).
//
// k_c is the contact stiffness (pressure per mm of overlap), h the skin
// thickness and l the node's share of lip length, so the force is the contact
// pressure k_c |g| times the area h l it acts on: refining the mesh does not
// change the result. The 1/2: every lip node is checked, on both lips, so
// each side carries half of the pressure between them ("two-half-pass").
// Neither side is privileged, and a sharp corner of one lip that pokes into
// the other is caught as that corner's own node.
//
// The depth is the distance to the nearest point of a lip (a point inside a
// lip edge, or a lip vertex), i.e. the signed distance to the edge of the
// skin it entered. Two properties make this robust:
//  - It is continuous. A node can only get into or out of other skin by
//    crossing a lip, where the depth is 0, so the energy never jumps.
//    (Simpler rules, such as "behind which lip segment?", fail near corners
//    and where incisions cross: the energy jumps, and Newton's line search
//    stalls. That is how this design was arrived at.)
//  - It handles corners exactly: inside the corner of a flap the nearest
//    edge is the one actually pressed into.
// "Inside skin other than its own" is tested literally: is the node inside
// a triangle that it is not a corner of (TriangleLocator on the current
// shape)?
//
// Why a penalty, not a barrier: barrier methods (e.g. IPC, incremental
// potential contact) keep surfaces strictly apart with an energy that is
// infinite at zero gap. But right after a cut, both lips lie exactly on top of
// each other (gap 0): with a barrier, the freshly cut skin would start in an
// impossible state. A penalty is zero for touching lips and only acts on
// overlap, so a fresh cut, a gaping wound and a closed one all work the same
// way. The cost: a tiny overlap remains (pressure / k_c).
//
// Everything is found from the current positions every time, so a lip can
// slide any distance along another (a flap being inset). Contact keeps no
// history, which also means it carries over re-meshing for free. It is
// frictionless.

double Simulation::contact_weight(int node) const {
    return contact_params_.stiffness * params_.thickness * 0.5 * lip_length_[node];
}

// energy(), gradient() and hessian() are usually called at the same
// positions in turn (and the line search's tunnelling check once more), so
// the pairs of the last positions asked about are kept.
const std::vector<Simulation::ContactPair>& Simulation::contact_pairs(const Eigen::VectorXd& x) const {
    if (!(pairs_valid_ && pairs_x_.size() == x.size() && pairs_x_ == x)) {
        pairs_ = find_contact_pairs(x);
        pairs_x_ = x;
        pairs_valid_ = true;
    }
    return pairs_;
}

std::vector<Simulation::ContactPair> Simulation::find_contact_pairs(const Eigen::VectorXd& x) const {
    std::vector<ContactPair> pairs;
    if (!contact_params_.enabled || lip_segments_.empty()) return pairs;

    // 1. Which lip nodes lie inside skin that is not their own? A grid of
    //    the triangles in their current shape answers "which triangle is
    //    this point in?" quickly (TriangleLocator, as used for cutting).
    //    A lip node always sits on a corner of its own triangles: those are
    //    skipped. On the edge of another triangle counts as inside (on a lip
    //    edge the depth below is 0 anyway, so that costs nothing).
    const Eigen::Map<const Eigen::Matrix2Xd> pos(x.data(), 2, mesh_.vertex_count());
    const TriangleLocator locator(pos, mesh_.triangles);

    const size_t ns = lip_segments_.size();
    for (int node : lip_nodes_) {
        const Eigen::Vector2d p = pos.col(node);
        Eigen::Vector3d w;
        if (locator.locate(p, &w, 1e-12, node) < 0) continue;  // not inside other skin

        // 2. How deep: the distance to the nearest lip (not its own edges),
        //    i.e. to the edge of the skin it has entered. The nearest point
        //    is either inside a lip segment or one of the lip's vertices.
        ContactPair best{node, -1, -1, 0.0};
        double best_d = kInf;
        for (size_t k = 0; k < ns; ++k) {
            const LipSegment& sg = lip_segments_[k];
            if (sg.u == node || sg.v == node) continue;
            const Eigen::Vector2d u = pos.col(sg.u), v = pos.col(sg.v);
            // Cheap reject: farther from the segment's bounding box than the best so far.
            const Eigen::Vector2d lo = u.cwiseMin(v), hi = u.cwiseMax(v);
            const Eigen::Vector2d out = (lo - p).cwiseMax(p - hi).cwiseMax(0.0);
            if (out.norm() >= best_d) continue;
            const double s = (v - u).squaredNorm() > 1e-24 ? contact::projection(p, u, v) : 0.0;
            if (s > 0.0 && s < 1.0) {
                const double d = std::abs(contact::signed_gap(p, u, v));
                if (d < best_d) {
                    best_d = d;
                    best.segment = static_cast<int>(k);
                    best.vertex = -1;
                }
            } else {
                const int c = s <= 0.0 ? sg.u : sg.v;
                const double d = (p - pos.col(c)).norm();
                if (d < best_d) {
                    best_d = d;
                    best.segment = -1;
                    best.vertex = c;
                }
            }
        }
        if (!(best_d > 0.0) || best_d == kInf) continue;
        best.gap = -best_d;
        pairs.push_back(best);
    }
    return pairs;
}

// The gap of one contact pair, g = -(distance to the nearest lip point), and
// its derivatives over the coordinates of the nodes involved: (node, segment
// start, segment end), or (node, vertex) when the nearest point is a vertex
// (then the last two entries stay zero).
void Simulation::pair_gap(const ContactPair& c, const Eigen::VectorXd& x, int nodes[3], contact::Vec6* grad,
                          contact::Mat6* hess) const {
    const Eigen::Vector2d p = x.segment<2>(2 * c.node);
    nodes[0] = c.node;
    if (c.segment >= 0) {
        // The distance to a segment's line is |signed gap|. Inside the other
        // skin the node is normally behind the segment (signed gap < 0); the
        // sign flip covers the rare tangled case where it is not.
        const LipSegment& sg = lip_segments_[c.segment];
        nodes[1] = sg.u;
        nodes[2] = sg.v;
        const double g = contact::signed_gap(p, x.segment<2>(2 * sg.u), x.segment<2>(2 * sg.v), grad, hess);
        if (g > 0.0) {
            if (grad) *grad = -*grad;
            if (hess) *hess = -*hess;
        }
        return;
    }
    // Nearest to a vertex c: g = -|p - c|, so dg/dp = -n, dg/dc = +n with
    // n = (p - c) / |p - c|; the second derivative is the change of n,
    // (I - n n^T) / |p - c|.
    nodes[1] = nodes[2] = c.vertex;
    const Eigen::Vector2d d = p - x.segment<2>(2 * c.vertex);
    const double len = d.norm();
    const Eigen::Vector2d n = d / len;
    if (grad) {
        grad->setZero();
        grad->segment<2>(0) = -n;
        grad->segment<2>(2) = n;
    }
    if (hess) {
        hess->setZero();
        const Eigen::Matrix2d P = (Eigen::Matrix2d::Identity() - n * n.transpose()) / len;
        hess->block<2, 2>(0, 0) = -P;
        hess->block<2, 2>(0, 2) = P;
        hess->block<2, 2>(2, 0) = P;
        hess->block<2, 2>(2, 2) = -P;
    }
}

// A penalty only notices overlap that exists at the end of a step. Contact
// measures how deep a lip node is inside other skin, so the only overlap it
// can miss is a node that went right through a piece of skin (a narrow flap,
// say) and out the other side within one step ("tunnelling"): at the end it
// is outside skin again, on the wrong side. So each trial step of the line
// search is checked continuously: did an undetected lip node pass into skin
// through one lip and out through another? Then the step is too long.
bool Simulation::tunnels(const Eigen::VectorXd& from, const Eigen::VectorXd& to) const {
    if (!contact_params_.enabled || lip_segments_.empty()) return false;
    std::vector<char> detected(mesh_.vertex_count(), 0);
    for (const ContactPair& c : contact_pairs(to)) detected[c.node] = 1;

    // Broad phase: a uniform grid over the area the lips swept through.
    // Each segment is listed in every cell its swept bounding box touches;
    // a node then only checks the segments listed where it swept. Without
    // this, every node meets every segment: a million checks per trial step
    // on a sheet with many incisions.
    auto swept = [&](std::initializer_list<int> nodes, Eigen::Vector2d& lo, Eigen::Vector2d& hi) {
        lo = Eigen::Vector2d::Constant(kInf);
        hi = Eigen::Vector2d::Constant(-kInf);
        for (int v : nodes) {
            lo = lo.cwiseMin(from.segment<2>(2 * v)).cwiseMin(to.segment<2>(2 * v));
            hi = hi.cwiseMax(from.segment<2>(2 * v)).cwiseMax(to.segment<2>(2 * v));
        }
    };
    const size_t ns = lip_segments_.size();
    std::vector<Eigen::Vector2d> seg_lo(ns), seg_hi(ns);
    Eigen::Vector2d lo = Eigen::Vector2d::Constant(kInf), hi = Eigen::Vector2d::Constant(-kInf);
    for (size_t k = 0; k < ns; ++k) {
        swept({lip_segments_[k].u, lip_segments_[k].v}, seg_lo[k], seg_hi[k]);
        lo = lo.cwiseMin(seg_lo[k]);
        hi = hi.cwiseMax(seg_hi[k]);
    }
    if (!lo.allFinite() || !hi.allFinite()) return false;
    const double cell = std::max({mean_edge_, (hi - lo).maxCoeff() / 256.0, 1e-9});
    const int nx = static_cast<int>((hi.x() - lo.x()) / cell) + 1, ny = static_cast<int>((hi.y() - lo.y()) / cell) + 1;
    auto cell_of = [&](double v, double origin, int count) {
        return std::clamp(static_cast<int>((v - origin) / cell), 0, count - 1);
    };
    std::vector<std::vector<int>> grid(static_cast<size_t>(nx) * ny);
    for (size_t k = 0; k < ns; ++k)
        for (int j = cell_of(seg_lo[k].y(), lo.y(), ny); j <= cell_of(seg_hi[k].y(), lo.y(), ny); ++j)
            for (int i = cell_of(seg_lo[k].x(), lo.x(), nx); i <= cell_of(seg_hi[k].x(), lo.x(), nx); ++i)
                grid[static_cast<size_t>(j) * nx + i].push_back(static_cast<int>(k));

    std::vector<int> seen(ns, -1);  // last node that checked each segment (avoid repeats)
    for (int node : lip_nodes_) {
        if (detected[node]) continue;
        Eigen::Vector2d plo, phi;
        swept({node}, plo, phi);
        if (phi.x() < lo.x() || phi.y() < lo.y() || plo.x() > hi.x() || plo.y() > hi.y()) continue;
        const Eigen::Vector2d p0 = from.segment<2>(2 * node), p1 = to.segment<2>(2 * node);
        // When did it first pass into skin through a lip, and when did it
        // last pass out of skin through a lip? In, then out again later:
        // it went right through.
        double first_in = kInf, last_out = -kInf;
        for (int j = cell_of(plo.y(), lo.y(), ny); j <= cell_of(phi.y(), lo.y(), ny); ++j)
            for (int i = cell_of(plo.x(), lo.x(), nx); i <= cell_of(phi.x(), lo.x(), nx); ++i)
                for (int k : grid[static_cast<size_t>(j) * nx + i]) {
                    if (seen[k] == node) continue;
                    seen[k] = node;
                    const LipSegment& sg = lip_segments_[k];
                    if (sg.u == node || sg.v == node) continue;
                    if ((phi.array() < seg_lo[k].array()).any() || (plo.array() > seg_hi[k].array()).any()) continue;
                    contact::Crossing c[2];
                    const int n = contact::crossings(p0, p1, from.segment<2>(2 * sg.u), to.segment<2>(2 * sg.u),
                                                     from.segment<2>(2 * sg.v), to.segment<2>(2 * sg.v), c);
                    for (int q = 0; q < n; ++q) {
                        if (c[q].inward)
                            first_in = std::min(first_in, c[q].time);
                        else
                            last_out = std::max(last_out, c[q].time);
                    }
                }
        if (first_in < last_out - 1e-9) return true;
    }
    return false;
}

ContactInfo Simulation::contact_info() const {
    ContactInfo info;
    info.node_pressure = Eigen::VectorXd::Zero(mesh_.vertex_count());
    // Pressure = k_c x overlap. Both passes see (about) the same overlap from
    // either side, each carrying half the force, so this is the pressure
    // between the lips.
    for (const ContactPair& c : contact_pairs(x_)) {
        const double pressure = contact_params_.stiffness * -c.gap;
        info.node_pressure[c.node] = pressure;
        info.max_pressure = std::max(info.max_pressure, pressure);
        info.max_overlap = std::max(info.max_overlap, -c.gap);
        info.total_force += contact_weight(c.node) * -c.gap;
        ++info.pairs;
    }
    return info;
}

// ---------------------------------------------------------------------------
// Newton's method
// ---------------------------------------------------------------------------

StepInfo Simulation::step(int max_iterations) {
    if (converged_) {
        StepInfo idle = last_info_;
        idle.iterations = 0;
        idle.line_search_cuts = 0;
        return idle;
    }

    StepInfo info;
    const double tol = residual_tolerance();
    const double max_step = kMaxStepFraction * mean_edge_;

    auto free_gradient = [this](const Eigen::VectorXd& x) {
        Eigen::VectorXd g = gradient(x);
        for (int i = 0; i < g.size(); ++i)
            if (fixed_[i]) g[i] = 0.0;  // pinned dofs feel no net force by definition
        return g;
    };

    Eigen::VectorXd g = free_gradient(x_);
    double E = energy(x_);
    info.residual = max_node_force(g);

    for (int it = 0; it < max_iterations && info.residual > tol; ++it) {
        // 1. Newton direction: solve H d = -g. The quadratic model of E around x
        //    has its minimum at x + d.
        //
        //    Newton only converges quadratically with the *exact* Hessian, so
        //    try that first. LDL^T factorization reveals whether H is positive
        //    definite (all pivots D > 0); only then is d guaranteed downhill.
        //    Usually it is, even when a few compressed elements are individually
        //    unstable, because tethers, pins and neighbours stabilise them.
        //
        //    The grabs add a low-rank term: H = A + U C U^T, where A is the sparse
        //    skin + tether stiffness, U (n x 2m) holds each grab's patch weights
        //    (one column for x, one for y) and C (2m x 2m) the finger springs.
        //    Assembling U C U^T would couple every pair of patch nodes (a dense
        //    block), so instead we factorize only A and apply the Woodbury
        //    (push-through) identity:
        //      (A + U C U^T)^-1 r = z - Y C (I + U^T Y C)^-1 U^T z,
        //      with z = A^-1 r and Y = A^-1 U.
        //    That costs 2m extra sparse solves and one tiny dense 2m x 2m solve.
        //    Because A is positive definite and U C U^T is positive semi-definite
        //    (the finger springs are convex), checking A alone is enough.
        const int m = static_cast<int>(grabs_.size());
        Eigen::MatrixXd U = Eigen::MatrixXd::Zero(x_.size(), 2 * m);
        Eigen::MatrixXd C = Eigen::MatrixXd::Zero(2 * m, 2 * m);
        {
            int j = 0;
            for (const auto& [id, gr] : grabs_) {
                for (size_t a = 0; a < gr.nodes.size(); ++a)
                    for (int i = 0; i < 2; ++i) {
                        const int dof = 2 * gr.nodes[a] + i;
                        if (!fixed_[dof]) U(dof, 2 * j + i) = gr.weights[a];  // pinned dofs can't move
                    }
                C.block<2, 2>(2 * j, 2 * j) = grab_stiffness(grab_point(gr, x_) - gr.target);
                ++j;
            }
        }

        Eigen::SimplicialLDLT<SparseMatrix> ldlt;
        Eigen::VectorXd d;
        auto try_solve = [&](const SparseMatrix& A) {
            ldlt.compute(A);
            if (ldlt.info() != Eigen::Success || !(ldlt.vectorD().minCoeff() > 0.0)) return false;
            const Eigen::VectorXd z = ldlt.solve(-g);
            if (m == 0) {
                d = z;
            } else {
                const Eigen::MatrixXd Y = ldlt.solve(U);  // solves for all 2m columns at once
                const Eigen::MatrixXd S = Eigen::MatrixXd::Identity(2 * m, 2 * m) + U.transpose() * Y * C;
                d = z - Y * (C * S.partialPivLu().solve(U.transpose() * z));
            }
            return d.allFinite();
        };

        SparseMatrix H = constrain(hessian(x_, /*project_psd=*/false, /*include_grabs=*/false));
        bool solved = try_solve(H);
        if (!solved) {
            // Not positive definite: fall back to per-element PSD projection,
            // which is always safe but converges only linearly.
            info.projected = true;
            H = constrain(hessian(x_, /*project_psd=*/true, /*include_grabs=*/false));
            solved = try_solve(H);
        }
        const double diag_scale = std::max(1e-12, H.diagonal().cwiseAbs().maxCoeff());
        for (double shift = 1e-8 * diag_scale; !solved && shift < 1e3 * diag_scale; shift *= 100.0) {
            // Still singular (e.g. no tethers and no pins: the whole sheet can
            // slide for free). Add a small multiple of the identity. This is the
            // Levenberg-Marquardt idea: a bit of "gradient descent" mixed in.
            SparseMatrix I(H.rows(), H.cols());
            I.setIdentity();
            info.regularized = true;
            solved = try_solve(H + shift * I);
        }
        if (!solved) d = -g / diag_scale;

        // 2. Step clamp: never move a node more than half an element per
        //    iteration. Newton's quadratic model is only trustworthy nearby.
        double largest = 0.0;
        for (int i = 0; i < mesh_.vertex_count(); ++i) largest = std::max(largest, d.segment<2>(2 * i).norm());
        if (largest > max_step) d *= max_step / largest;

        // Safety net: d must point downhill (slope = dE/dalpha < 0).
        double slope = g.dot(d);
        if (!(slope < 0.0)) {
            d = -g;
            largest = max_node_force(d);
            if (largest > max_step) d *= max_step / largest;
            slope = g.dot(d);
        }

        // 3. Backtracking line search: halve the step until the energy drops by
        //    at least a small fraction of what the slope predicts (Armijo rule).
        //    A step that inverts an element has E = infinity and is rejected.
        //
        //    Near equilibrium the predicted decrease can be smaller than the
        //    rounding error of E itself (doubles carry ~16 significant digits,
        //    so E = 13.2 is only known to ~1e-15). Comparing energies then is a
        //    coin toss. In that regime we judge by the residual force instead,
        //    which is still computed accurately.
        Eigen::VectorXd x_try;
        Eigen::VectorXd g_try;
        double E_try = kInf;
        auto line_search = [&](const Eigen::VectorXd& dir, double dir_slope) {
            const bool energy_unreliable = -dir_slope < kEnergyRoundoff * std::max(1.0, std::abs(E));
            double alpha = 1.0;
            for (int cut = 0; cut <= kMaxLineSearchCuts; ++cut) {
                x_try = x_ + alpha * dir;
                E_try = energy(x_try);
                // A step that pushes a lip right through another is as illegal
                // as one that turns a triangle inside out.
                if (std::isfinite(E_try) && tunnels(x_, x_try)) E_try = kInf;
                bool accepted = false;
                if (energy_unreliable) {
                    if (std::isfinite(E_try)) {
                        g_try = free_gradient(x_try);
                        accepted = max_node_force(g_try) < info.residual;
                    }
                } else {
                    accepted = E_try <= E + kArmijo * alpha * dir_slope;
                }
                if (accepted) return true;
                alpha *= 0.5;
                ++info.line_search_cuts;
            }
            return false;
        };
        bool accepted = line_search(d, slope);
        if (!accepted && contact_params_.enabled && !lip_segments_.empty()) {
            // Contact makes the energy only piecewise smooth: where a lip
            // node is equally near two parts of the other lip, it has a kink.
            // Sitting on one, the Newton direction (built from one side's
            // curvature) can climb on the other side however short the step.
            // Steepest descent always goes downhill: try it before giving up.
            Eigen::VectorXd sd = -g;
            const double big = max_node_force(sd);
            if (big > max_step) sd *= max_step / big;
            accepted = line_search(sd, g.dot(sd));
        }
        if (!accepted) {
            // No downhill step exists at floating-point precision: we are as
            // close to equilibrium as doubles allow. Stop rather than spin.
            info.stalled = true;
            break;
        }

        x_ = x_try;
        E = E_try;
        g = free_gradient(x_);
        info.residual = max_node_force(g);
        ++info.iterations;
    }

    info.energy = E;
    info.converged = info.residual <= tol || info.stalled;
    converged_ = info.converged;
    // At equilibrium, a suture pulled beyond its thread's strength snaps;
    // the sheet then has to settle again.
    if (converged_ && snap_overloaded_suture()) info.converged = false;
    last_info_ = info;
    return info;
}

}  // namespace skinsim
