// All tissue parameters in one place.
//
// The solver reads this struct every iteration, so it may be changed at any
// time (e.g. from a UI slider between frames). The simulation then simply
// relaxes to the new equilibrium; nothing needs to be rebuilt.
//
// Units: lengths in mm, forces in N, stresses/moduli in MPa (= N/mm^2).
// 1 MPa = 1000 kPa. Energies come out in N*mm (= mJ).
#pragma once

namespace skinsim {

struct TissueParams {
    // mu: initial shear modulus of the dermis. Literature values for skin span
    // roughly 5-200 kPa depending on site, age, and test method. 50 kPa is a
    // middle-of-the-road starting point, not a measured value.
    double shear_modulus = 0.05;  // MPa (= 50 kPa)

    // Mechanically active thickness (dermis). Membrane forces scale with mu * thickness.
    double thickness = 2.0;  // mm

    // Stiffness of the attachment to the underlying base, per unit of skin
    // area: force per area per displacement. The ratio sqrt(mu*thickness / tether)
    // is the distance over which a local pull spreads out
    // (defaults: sqrt(0.05*2 / 0.001) = 10 mm).
    double tether_stiffness = 0.001;  // N/mm^3 (= 1 kPa/mm)

    // Stiffness of the "finger" spring pulling a grabbed point to the cursor.
    // Stiffer = the skin follows the cursor more exactly.
    double grab_stiffness = 5.0;  // N/mm

    // Hold the outer edge of the sheet fixed (the sheet is a window into a
    // larger area of skin, which continues beyond the edges).
    bool pin_boundary = true;
};

}  // namespace skinsim
