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

    // --- Collagen fibres along the relaxed skin tension lines (RSTL), GOH model.
    // The defaults here switch them OFF (k1 = 0), so the solver's isotropic
    // behaviour stays exactly as tested; the app's defaults and presets turn
    // them on. See material.hpp for the formulas.
    double fibre_angle = 0.0;         // RSTL direction, degrees anticlockwise from the x axis
    double fibre_stiffness = 0.0;     // k1, MPa
    double fibre_nonlinearity = 0.0;  // k2, dimensionless
    double fibre_dispersion = 0.0;    // kappa, 0 (aligned) .. 1/3 (isotropic)

    // --- In-vivo prestretch. Living skin is stretched beyond its natural,
    // unloaded size, which is why incisions gape and excised skin shrinks.
    // The rest mesh is the in-vivo state; the natural state is smaller by
    // these factors along and across the RSTL. 1 = no prestress (default).
    double prestretch_along = 1.0;
    double prestretch_across = 1.0;

    // --- Undermining: fraction of the tether stiffness left where the skin
    // has been undermined (freed from the underlying tissue with the brush).
    double undermined_tether = 0.05;

    // --- Wrinkling (tension-field theory, see wrinkling.hpp): how much of
    // its resistance to in-plane compression the skin loses by buckling out
    // of the plane. 0 = flat membrane (squeezed skin pushes back as a solid
    // block would; the default here, so the solver's tested behaviour is
    // unchanged); 0.9 = buckled skin keeps 10% (its bending stiffness and
    // the tissue under it still resist a little). Capped at 0.99: the
    // remainder keeps elements from being squashed flat or turned inside out.
    // Tension is never affected.
    double wrinkling = 0.0;
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

// Sutures: each is a thread from an entry point to an exit point, tied to a
// certain length. When the two points are farther apart than that, the
// thread pulls them together like a stiff spring; when closer, it is slack
// (a thread cannot push). The force it carries is the tension on the closure.
struct SutureParams {
    // Stiffness of the thread plus the tissue it bites into. 20 N/mm: at a
    // typical 1 N tension, the thread is only 0.05 mm longer than tied.
    double stiffness = 20.0;  // N/mm

    // Width of the strip of dermis each bite pulls on (the bite runs from the
    // needle hole to the wound edge). A real thread is ~0.15 mm across, but the
    // dermis around it shares the load, and a 2D membrane cannot resolve a
    // thread anyway. A finite width keeps the stress finite and, because the
    // load is integrated over the elements, independent of the mesh.
    double strip_width = 2.0;  // mm

    // Sutures snap when their thread is pulled harder than its knot can hold
    // (see thread_strength). Off by default in the solver, so the tested
    // behaviour is unchanged; the app turns it on.
    bool can_break = false;
};

// Breaking strength of a tied thread ("knot-pull tensile strength"), in N, by
// USP size: 7 means 7-0 (finest), 2 means 2-0. Returns 0 for any other size,
// which means "ideal thread, never breaks".
//
// Values: the minimum average knot-pull strength the US Pharmacopeia requires
// of sterile Class I non-absorbable sutures (silk, monofilament nylon or
// polypropylene), USP <881> / "Nonabsorbable Surgical Suture", Table 1.
// Real threads of good brands are typically somewhat stronger, so a suture
// here snaps a little early rather than late. The knot is the weak point of
// a thread, which is why knot-pull (not straight-pull) strength is used.
//
// Thread tension vs closing force: a simple interrupted suture is a loop that
// crosses the wound twice (once deep in the dermis, once over the top with
// the knot), so the force pulling the edges together is 2 x the tension in
// the thread. The snap test compares tension = closing force / 2 with this.
inline double thread_strength(int usp_size) {
    switch (usp_size) {
        case 7: return 1.08;
        case 6: return 1.96;
        case 5: return 3.92;
        case 4: return 5.88;
        case 3: return 9.41;
        case 2: return 14.1;
        default: return 0.0;
    }
}

// Contact between incision lips: the two sides of a wound (or a flap and the
// edge it is inset into) press against each other instead of passing through.
// Modelled as a penalty: a stiff spring that switches on only where the lips
// overlap, so the overlap is about (contact pressure / stiffness).
struct ContactParams {
    bool enabled = true;

    // Pressure per mm of overlap. At 10 N/mm^3 a contact pressure of 100 kPa
    // (0.1 MPa) leaves 0.01 mm of overlap: invisible, yet soft enough to keep
    // the stiffness matrix well conditioned.
    double stiffness = 50.0;  // N/mm^3 (= MPa/mm)
};

}  // namespace skinsim
