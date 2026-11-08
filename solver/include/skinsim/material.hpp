// Constitutive law: incompressible, strain-stiffening membrane in plane stress.
//
// Inputs are the 2x2 deformation gradient F of a triangle (how its edges map
// from rest to current shape), the shear modulus mu (MPa) and the stiffening
// rate b (dimensionless).
//
// Derivation in brief:
//   Skin is (nearly) incompressible: l1 l2 l3 = 1 for the three principal
//   stretches. In a thin sheet with nothing pushing on its faces (plane
//   stress), the thickness stretch is whatever incompressibility demands:
//   l3 = 1 / J, with J = det F the in-plane area ratio. Hence
//        Q(F) = l1^2 + l2^2 + l3^2 - 3 = tr(F^T F) + 1/J^2 - 3      (Q >= 0, = 0 at rest)
//
//   Neo-Hookean (b = 0):          W = mu/2 Q
//   Exponential (b > 0):          W = mu/(2b) (exp(b Q) - 1)
//   The exponential form (Demiray 1972, widely used for soft tissue) behaves
//   like Neo-Hookean for small strains and stiffens steeply as the tissue is
//   stretched, mimicking collagen fibres straightening and taking up load:
//   the J-shaped stress-strain curve of skin. b sets how early it "locks".
//   As b -> 0 it reduces exactly to Neo-Hookean.
//
//   1/J^2 -> infinity as a triangle is squashed to zero area, so inverting an
//   element is impossible for any step that decreases energy.
//
// Derivatives with respect to F (needed by Newton's method):
//   P_nh = mu ( F - J^-3 cof(F) )                    Neo-Hookean stress
//   P    = dW/dF = exp(bQ) P_nh                      (first Piola-Kirchhoff stress)
//   d2W/dF2 = exp(bQ) ( K_nh + (2b/mu) p p^T ),      p = vec(P_nh)
//   K_nh = mu ( I + 3 J^-4 c c^T - J^-3 Hc )
// where cof(F) = J F^-T is the cofactor matrix, c = vec(cof F), and Hc is the
// constant 4x4 matrix with dJ/dvecF = Hc vecF = c (in 2D, J is quadratic in F).
// vec() stacks a matrix column by column: vec(F) = [F00, F10, F01, F11].
// The extra term (2b/mu) p p^T is positive semi-definite: stiffening never
// destabilises the solver.
#pragma once

#include <Eigen/Core>

namespace skinsim {
namespace membrane {

using Vec4 = Eigen::Matrix<double, 4, 1>;
using Mat4 = Eigen::Matrix<double, 4, 4>;

// Q = tr(F^T F) + 1/J^2 - 3, the incompressible first invariant minus 3.
// Returns +infinity if J <= 0.
double invariant_q(const Eigen::Matrix2d& F);

// Strain energy per unit reference volume (MPa = N/mm^2 = N*mm/mm^3).
// Returns +infinity if J <= 0 (inverted or collapsed element) or on overflow.
double energy_density(const Eigen::Matrix2d& F, double mu, double b);

// First Piola-Kirchhoff stress P = dW/dF. Only meaningful for J > 0.
Eigen::Matrix2d first_piola(const Eigen::Matrix2d& F, double mu, double b);

// Second derivative d2W/(dvecF dvecF), 4x4, symmetric. Only for J > 0.
Mat4 stiffness(const Eigen::Matrix2d& F, double mu, double b);

// Closest positive semi-definite matrix: eigen-decompose and clip negative
// eigenvalues to zero. Newton's fallback when the exact Hessian is indefinite.
Mat4 project_psd(const Mat4& K);

}  // namespace membrane
}  // namespace skinsim
