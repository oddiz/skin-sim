// Excision designs and lesions.
//
// A lesion (a naevus, a skin cancer) is excised by cutting all round it with
// a margin of normal skin and removing the island (see remove_islands in
// mesher.hpp). The defect is then closed, here by sutures.
//
// The classic design is the fusiform (elliptical) excision: a spindle about
// three times as long as it is wide, with tips of about 30 degrees, its long
// axis along the relaxed skin tension lines (RSTL). A round defect closed
// directly would leave a "dog-ear" (a pucker of excess skin) at each end; the
// tapered ends spread that excess out. Shorter, blunter ellipses leave more.
#pragma once

#include <Eigen/Core>

#include "skinsim/cuts.hpp"

namespace skinsim {

// Outline of a fusiform excision from tip a to tip b, as a closed polyline
// (the last point repeats the first), points about `spacing` mm apart.
// length_to_width: length / width (3 is classic). apex_angle_deg: the angle
// at each tip (30 is classic).
//
// The half-width along the axis, as a fraction f of the maximum, is a cubic
// in the distance t from the centre (0 at the centre, 1 at a tip):
//   f(t) = 1 + (m - 3) t^2 + (2 - m) t^3,
// chosen so f(0) = 1 with zero slope (rounded middle), f(1) = 0 (the tip),
// and the slope at the tip gives the requested tip angle: m = tan(apex / 2)
// x length_to_width. The sides bulge in the middle and run nearly straight
// into the tips, like the drawings in surgical textbooks.
Polyline fusiform_outline(const Eigen::Vector2d& a, const Eigen::Vector2d& b, double length_to_width,
                          double apex_angle_deg, double spacing = 0.5);

}  // namespace skinsim
