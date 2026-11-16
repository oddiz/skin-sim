// Local flap designs: the excision around a lesion and the flap(s) that
// close it, drawn together, in rest (tissue) coordinates.
//
// A local flap moves skin next to a defect into it, keeping it attached by
// its base (the pedicle, which carries its blood supply). Designs are
// grouped by how the skin moves:
//
// ADVANCEMENT: the skin slides straight forward.
//   Advancement     (U-flap) square defect; two parallel incisions run back
//                   from its corners. Length : width is the main choice.
//                   Optional Burow's triangles at the base take up the extra
//                   length of the outer lips (no dog-ears).
//   H-plasty        bilateral advancement: two such flaps from opposite
//                   sides, each advancing half the defect.
//   A-T plasty      triangular defect; its base is extended both ways; the
//                   two sides slide together and the scar is a T.
//   V-Y island      a triangle of skin cut free all round but left on its
//                   subcutaneous pedicle (here: its tethering, not
//                   undermined) slides into a round defect; the V closes
//                   behind it as a Y.
//   Keystone        (Behan) a curved trapezoid beside a fusiform defect, also
//                   an island on its pedicle, with V-Y closures at both ends.
// ROTATION: the skin swings around a pivot.
//   Rotation        triangular defect; an arc centred on its apex; the
//                   sector rotates by the apex angle.
//   O-Z             round defect; two opposite arcs, two rotation flaps that
//                   each fill half; the scar is an S (a Z).
// TRANSPOSITION: the skin is lifted over a bridge of intact skin.
//   Rhomboid        (Limberg) 60 / 120 degree rhombus; an identical rhombus
//                   beside it swings 60 degrees about the far end of its base.
//   Dufourmentel    any rhombus (60-90 degrees); the first limb bisects the
//                   extended short diagonal and the extended side, the
//                   second runs parallel to the long diagonal: a broader base
//                   and a smaller turn than Limberg's.
//   Bilobed         (Zitelli) round defect; two lobes on arcs around a pivot
//                   one radius from the defect, 45 degrees apart: the first
//                   fills the defect, the second the first's donor site, and
//                   the second's donor site closes in a line. A Burow's
//                   triangle between the defect and the pivot.
//   Z-plasty        no defect: a Z of three equal limbs; the two triangular
//                   flaps swap places, lengthening the central limb (a scar
//                   contracture) and turning it about 90 degrees.
// (Interpolation flaps, e.g. paramedian forehead or nasolabial, pass their
// pedicle over intact skin: that needs a third dimension, so they are not
// designed here.)
//
// The design is pure geometry. Cutting, removing the defect (and any Burow's
// triangles) and undermining are done by Sheet::make_flap.
#pragma once

#include <utility>
#include <vector>

#include <Eigen/Core>

#include "skinsim/cuts.hpp"

namespace skinsim {

// Stored values: never renumber (saved scenarios use them).
enum class FlapType {
    Advancement = 0,
    Rotation = 1,
    Rhomboid = 2,
    HPlasty = 3,
    ATPlasty = 4,
    VYIsland = 5,
    Keystone = 6,
    OZ = 7,
    Dufourmentel = 8,
    Bilobed = 9,
    ZPlasty = 10,
};
constexpr int kFlapTypeCount = 11;

struct FlapShape {
    // Advancement, H-plasty: length of each flap over its width (the
    // defect's width). V-Y: length of the V over the defect's diameter.
    double length_to_width = 3.0;
    // Rotation, O-Z: angle swept by the arc(s), degrees. Textbooks draw a
    // rotation arc about four times the defect's width long (~110 degrees).
    double arc_angle_deg = 110.0;
    // Build the flap on the other side (mirror image).
    bool mirror = false;
    // Advancement, H-plasty: excise Burow's triangles at the flaps' bases.
    bool burow = false;
    // Dufourmentel: the rhombus' acute angle, degrees (60 = Limberg's).
    double rhombus_angle_deg = 70.0;
    // Z-plasty: angle between the lateral limbs and the central one, degrees
    // (30 / 45 / 60 / 75 / 90 lengthen by about 25 / 50 / 75 / 100 / 125%).
    double z_angle_deg = 60.0;
};

struct FlapDesign {
    FlapType type = FlapType::Advancement;
    bool valid = false;
    Eigen::Vector2d centre = Eigen::Vector2d::Zero();  // of the lesion / defect
    double defect_diameter = 0.0;   // the circle (lesion + margins) the defect contains, mm
    Polyline defect;                // closed outline, cut and removed (empty: Z-plasty)
    std::vector<Polyline> burow;    // more closed outlines cut and removed (Burow's triangles)
    std::vector<Polyline> incisions;  // the flaps' own cuts (open polylines)
    std::vector<Polyline> flaps;    // closed outline of each flap (for drawing, undermining)
    // Island flaps (V-Y, keystone): cut free all round, but they live on the
    // tissue beneath them (their pedicle), so they are not undermined.
    std::vector<Polyline> islands;
    std::vector<Polyline> bases;    // the uncut edge(s) the flaps stay attached by
    Eigen::Vector2d pivot = Eigen::Vector2d::Zero();  // rotation / transposition centre (first flap)
    double turn_deg = 0.0;          // how far the flap turns into place (0 = advancement)
    // Advancement: flap length / width. Rotation, O-Z: arc length / defect
    // width (textbooks aim for about 4 for a rotation). Rhomboid: 1.
    double length_to_width = 0.0;
    // Z-plasty: theoretical gain in length along the central limb (0.75 = 75%).
    double lengthening = 0.0;
    // Key sutures: (one needle hole, the other), each a bite away from the
    // cuts. Tied, they pull the flaps into place.
    std::vector<std::pair<Eigen::Vector2d, Eigen::Vector2d>> key_sutures;
};

// Design a flap for a round defect of the given diameter centred at `centre`,
// with the flap harvested in direction `direction` (from the defect towards
// the flap; need not be normalized). Z-plasty: the central limb runs from
// `centre` along `direction`, as long as `direction` (min 4 mm); the
// diameter is ignored. `bite` is the distance of the key
// sutures' needle holes from the cuts, mm. Returns valid = false for a
// degenerate request (zero size or direction).
FlapDesign design_flap(FlapType type, const Eigen::Vector2d& centre, const Eigen::Vector2d& direction,
                       double defect_diameter, const FlapShape& shape = FlapShape{}, double bite = 2.0);

// Is p inside the closed polygon (even-odd rule)? For tests and undermining.
bool inside_polygon(const Polyline& polygon, const Eigen::Vector2d& p);

}  // namespace skinsim
