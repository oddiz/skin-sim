#include "skinsim/simulation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

#include <Eigen/LU>  // provides Matrix::inverse(); Eigen/Core only declares it
#include <Eigen/SparseCholesky>

#include "skinsim/material.hpp"

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
    for (const Triangle& t : mesh_.triangles) {
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
        e.v = t;
        e.area = area;
        e.grad_n.row(1) = Dinv.row(0);
        e.grad_n.row(2) = Dinv.row(1);
        e.grad_n.row(0) = -(Dinv.row(0) + Dinv.row(1));  // shape functions sum to 1
        elements_.push_back(e);

        for (int a = 0; a < 3; ++a) node_area_[t[a]] += area / 3.0;
    }

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
    params_ = sanitized(params_);
    apply_pins();
}

void Simulation::set_params(const TissueParams& params) {
    const TissueParams p = sanitized(params);
    const bool pins_changed = p.pin_boundary != params_.pin_boundary;
    params_ = p;
    if (pins_changed) apply_pins();
    converged_ = false;  // new parameters -> new equilibrium to find
}

void Simulation::set_grab_params(const GrabParams& params) {
    grab_params_ = sanitized(params);
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
    const double mu = params_.shear_modulus;
    const double b = params_.stiffening;
    const double h = params_.thickness;
    double E = 0.0;

    for (const Element& e : elements_) {
        const double W = membrane::energy_density(deformation_gradient(e, x), mu, b);
        if (W == kInf) return kInf;  // inverted element: this state is not allowed
        E += h * e.area * W;
    }

    const double k = params_.tether_stiffness;
    if (k > 0.0) {
        for (int i = 0; i < mesh_.vertex_count(); ++i) {
            E += 0.5 * k * node_area_[i] * (x.segment<2>(2 * i) - rest_.segment<2>(2 * i)).squaredNorm();
        }
    }

    for (const auto& [id, g] : grabs_) {  // C++17 "structured binding" over map entries
        E += grab_energy(grab_point(g, x) - g.target);
    }
    return E;
}

Eigen::VectorXd Simulation::gradient(const Eigen::VectorXd& x) const {
    const double mu = params_.shear_modulus;
    const double b = params_.stiffening;
    const double h = params_.thickness;
    Eigen::VectorXd g = Eigen::VectorXd::Zero(x.size());

    for (const Element& e : elements_) {
        const Eigen::Matrix2d P = membrane::first_piola(deformation_gradient(e, x), mu, b);
        // Chain rule: dE/dx_a = h * area * P * grad N_a. This is the internal
        // force the element exerts on node a (with the opposite sign).
        for (int a = 0; a < 3; ++a) {
            g.segment<2>(2 * e.v[a]) += h * e.area * P * e.grad_n.row(a).transpose();
        }
    }

    const double k = params_.tether_stiffness;
    if (k > 0.0) {
        for (int i = 0; i < mesh_.vertex_count(); ++i) {
            g.segment<2>(2 * i) += k * node_area_[i] * (x.segment<2>(2 * i) - rest_.segment<2>(2 * i));
        }
    }

    for (const auto& [id, gr] : grabs_) {
        // Each patch node carries its weighted share of the finger's force.
        const Eigen::Vector2d f = grab_force(grab_point(gr, x) - gr.target);
        for (size_t a = 0; a < gr.nodes.size(); ++a) g.segment<2>(2 * gr.nodes[a]) += gr.weights[a] * f;
    }
    return g;
}

SparseMatrix Simulation::hessian(const Eigen::VectorXd& x, bool project_psd, bool include_grabs) const {
    const double mu = params_.shear_modulus;
    const double b = params_.stiffening;
    const double h = params_.thickness;

    // Sparse matrices are built from a list of (row, col, value) "triplets";
    // duplicates are summed, which is exactly what assembly needs (several
    // elements contribute to the same node pair).
    std::vector<Eigen::Triplet<double>> trip;
    trip.reserve(elements_.size() * 36 + x.size() + grabs_.size() * 18);

    for (const Element& e : elements_) {
        membrane::Mat4 K = membrane::stiffness(deformation_gradient(e, x), mu, b);
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
        for (int i = 0; i < static_cast<int>(x.size()); ++i) trip.emplace_back(i, i, k * node_area_[i / 2]);
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
    const double scale = params_.shear_modulus * params_.thickness * L +
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
        const bool energy_unreliable = -slope < kEnergyRoundoff * std::max(1.0, std::abs(E));
        double alpha = 1.0;
        Eigen::VectorXd x_try;
        Eigen::VectorXd g_try;
        double E_try = kInf;
        bool accepted = false;
        for (int cut = 0; cut <= kMaxLineSearchCuts; ++cut) {
            x_try = x_ + alpha * d;
            E_try = energy(x_try);
            if (energy_unreliable) {
                if (std::isfinite(E_try)) {
                    g_try = free_gradient(x_try);
                    accepted = max_node_force(g_try) < info.residual;
                }
            } else {
                accepted = E_try <= E + kArmijo * alpha * slope;
            }
            if (accepted) break;
            alpha *= 0.5;
            ++info.line_search_cuts;
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
    last_info_ = info;
    return info;
}

}  // namespace skinsim
