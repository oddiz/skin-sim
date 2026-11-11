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

// ---------------------------------------------------------------------------
// Collagen fibres: Gasser-Ogden-Holzapfel (GOH) model, one fibre family.
//
// Dermal collagen is not random: it is preferentially aligned, which is why
// skin is stiffer in some directions than others and why incisions along the
// relaxed skin tension lines (RSTL) behave differently from those across.
// The GOH model adds, on top of the isotropic matrix above,
//
//   W_f = k1/(2 k2) (exp(k2 <E>^2) - 1)
//   E   = kappa (I1 - 3) + (1 - 3 kappa)(I4 - 1)
//
//   I4 = |F a|^2       squared stretch of the fibre direction a (unit vector)
//   I1 = Q + 3         as above (includes the thickness stretch 1/J)
//   kappa in [0, 1/3]  dispersion: 0 = all fibres exactly along a,
//                      1/3 = fibres in every direction (isotropic)
//   <E> = max(E, 0)    fibres are wavy threads: they resist stretching but
//                      buckle instead of carrying compression.
//   k1 (MPa)           fibre stiffness; k2 (-) how steeply fibres stiffen.
//                      As k2 -> 0, W_f -> k1/2 <E>^2.
//
// Derivatives, with dE/dF = 2 kappa (F - J^-3 cof F) + 2 (1 - 3 kappa) F a a^T:
//   P_f = psi'(E) dE/dF,                 psi'  = k1 E exp(k2 E^2)
//   K_f = psi'' vec(dE) vec(dE)^T + psi' d2E/dF2,
//                                        psi'' = k1 (1 + 2 k2 E^2) exp(k2 E^2)
//   d2E/dF2 = kappa d2I1 + (1 - 3 kappa) 2 (a a^T (x) I),
//   d2I1    = 2 (I + 3 J^-4 c c^T - J^-3 Hc)       (same pieces as Neo-Hookean)
// All zero while the fibres are slack (E <= 0).
// ---------------------------------------------------------------------------
struct Fibres {
    double k1 = 0.0;     // MPa; 0 = no fibres
    double k2 = 0.0;     // dimensionless
    double kappa = 0.0;  // dispersion, 0..1/3
    Eigen::Vector2d direction = Eigen::Vector2d(1.0, 0.0);  // unit vector (natural configuration)
};

double fibre_energy_density(const Eigen::Matrix2d& F, const Fibres& f);
Eigen::Matrix2d fibre_piola(const Eigen::Matrix2d& F, const Fibres& f);
Mat4 fibre_stiffness(const Eigen::Matrix2d& F, const Fibres& f);

}  // namespace membrane
}  // namespace skinsim
