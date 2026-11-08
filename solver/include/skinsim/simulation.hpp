// Quasi-static simulation of the skin sheet.
//
// "Quasi-static" means there is no inertia: at every moment the sheet sits in
// (or is moving towards) mechanical equilibrium. That is the right model for
// skin being slowly pulled with forceps, and it is far more stable than
// simulating velocities and accelerations.
//
// The state is the vector x of all node positions, laid out x0, y0, x1, y1, ...
// (2N numbers, "degrees of freedom" or dofs). The total potential energy is
//
//   E(x) = sum over triangles   thickness * area * W(F)          (skin)
//        + sum over nodes       1/2 k A_i |x_i - X_i|^2          (tethers)
//        + sum over grabs       1/2 k_g |p(x) - target|^2        (fingers)
//
// Equilibrium = minimum of E. step() moves towards it with Newton's method.
#pragma once

#include <array>
#include <map>
#include <vector>

#include <Eigen/Core>
#include <Eigen/SparseCore>

#include "skinsim/mesh.hpp"
#include "skinsim/params.hpp"

namespace skinsim {

// Diagnostics from one call to step(). Worth showing in a HUD: they tell you
// whether the solver is keeping up.
struct StepInfo {
    int iterations = 0;        // Newton iterations performed this call
    int line_search_cuts = 0;  // total step halvings (many = solver struggling)
    bool projected = false;    // exact Hessian was indefinite; used PSD-projected fallback
    bool regularized = false;  // the stiffness matrix needed a diagonal shift to factorize
    bool converged = false;    // residual below tolerance (sheet at rest)
    bool stalled = false;      // line search could not reduce energy further
    double energy = 0.0;       // total energy after the step, N*mm
    double residual = 0.0;     // largest net force on any free node, N
};

using SparseMatrix = Eigen::SparseMatrix<double>;

class Simulation {
public:
    // `explicit` stops C++ from silently converting a TriMesh into a
    // Simulation where you didn't ask for one.
    explicit Simulation(TriMesh mesh);

    // --- parameters (may change between any two steps) ---
    void set_params(const TissueParams& params);
    const TissueParams& params() const { return params_; }

    // --- state ---
    const TriMesh& mesh() const { return mesh_; }
    // Current positions as a 2 x N matrix, viewing the same memory as the dof
    // vector (no copy). The Map type is Eigen's "view" of existing memory.
    Eigen::Map<const Eigen::Matrix2Xd> positions() const {
        return Eigen::Map<const Eigen::Matrix2Xd>(x_.data(), 2, mesh_.vertex_count());
    }
    const Eigen::VectorXd& dofs() const { return x_; }
    void set_dofs(const Eigen::VectorXd& x);  // for tests; marks the state dirty
    void reset();                             // back to rest, grabs released

    // --- interaction ---
    // Grab whatever material point is under `point` (current configuration, mm).
    // `id` is chosen by the caller, e.g. the touch index, so several fingers can
    // grab at once. Returns false if the point is not on the sheet.
    bool grab(int id, const Eigen::Vector2d& point);
    void move_grab(int id, const Eigen::Vector2d& target);
    void release_grab(int id);
    void release_all_grabs();
    int grab_count() const { return static_cast<int>(grabs_.size()); }

    // --- solving ---
    // Run up to max_iterations Newton iterations. Returns immediately (and
    // cheaply) if the sheet is already in equilibrium and nothing changed.
    StepInfo step(int max_iterations);
    double residual_tolerance() const;

    // --- the underlying maths, public so tests (and later Python) can check them ---
    double energy(const Eigen::VectorXd& x) const;
    Eigen::VectorXd gradient(const Eigen::VectorXd& x) const;
    // project_psd=false gives the exact Hessian (for finite-difference checks);
    // true clips each element to positive semi-definite (Newton's fallback).
    SparseMatrix hessian(const Eigen::VectorXd& x, bool project_psd) const;

    // Smallest current/rest area ratio J over all triangles. 1 = undeformed,
    // < 1 compressed, > 1 stretched, <= 0 would mean an inverted element.
    double min_area_ratio() const;
    // Largest area ratio J (the most stretched triangle).
    double max_area_ratio() const;

private:
    struct Element {
        std::array<int, 3> v;                // vertex indices
        Eigen::Matrix<double, 3, 2> grad_n;  // row a = gradient of shape function N_a (1/mm)
        double area;                         // rest area, mm^2
    };
    struct Grab {
        int element;
        Eigen::Vector3d bary;  // barycentric weights of the grabbed point in that element
        Eigen::Vector2d target;
    };

    Eigen::Matrix2d deformation_gradient(const Element& e, const Eigen::VectorXd& x) const;
    Eigen::Vector2d grab_point(const Grab& g, const Eigen::VectorXd& x) const;
    void apply_pins();
    // Fixed dofs: drop their rows/columns and put 1 on the diagonal, so the
    // Newton step for them is exactly zero.
    SparseMatrix constrain(const SparseMatrix& H) const;
    double max_node_force(const Eigen::VectorXd& g) const;

    TriMesh mesh_;
    TissueParams params_;
    std::vector<Element> elements_;
    Eigen::VectorXd rest_;       // X, the rest positions as a dof vector
    Eigen::VectorXd x_;          // current positions
    Eigen::VectorXd node_area_;  // lumped area per node (1/3 of each adjacent triangle)
    std::vector<int> boundary_nodes_;
    std::vector<char> fixed_;    // per dof: 1 = pinned (char, not bool: see .cpp)
    std::map<int, Grab> grabs_;
    double mean_edge_ = 1.0;     // mm, sets the step clamp and force scale

    bool converged_ = false;
    StepInfo last_info_;
};

}  // namespace skinsim
