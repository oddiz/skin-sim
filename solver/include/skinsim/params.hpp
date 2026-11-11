// Simulation parameters, grouped in plain structs.
//
// The solver reads these every iteration, so they may be changed at any time
// (e.g. from a UI slider between frames). The simulation then simply relaxes
// to the new equilibrium; nothing needs to be rebuilt.
//
// Units: lengths in mm, forces in N, stresses/moduli in MPa (= N/mm^2).
// 1 MPa = 1000 kPa. Energies come out in N*mm (= mJ).
//
// Default values are plausible orders of magnitude for adult skin, chosen for
// a good feel. They are NOT calibrated against measurements yet.
#pragma once

#include <limits>

namespace skinsim {

// Properties of the tissue itself: what a site preset (face, back...) describes.
struct TissueParams {
    // mu: initial (small-strain) shear modulus of the dermis. Literature values
    // span roughly 5-200 kPa depending on site, age and test method.
    double shear_modulus = 0.05;  // MPa (= 50 kPa)

    // b: strain-stiffening rate (dimensionless). 0 = Neo-Hookean (no lock-up).
    // Higher = the J-shaped curve turns up earlier. At b = 8, skin pulled to
    // 30% stretch is ~6x stiffer than at rest, and ~100x stiffer at 50%.
    double stiffening = 8.0;

    // Mechanically active thickness (dermis). Membrane forces scale with mu * thickness.
    double thickness = 2.0;  // mm

    // Stiffness of the attachment to the underlying base, per unit of skin
    // area: force per area per displacement. sqrt(mu*thickness / tether) is the
    // distance over which a local pull spreads out
    // (defaults: sqrt(0.05*2 / 0.001) = 10 mm). Undermining lowers this (M6).
    double tether_stiffness = 0.001;  // N/mm^3 (= 1 kPa/mm)

    // Hold the outer edge of the sheet fixed (the sheet is a window into a
    // larger area of skin, which continues beyond the edges).
    bool pin_boundary = true;
};

// How the user's "finger" (forceps / skin hook) acts on the skin.
struct GrabParams {
    // The pull is spread over the skin within this radius of the grab point,
    // like the jaws of forceps. A pure point force would cause infinite stress
    // at that point in a continuum, so results would never converge as the mesh
    // is refined. Applies to grabs made after the change.
    double radius = 3.0;  // mm

    // Spring between the cursor and the grabbed skin. Stiffer = the skin
    // follows the cursor more exactly (until max_force is reached).
    double stiffness = 5.0;  // N/mm

    // Maximum pulling force. Beyond it, the finger pulls with constant force and
    // the skin lags behind the cursor: a surgeon's pull is finite too.
    double max_force = 5.0;  // N (use infinity for "unlimited")
};

// Sutures: each is a stiff spring of zero rest length between two points of
// the skin. Stiff, so a tied suture holds the edges (almost) together; the
// force it carries is the tension on the closure at that point.
struct SutureParams {
    // 20 N/mm: at a typical 1 N closure tension, the edges stay 0.05 mm apart.
    double stiffness = 20.0;  // N/mm
};

}  // namespace skinsim
