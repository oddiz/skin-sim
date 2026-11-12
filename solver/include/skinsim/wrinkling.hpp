// Wrinkling: tension-field theory for the skin membrane.
//
// Why: a membrane has no bending stiffness, so in reality it cannot carry
// compression. Push the ends of a sheet of paper together and it does not
// shorten in its plane: it buckles out of the plane (wrinkles, or in skin, a
// dog-ear). Our 2D model has no "out of the plane", so a flat membrane
// squeezed in-plane resists like a solid block: it overestimates compressive
// stress (and the force needed to close a defect against it).
//
// Tension-field theory (Wagner 1929, Pipkin 1986) replaces the energy W by the
// "relaxed" energy: the lowest energy the skin can reach by hiding some extra
// length in fine wrinkles. Wrinkles can only ADD material length to a
// direction (the skin lying in folds is longer than the straight distance it
// spans), never remove it, so in terms of the metric C = F^T F (C measures
// squared lengths: |dx|^2 = dX^T C dX)
//
//     W_rel(C) = min over D >= 0 of W(C + D),
//
// where "D >= 0" means D is a positive semi-definite matrix (adds length in
// some directions, removes it in none) and C* = C + D is the "effective"
// metric the material actually feels. Three cases come out of this:
//   taut:      stress is tensile in every direction -> D = 0, W_rel = W.
//   wrinkled:  tension one way, compression the other -> D = w w^T: extra
//              length along one direction w only, chosen so the stress along
//              w is exactly zero (wrinkles carry no load across them).
//   slack:     compressed both ways (the skin is shorter than its natural
//              size in every direction) -> it simply lies in folds,
//              W_rel = 0, no stress at all.
//
// Our W (Neo-Hookean/Demiray matrix plus GOH fibres) is convex in C, so this
// is a convex minimisation: the solution is unique and easy to find. Two
// bonuses follow from convexity theory:
//   - The relaxed stress is the stress of the effective state (the
//     "envelope theorem": the optimal D does not move to first order).
//   - The relaxed stress is never compressive, and the relaxed stiffness is
//     positive semi-definite: wrinkled skin never destabilises Newton.
//
// Inputs and outputs follow material.hpp: F is the 2x2 elastic deformation
// gradient, energy per unit natural volume, P = dW/dF, K = d2W/dF2 (vec order).
// With a prestretch, call these with the elastic part F_e = F G, as the
// Simulation does for the plain material (the cone D >= 0 maps onto itself).
#pragma once

#include <Eigen/Core>

#include "skinsim/material.hpp"

namespace skinsim {
namespace membrane {

// The tissue's constitutive parameters in one place.
struct Material {
    double mu = 0.05;  // shear modulus, MPa
    double b = 0.0;    // exponential stiffening
    Fibres fibres;
};

enum class MembraneState { Taut, Wrinkled, Slack };

// The relaxed state of one element.
struct RelaxedState {
    MembraneState state = MembraneState::Taut;
    Eigen::Matrix2d C = Eigen::Matrix2d::Identity();      // metric F^T F
    Eigen::Matrix2d C_eff = Eigen::Matrix2d::Identity();  // C + D, what the material feels
    Eigen::Vector2d w = Eigen::Vector2d::Zero();          // wrinkled: D = w w^T
};

// Solve the relaxation for one deformation. Only meaningful for det F > 0.
RelaxedState relax(const Eigen::Matrix2d& F, const Material& m);

// Relaxed energy density; +infinity if det F <= 0 (like energy_density).
double relaxed_energy_density(const Eigen::Matrix2d& F, const Material& m);
// Relaxed first Piola-Kirchhoff stress, dW_rel/dF.
Eigen::Matrix2d relaxed_piola(const Eigen::Matrix2d& F, const Material& m);
// Relaxed stiffness d2W_rel/dF2, 4x4 symmetric, positive semi-definite.
Mat4 relaxed_stiffness(const Eigen::Matrix2d& F, const Material& m);

// How much extra skin is bunched up in wrinkles: the largest relative excess
// of material length over the distance it spans, in any direction
// (0 = none, 0.1 = 10% more skin than room for it). This is where the real
// skin would pucker (dog-ears).
double excess_length(const RelaxedState& s);

// The unrelaxed material written in terms of C (used by the relaxation and
// exposed for tests): W(C), dW/dC (symmetric 2x2) and d2W/dC2 (4x4 in vec
// order, symmetrised so it only acts on the symmetric part of a change of C).
struct MetricDerivatives {
    double W = 0.0;
    Eigen::Matrix2d S = Eigen::Matrix2d::Zero();  // dW/dC, half the 2nd Piola-Kirchhoff stress
    Mat4 K = Mat4::Zero();
};
MetricDerivatives metric_derivatives(const Eigen::Matrix2d& C, const Material& m, bool with_hessian = true);

}  // namespace membrane
}  // namespace skinsim
