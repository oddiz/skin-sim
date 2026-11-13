// Local flap designs: the excision around a lesion and the flap that closes
// it, drawn together, in rest (tissue) coordinates.
//
// A local flap moves skin next to a defect into it, keeping it attached by
// its base (the pedicle, which carries its blood supply). The three classic
// designs differ in how the skin moves:
//
//   Advancement  the flap slides straight forward into the defect. Square
//                defect; two parallel incisions run back from its corners.
//                Length : width is the main design choice (longer = easier
//                to advance, but a long narrow flap is less well perfused).
//
//   Rotation     the flap swings around a pivot into a triangular defect.
//                An arc of a circle, centred on the triangle's apex, starts
//                at one base corner; the sector it encloses rotates by the
//                triangle's apex angle, so its edge lands on the other side
//                of the triangle. A longer arc spreads the closing tension.
//
//   Rhomboid     (Limberg) the flap jumps over a narrow bridge of skin
//                ("transposition"). The defect is a rhombus with 60 and 120
//                degree angles; the flap is an identical rhombus beside it,
//                whose first limb continues the short diagonal, and which
//                swings 60 degrees about the far end of its base.
//
// The design is pure geometry. Cutting, removing the defect and undermining
// are done by Sheet::make_flap.
#pragma once

#include <utility>
#include <vector>

#include <Eigen/Core>

#include "skinsim/cuts.hpp"

namespace skinsim {

enum class FlapType { Advancement = 0, Rotation = 1, Rhomboid = 2 };

struct FlapShape {
    // Advancement: length of the flap over its width (the defect's width).
    double length_to_width = 3.0;
    // Rotation: angle swept by the arc, degrees. Textbooks draw arcs about
    // four times the defect's width long, about 110 degrees here.
    double arc_angle_deg = 110.0;
    // Rotation and rhomboid: build the flap on the other side (mirror image).
    bool mirror = false;
};

struct FlapDesign {
    FlapType type = FlapType::Advancement;
    bool valid = false;
    Eigen::Vector2d centre = Eigen::Vector2d::Zero();  // of the lesion / defect
    double defect_diameter = 0.0;   // the circle (lesion + margins) the defect contains, mm
    Polyline defect;                // closed outline, cut and removed
    std::vector<Polyline> incisions;  // the flap's own cuts (open polylines)
    Polyline flap;                  // closed outline of the flap (for drawing, undermining)
    Polyline base;                  // the uncut edge the flap stays attached by
    Eigen::Vector2d pivot = Eigen::Vector2d::Zero();  // rotation / transposition centre
    double turn_deg = 0.0;          // how far the flap turns into place (0 = advancement)
    // Advancement: flap length / width. Rotation: arc length / defect width
    // (textbooks aim for about 4). Rhomboid: 1 (the flap copies the defect).
    double length_to_width = 0.0;
    // Key sutures: (entry on the flap, exit on the far side of the defect),
    // a bite away from the cuts. Tied, they pull the flap into place.
    std::vector<std::pair<Eigen::Vector2d, Eigen::Vector2d>> key_sutures;
};

// Design a flap for a round defect of the given diameter centred at `centre`,
// with the flap harvested in direction `direction` (from the defect towards
// the flap; need not be normalized). `bite` is the distance of the key
// sutures' needle holes from the cuts, mm. Returns valid = false for a
// degenerate request (zero size or direction).
FlapDesign design_flap(FlapType type, const Eigen::Vector2d& centre, const Eigen::Vector2d& direction,
                       double defect_diameter, const FlapShape& shape = FlapShape{}, double bite = 2.0);

// Is p inside the closed polygon (even-odd rule)? For tests and undermining.
bool inside_polygon(const Polyline& polygon, const Eigen::Vector2d& p);

}  // namespace skinsim
