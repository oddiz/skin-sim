// Constitutive law: incompressible Neo-Hookean membrane in plane stress.
//
// Inputs are the 2x2 deformation gradient F of a triangle (how its edges map
// from rest to current shape) and the shear modulus mu (MPa).
//
// Derivation in brief:
//   3D incompressible Neo-Hookean:  W = mu/2 (l1^2 + l2^2 + l3^2 - 3),  l1 l2 l3 = 1
//   Thin sheet, nothing pushing on its faces (plane stress) -> the thickness
//   stretch is whatever incompressibility demands: l3 = 1 / J, with J = det F.
//   So, per unit *reference* volume:
//        W(F) = mu/2 ( tr(F^T F) + 1/J^2 - 3 )
//   1/J^2 -> infinity as a triangle is squashed to zero area, which makes
//   inverting an element impossible for any step that decreases energy.
//
// Derivatives with respect to F (needed by Newton's method):
//   P = dW/dF  = mu ( F - J^-3 cof(F) )         (first Piola-Kirchhoff stress)
//   d2W/dF2    = mu ( I + 3 J^-4 c c^T - J^-3 Hc )
// where cof(F) = J F^-T is the cofactor matrix, c = vec(cof F), and Hc is the
// constant 4x4 matrix with dJ/dvecF = Hc vecF = c (in 2D, J is quadratic in F).
// vec() stacks a matrix column by column: vec(F) = [F00, F10, F01, F11].
#pragma once

#include <Eigen/Core>

namespace skinsim {
namespace membrane_nh {

using Vec4 = Eigen::Matrix<double, 4, 1>;
using Mat4 = Eigen::Matrix<double, 4, 4>;

// Strain energy per unit reference volume (MPa = N/mm^2 = N*mm/mm^3).
// Returns +infinity if J <= 0 (inverted or collapsed element).
double energy_density(const Eigen::Matrix2d& F, double mu);

// First Piola-Kirchhoff stress P = dW/dF. Only meaningful for J > 0.
Eigen::Matrix2d first_piola(const Eigen::Matrix2d& F, double mu);

// Second derivative d2W/(dvecF dvecF), 4x4, symmetric. Only for J > 0.
Mat4 stiffness(const Eigen::Matrix2d& F, double mu);

// Closest positive semi-definite matrix: eigen-decompose and clip negative
// eigenvalues to zero. Used so every element pushes Newton *downhill*.
Mat4 project_psd(const Mat4& K);

}  // namespace membrane_nh
}  // namespace skinsim
